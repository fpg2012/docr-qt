//! Document: xref parsing, lazily loaded objects, incremental saving.

use std::collections::{HashMap, HashSet};
use std::fs;
use std::io::{BufWriter, Write};
use std::path::Path;

use crate::error::{Error, Result};
use crate::lexer::{Keyword, TokenKind};
use crate::modification::Modification;
use crate::object::{Dictionary, IndirectObject, Object, Ref, Stream};
use crate::parser::Parser;
use crate::serialize;
use crate::source::Source;
use crate::util::{find_subslice, rfind_subslice};
use crate::xref::{XRef, XrefEntry};

/// Guard against absurd object numbers in damaged files.
const MAX_OBJECTS: usize = 5_000_000;

/// Initial window used when parsing an object; doubled on truncation.
const OBJECT_WINDOW: usize = 8 * 1024;
/// Initial window used when parsing an xref table.
const XREF_WINDOW: usize = 64 * 1024;

/// A parsed PDF document.
///
/// Objects are parsed lazily on first access and cached.  A document opened
/// from a path reads windows from the file on demand (like the C version's
/// `fseek`); `Document::parse` keeps a caller-provided buffer in memory.
/// Borrowed access (`object`, `resolve`, `catalog`) avoids copies; clone the
/// object if you need to hold several at once.
pub struct Document {
    source: Source,
    version: String,
    trailer: Dictionary,
    xref: XRef,
    xref_offset: u64,
    cache: HashMap<u32, IndirectObject>,
    /// Objects currently being loaded; detects self-referential object streams.
    loading: HashSet<u32>,
    modification: Modification,
}

impl Document {
    /// Open a PDF file and read its xref; object data stays on disk until
    /// first access.
    pub fn open(path: impl AsRef<Path>) -> Result<Self> {
        Self::from_source(Source::open(path)?)
    }

    /// Parse a PDF already in memory.
    pub fn parse(data: Vec<u8>) -> Result<Self> {
        Self::from_source(Source::from_bytes(data))
    }

    fn from_source(source: Source) -> Result<Self> {
        let version = detect_version(&source)?;
        let xref_offset = find_startxref(&source)?;

        let mut xref = XRef::new(0);
        let mut trailer = Dictionary::new();
        read_xref_chain(&source, xref_offset, &mut xref, &mut trailer)?;

        Ok(Self {
            source,
            version,
            trailer,
            xref,
            xref_offset,
            cache: HashMap::new(),
            loading: HashSet::new(),
            modification: Modification::new(),
        })
    }

    pub fn version(&self) -> &str {
        &self.version
    }

    /// Length of the underlying source in bytes.
    pub fn source_len(&self) -> u64 {
        self.source.len()
    }

    /// Read the entire source into memory.  For a file-backed document this
    /// defeats lazy loading and is only intended for callers that really need
    /// all bytes (hashing, custom copying, ...).
    pub fn read_all(&self) -> Result<Vec<u8>> {
        self.source.read_all()
    }

    pub fn trailer(&self) -> &Dictionary {
        &self.trailer
    }

    pub fn trailer_mut(&mut self) -> &mut Dictionary {
        &mut self.trailer
    }

    pub fn xref(&self) -> &XRef {
        &self.xref
    }

    pub fn xref_offset(&self) -> u64 {
        self.xref_offset
    }

    pub fn modification(&self) -> &Modification {
        &self.modification
    }

    pub fn modification_mut(&mut self) -> &mut Modification {
        &mut self.modification
    }

    /// True when the trailer declares `/Encrypt`.  The object structure can
    /// still be inspected, but strings/streams are encrypted and filters will
    /// usually fail.
    pub fn is_encrypted(&self) -> bool {
        self.trailer.contains_key("Encrypt")
    }

    pub fn set_modification(&mut self, modification: Modification) {
        self.modification = modification;
    }

    /// Drop the lazy-loading cache.  Useful when memory matters more than speed.
    pub fn clear_cache(&mut self) {
        self.cache.clear();
    }

    /// Load (if needed) and return an object by object number.
    pub fn object(&mut self, num: u32) -> Result<&Object> {
        self.load(num)?;
        Ok(&self.cache.get(&num).expect("object was just loaded").object)
    }

    /// Load (if needed) and return the whole indirect object.
    pub fn indirect_object(&mut self, num: u32) -> Result<&IndirectObject> {
        self.load(num)?;
        Ok(self.cache.get(&num).expect("object was just loaded"))
    }

    /// Follow an indirect reference.
    ///
    /// The C `pindf_deref(doc, obj)` returned a borrowed pointer for an
    /// arbitrary object; that cannot be expressed safely when `obj` is itself
    /// borrowed from the document.  Resolve by `Ref` instead.
    pub fn resolve(&mut self, reference: Ref) -> Result<&Object> {
        self.object(reference.num)
    }

    /// Convenience: follow `/Root` from the trailer.
    pub fn catalog(&mut self) -> Result<&Object> {
        let reference = self
            .trailer
            .get("Root")
            .and_then(Object::as_ref)
            .ok_or_else(|| Error::document("trailer has no indirect /Root"))?;
        self.resolve(reference)
    }

    fn load(&mut self, num: u32) -> Result<()> {
        if self.cache.contains_key(&num) {
            return Ok(());
        }
        // Guard against cycles such as an object stream containing itself.
        if !self.loading.insert(num) {
            return Err(Error::document(format!(
                "cyclic object reference while loading object {num}"
            )));
        }
        let result = self.load_uncached(num);
        self.loading.remove(&num);
        result
    }

    fn load_uncached(&mut self, num: u32) -> Result<()> {
        let entry = self
            .xref
            .get(num)
            .ok_or_else(|| Error::document(format!("object {num} has no xref entry")))?;

        let mut ind = match entry {
            XrefEntry::Free { .. } => return Err(Error::document(format!("object {num} is free"))),
            XrefEntry::Normal { offset, .. } => {
                if offset >= self.source.len() {
                    return Err(Error::document(format!(
                        "object {num} offset {offset} is past the end of the file"
                    )));
                }
                self.parse_indirect_at(offset)?
            }
            XrefEntry::Compressed { stream_num, index } => {
                self.load_compressed(num, stream_num, index)?
            }
        };

        ind.num = num;
        self.cache.insert(num, ind);
        Ok(())
    }

    /// Parse an indirect object at an absolute file offset, growing the read
    /// window until the object is complete.
    fn parse_indirect_at(&self, offset: u64) -> Result<IndirectObject> {
        parse_object_at(&self.source, offset)
    }

    fn load_compressed(&mut self, num: u32, stream_num: u32, index: u32) -> Result<IndirectObject> {
        self.load(stream_num)?;

        // Decode the object stream and parse its header while holding only an
        // immutable borrow of the cache; then release it before inserting.
        let (decoded, first, pairs) = {
            let ind = self.cache.get(&stream_num).expect("loaded above");
            let stream = ind.object.as_stream().ok_or_else(|| {
                Error::document(format!("object {stream_num} is not an object stream"))
            })?;
            if stream.dict.get_name("Type") != Some(b"ObjStm".as_slice()) {
                return Err(Error::document(format!(
                    "object {stream_num} is not an object stream (/Type is not /ObjStm)"
                )));
            }
            if stream.dict.contains_key("Extends") {
                return Err(Error::unsupported("object streams with /Extends"));
            }
            let n = stream
                .dict
                .get_int("N")
                .ok_or_else(|| Error::document("object stream has no /N"))?;
            let first = stream
                .dict
                .get_int("First")
                .ok_or_else(|| Error::document("object stream has no /First"))?;
            if n < 0 || first < 0 {
                return Err(Error::document("negative /N or /First in object stream"));
            }

            let decoded = stream.decode()?;
            if first as usize > decoded.len() {
                return Err(Error::document("object stream /First is past its data"));
            }
            if n as usize > decoded.len() {
                return Err(Error::document("object stream /N is too large"));
            }

            let mut header = Parser::new(&decoded);
            let mut pairs = Vec::with_capacity(n as usize);
            for _ in 0..n {
                let obj_num = header.expect_int()?;
                let offset = header.expect_int()?;
                if obj_num < 0 || offset < 0 {
                    return Err(Error::document(
                        "negative object number or offset in object stream header",
                    ));
                }
                pairs.push((obj_num as u32, offset as u64));
            }
            (decoded, first as usize, pairs)
        };

        let offset = pairs
            .iter()
            .find(|(obj_num, _)| *obj_num == num)
            .map(|(_, offset)| *offset)
            .or_else(|| pairs.get(index as usize).map(|(_, offset)| *offset))
            .ok_or_else(|| {
                Error::document(format!(
                    "object {num} not found in object stream {stream_num}"
                ))
            })?;

        let start = first
            .checked_add(offset as usize)
            .filter(|&start| start <= decoded.len())
            .ok_or_else(|| Error::document("object offset outside object stream"))?;
        let mut parser = Parser::at(&decoded, start);
        let object = parser.parse_object()?;

        Ok(IndirectObject {
            num,
            gen: 0,
            object,
        })
    }

    /// Append the incremental update (new objects + xref + trailer) to `out`.
    ///
    /// `base_offset` is the length of the original file, i.e. the position at
    /// which this data will be appended.  Returns the new `startxref` offset.
    /// When the modification set is empty nothing is written and the existing
    /// xref offset is returned.
    pub fn write_incremental(
        &self,
        out: &mut impl Write,
        base_offset: u64,
        compress_xref: bool,
    ) -> Result<u64> {
        if self.modification.is_empty() {
            return Ok(self.xref_offset);
        }

        let mut buf = Vec::new();
        let mut offsets: Vec<(u32, u64, u16)> = Vec::new();
        for (num, ind) in self.modification.iter() {
            offsets.push((*num, base_offset + buf.len() as u64, ind.gen));
            serialize::write_indirect(&mut buf, ind);
        }
        let xref_offset = base_offset + buf.len() as u64;

        let max_mod = self.modification.max_obj_num().unwrap_or(0) as usize;
        let mut trailer = self.trailer.clone();
        trailer.insert("Prev", Object::Int(self.xref_offset as i64));
        // Keys that only make sense for the previous xref stream.
        for key in ["Filter", "Index", "W", "Type", "DecodeParms", "Length"] {
            trailer.remove(key);
        }

        if !compress_xref {
            let size = (max_mod + 1).max(self.xref.size());
            trailer.insert("Size", Object::Int(size as i64));

            write_xref_table(&mut buf, &offsets);
            buf.extend_from_slice(b"trailer\r\n");
            serialize::write_dictionary(&mut buf, &trailer);
            write_startxref(&mut buf, xref_offset);
        } else {
            let xref_obj_num = max_mod as u32 + 1;
            let size = (max_mod + 1)
                .max(self.xref.size())
                .max(xref_obj_num as usize + 1);
            trailer.insert("Size", Object::Int(size as i64));

            // The xref stream describes itself as well.
            offsets.push((xref_obj_num, xref_offset, 0));
            let (index, rows, offset_width) = build_xref_rows(&offsets);
            let compressed = deflate(&rows)?;

            let mut dict = trailer;
            dict.insert("Type", Object::name("XRef"));
            dict.insert(
                "W",
                Object::Array(vec![
                    Object::Int(1),
                    Object::Int(offset_width as i64),
                    Object::Int(2),
                ]),
            );
            dict.insert("Index", int_array(&index));
            dict.insert("Filter", Object::name("FlateDecode"));
            dict.insert("Length", Object::Int(compressed.len() as i64));

            let ind = IndirectObject {
                num: xref_obj_num,
                gen: 0,
                object: Object::Stream(Stream::new(dict, compressed, 0)),
            };
            serialize::write_indirect(&mut buf, &ind);
            write_startxref(&mut buf, xref_offset);
        }

        out.write_all(&buf)?;
        Ok(xref_offset)
    }

    /// Write the original bytes followed by the incremental update to `path`.
    ///
    /// The source is copied in bounded chunks, so a file-backed document is
    /// never loaded into memory as a whole.
    pub fn save(&self, path: impl AsRef<Path>) -> Result<()> {
        let file = fs::File::create(path)?;
        let mut writer = BufWriter::new(file);
        self.source.copy_to(&mut writer)?;
        self.write_incremental(&mut writer, self.source.len(), true)?;
        writer.flush()?;
        Ok(())
    }
}

fn write_startxref(buf: &mut Vec<u8>, offset: u64) {
    buf.extend_from_slice(b"\r\nstartxref\r\n");
    buf.extend_from_slice(offset.to_string().as_bytes());
    buf.extend_from_slice(b"\r\n%%EOF\n");
}

/// Write one xref subsection per contiguous run of object numbers.
fn write_xref_table(buf: &mut Vec<u8>, offsets: &[(u32, u64, u16)]) {
    buf.extend_from_slice(b"xref\n");
    let mut i = 0;
    while i < offsets.len() {
        let start = i;
        while i + 1 < offsets.len() && offsets[i + 1].0 == offsets[i].0 + 1 {
            i += 1;
        }
        let first = offsets[start].0;
        let len = (i - start + 1) as u32;
        buf.extend_from_slice(format!("{first} {len}\r\n").as_bytes());
        for &(_, offset, gen) in &offsets[start..=i] {
            buf.extend_from_slice(format!("{offset:010} {gen:05} n\r\n").as_bytes());
        }
        i += 1;
    }
}

fn int_array(values: &[(u32, u32)]) -> Object {
    let mut items = Vec::with_capacity(values.len() * 2);
    for &(first, len) in values {
        items.push(Object::Int(first as i64));
        items.push(Object::Int(len as i64));
    }
    Object::Array(items)
}

/// Build `/Index` pairs and the binary rows for an xref stream.
fn build_xref_rows(offsets: &[(u32, u64, u16)]) -> (Vec<(u32, u32)>, Vec<u8>, usize) {
    let max_offset = offsets
        .iter()
        .map(|(_, offset, _)| *offset)
        .max()
        .unwrap_or(0);
    let offset_width = if max_offset > u32::MAX as u64 { 8 } else { 4 };

    let mut index = Vec::new();
    let mut rows = Vec::new();
    let mut i = 0;
    while i < offsets.len() {
        let start = i;
        while i + 1 < offsets.len() && offsets[i + 1].0 == offsets[i].0 + 1 {
            i += 1;
        }
        index.push((offsets[start].0, (i - start + 1) as u32));
        for &(_, offset, gen) in &offsets[start..=i] {
            rows.push(1u8); // type 1: uncompressed object
            for shift in (0..offset_width).rev() {
                rows.push((offset >> (shift * 8)) as u8);
            }
            rows.extend_from_slice(&gen.to_be_bytes());
        }
        i += 1;
    }
    (index, rows, offset_width)
}

fn deflate(data: &[u8]) -> Result<Vec<u8>> {
    use flate2::write::ZlibEncoder;
    use flate2::Compression;

    let mut encoder = ZlibEncoder::new(Vec::new(), Compression::default());
    encoder.write_all(data)?;
    Ok(encoder.finish()?)
}

// ------------------------------------------------------------- xref parsing

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum XrefStart {
    Table,
    Stream,
}

fn read_xref_chain(
    source: &Source,
    start: u64,
    xref: &mut XRef,
    trailer: &mut Dictionary,
) -> Result<()> {
    let mut offset = start;
    let mut seen = HashSet::new();
    let mut first = true;

    while seen.insert(offset) {
        if offset >= source.len() {
            return Err(Error::document(format!(
                "xref offset {offset} is past the end of the file"
            )));
        }

        let prev = match read_xref_start(source, offset)? {
            XrefStart::Table => {
                let (local_xref, local_trailer, prev, xref_stm) =
                    read_xref_table_at(source, offset)?;
                // Merge only after a complete parse so a retry caused by a
                // short read window does not duplicate sections.
                merge_xref(xref, &local_xref);
                if first {
                    *trailer = local_trailer;
                    // Hybrid-reference file: the table points at an additional
                    // xref stream that holds compressed-object entries.
                    if let Some(xref_stm_offset) = xref_stm {
                        let ind = parse_object_at(source, xref_stm_offset)?;
                        read_xref_stream(&ind, xref, trailer, false)?;
                    }
                }
                prev
            }
            XrefStart::Stream => {
                // startxref points at an xref stream object.
                let ind = parse_object_at(source, offset)?;
                read_xref_stream(&ind, xref, trailer, first)?
            }
        };

        first = false;
        match prev {
            Some(prev) if prev != 0 => offset = prev,
            _ => break,
        }
    }
    Ok(())
}

fn read_xref_start(source: &Source, offset: u64) -> Result<XrefStart> {
    read_window(source, offset, 1024, |data| {
        let mut parser = Parser::at(data, 0);
        let token = parser.next_token()?;
        match token.kind {
            TokenKind::Keyword(Keyword::Xref) => Ok(XrefStart::Table),
            TokenKind::Int(_) => Ok(XrefStart::Stream),
            _ => Err(Error::syntax(
                token.start,
                format!(
                    "expected 'xref' or an xref stream, found {}",
                    token.describe()
                ),
            )),
        }
    })
}

/// Parse the xref table at `offset` in a growing window.  Parsing into local
/// structures keeps failed attempts (short reads) side-effect free.
fn read_xref_table_at(
    source: &Source,
    offset: u64,
) -> Result<(XRef, Dictionary, Option<u64>, Option<u64>)> {
    read_window(source, offset, XREF_WINDOW, |data| {
        let mut xref = XRef::new(0);
        let mut trailer = Dictionary::new();
        let mut parser = Parser::at(data, 0);
        parser.expect_keyword(Keyword::Xref)?;
        let (prev, xref_stm) = read_xref_table(&mut parser, &mut xref, &mut trailer)?;
        Ok((xref, trailer, prev, xref_stm))
    })
}

fn merge_xref(dst: &mut XRef, src: &XRef) {
    for section in src.sections() {
        dst.add_section(section.first_obj, section.len);
    }
    for (num, entry) in src.iter() {
        dst.set(num, entry);
    }
}

fn read_xref_table(
    parser: &mut Parser<'_>,
    xref: &mut XRef,
    trailer: &mut Dictionary,
) -> Result<(Option<u64>, Option<u64>)> {
    loop {
        let token = parser.next_token()?;
        match token.kind {
            TokenKind::Keyword(Keyword::Trailer) => {
                let dict = match parser.parse_object()? {
                    Object::Dict(dict) => dict,
                    other => {
                        return Err(Error::document(format!(
                            "trailer must be a dictionary, found {}",
                            other.type_name()
                        )))
                    }
                };
                let prev = positive(&dict, "Prev");
                let xref_stm = positive(&dict, "XRefStm");
                if let Some(size) = dict.get_int("Size") {
                    if size > 0 {
                        xref.set_declared_size(size as usize);
                    }
                }
                *trailer = dict;
                return Ok((prev, xref_stm));
            }
            TokenKind::Int(first_obj) => {
                let count = parser.expect_int()?;
                let first_obj = validate_section(first_obj, count)?;
                xref.add_section(first_obj, count as u32);
                for i in 0..count as u32 {
                    let offset = parser.expect_int()?;
                    let gen = parser.expect_int()?;
                    let kind = parser.next_token()?;
                    let gen = u16::try_from(gen)
                        .map_err(|_| Error::document("generation number out of range"))?;
                    let entry = match kind.kind {
                        TokenKind::Keyword(Keyword::N) => XrefEntry::Normal {
                            offset: u64::try_from(offset)
                                .map_err(|_| Error::document("negative object offset"))?,
                            gen,
                        },
                        TokenKind::Keyword(Keyword::F) => XrefEntry::Free { gen },
                        _ => {
                            return Err(Error::syntax(
                                kind.start,
                                format!(
                                    "expected 'n' or 'f' in xref entry, found {}",
                                    kind.describe()
                                ),
                            ))
                        }
                    };
                    xref.set(first_obj + i, entry);
                }
            }
            TokenKind::Eof => {
                return Err(Error::syntax(
                    token.start,
                    "unexpected end of input inside xref table",
                ))
            }
            _ => {
                return Err(Error::syntax(
                    token.start,
                    format!(
                        "expected xref subsection or trailer, found {}",
                        token.describe()
                    ),
                ))
            }
        }
    }
}

fn read_xref_stream(
    ind: &IndirectObject,
    xref: &mut XRef,
    trailer: &mut Dictionary,
    first: bool,
) -> Result<Option<u64>> {
    let stream = ind
        .object
        .as_stream()
        .ok_or_else(|| Error::document(format!("xref object {} is not a stream", ind.num)))?;
    let dict = &stream.dict;

    let size = dict
        .get_int("Size")
        .ok_or_else(|| Error::document("xref stream has no /Size"))?;
    if size < 0 || size as usize > MAX_OBJECTS {
        return Err(Error::document("invalid /Size in xref stream"));
    }
    xref.set_declared_size(size as usize);

    let w = dict
        .get_array("W")
        .ok_or_else(|| Error::document("xref stream has no /W array"))?;
    if w.is_empty() || w.len() > 3 {
        return Err(Error::document("xref stream /W must have 1 to 3 entries"));
    }
    let mut widths = [0usize; 3];
    for (slot, item) in widths.iter_mut().zip(w) {
        let value = item
            .as_int()
            .ok_or_else(|| Error::document("non-integer entry in /W"))?;
        if !(0..=8).contains(&value) {
            return Err(Error::document("/W field width must be between 0 and 8"));
        }
        *slot = value as usize;
    }
    if widths.iter().sum::<usize>() == 0 {
        return Err(Error::document("xref stream /W is all zeros"));
    }

    let index_pairs = match dict.get("Index") {
        None => vec![(0u32, size as u32)],
        Some(Object::Array(items)) => {
            if items.len() % 2 != 0 {
                return Err(Error::document("/Index must contain pairs of integers"));
            }
            let mut pairs = Vec::with_capacity(items.len() / 2);
            for pair in items.chunks(2) {
                let first_obj = pair[0]
                    .as_int()
                    .and_then(|v| u32::try_from(v).ok())
                    .ok_or_else(|| Error::document("invalid /Index entry"))?;
                let count = pair[1]
                    .as_int()
                    .and_then(|v| u32::try_from(v).ok())
                    .ok_or_else(|| Error::document("invalid /Index entry"))?;
                pairs.push((first_obj, count));
            }
            pairs
        }
        Some(_) => return Err(Error::document("/Index must be an array")),
    };

    let decoded = stream.decode()?;
    let mut pos = 0usize;
    for (first_obj, count) in index_pairs {
        if count == 0 {
            continue;
        }
        validate_section(first_obj as i64, count as i64)?;
        xref.add_section(first_obj, count);
        for i in 0..count {
            let type_ = if widths[0] == 0 {
                1
            } else {
                read_be(&decoded, &mut pos, widths[0])?
            };
            let field2 = read_be(&decoded, &mut pos, widths[1])?;
            let field3 = read_be(&decoded, &mut pos, widths[2])?;

            let entry = match type_ {
                0 => XrefEntry::Free { gen: field3 as u16 },
                1 => XrefEntry::Normal {
                    offset: field2,
                    gen: field3 as u16,
                },
                2 => XrefEntry::Compressed {
                    stream_num: u32::try_from(field2)
                        .map_err(|_| Error::document("object stream number out of range"))?,
                    index: u32::try_from(field3)
                        .map_err(|_| Error::document("object stream index out of range"))?,
                },
                other => {
                    return Err(Error::document(format!(
                        "invalid xref stream entry type {other}"
                    )))
                }
            };
            xref.set(first_obj + i, entry);
        }
    }

    if first {
        *trailer = dict.clone();
    }
    Ok(positive(dict, "Prev"))
}

fn read_be(data: &[u8], pos: &mut usize, width: usize) -> Result<u64> {
    if width == 0 {
        return Ok(0);
    }
    let end = pos
        .checked_add(width)
        .ok_or_else(|| Error::document("xref stream offset overflow"))?;
    let slice = data
        .get(*pos..end)
        .ok_or_else(|| Error::document("truncated xref stream"))?;
    let mut value = 0u64;
    for &byte in slice {
        value = (value << 8) | byte as u64;
    }
    *pos = end;
    Ok(value)
}

fn positive(dict: &Dictionary, key: &str) -> Option<u64> {
    dict.get_int(key)
        .and_then(|v| u64::try_from(v).ok())
        .filter(|&v| v > 0)
}

fn validate_section(first_obj: i64, count: i64) -> Result<u32> {
    if first_obj < 0 || count < 0 {
        return Err(Error::document("negative xref section header"));
    }
    let end = first_obj as u64 + count as u64;
    if end > MAX_OBJECTS as u64 {
        return Err(Error::document("xref section exceeds the object limit"));
    }
    Ok(first_obj as u32)
}

/// Increase the read window until `parse` succeeds or the error is no longer
/// plausibly caused by the window ending early.
fn read_window<T>(
    source: &Source,
    offset: u64,
    initial: usize,
    parse: impl Fn(&[u8]) -> Result<T>,
) -> Result<T> {
    let total = source.len();
    if offset >= total {
        return Err(Error::document(format!(
            "offset {offset} is past the end of the file (length {total})"
        )));
    }
    let remaining = (total - offset).min(usize::MAX as u64) as usize;
    let mut window = initial.min(remaining).max(1);

    loop {
        let data = source.read_range(offset, window)?;
        let at_eof = offset + data.len() as u64 >= total;
        match parse(&data) {
            Ok(value) => return Ok(value),
            Err(error) => {
                let retry = !at_eof && window < remaining && error.is_near_end(data.len());
                if !retry {
                    return Err(error.rebase(offset));
                }
                window = window.saturating_mul(2).min(remaining);
            }
        }
    }
}

fn parse_object_at(source: &Source, offset: u64) -> Result<IndirectObject> {
    let mut ind = read_window(source, offset, OBJECT_WINDOW, |data| {
        Parser::at(data, 0).parse_indirect_object()
    })?;
    // The parser records the stream data offset relative to the window.
    if let Object::Stream(stream) = &mut ind.object {
        stream.data_offset = stream.data_offset.saturating_add(offset);
    }
    Ok(ind)
}

fn detect_version(source: &Source) -> Result<String> {
    let head = source.read_range(0, 1024)?;
    if let Some(pos) = find_subslice(&head, b"%PDF-") {
        let rest = &head[pos + 5..];
        let end = rest
            .iter()
            .position(|&b| b == b'\r' || b == b'\n')
            .unwrap_or(rest.len());
        let version = String::from_utf8_lossy(&rest[..end]).trim().to_string();
        if !version.is_empty() {
            return Ok(format!("PDF-{version}"));
        }
    }
    Ok("PDF-1.7".into())
}

fn find_startxref(source: &Source) -> Result<u64> {
    // The keyword is normally within the last 100 bytes; a larger fallback
    // covers files with trailing garbage.
    let total = source.len();
    for tail in [2048usize, 64 * 1024] {
        let start = total.saturating_sub(tail as u64);
        let data = source.read_range(start, tail)?;
        let Some(pos) = rfind_subslice(&data, b"startxref") else {
            continue;
        };
        let mut parser = Parser::at(&data, pos + b"startxref".len());
        let token = parser.next_token().map_err(|error| error.rebase(start))?;
        return match token.kind {
            TokenKind::Int(value) => {
                u64::try_from(value).map_err(|_| Error::document("negative startxref offset"))
            }
            _ => Err(Error::syntax(
                (start as usize).saturating_add(token.start),
                format!("expected startxref offset, found {}", token.describe()),
            )),
        };
    }
    Err(Error::document("'startxref' not found"))
}
