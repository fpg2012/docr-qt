//! Stream filters and prediction.
//!
//! Output buffers grow dynamically, so streams larger than any fixed size are
//! handled correctly (the C version used two 1 MiB buffers).

use std::io::Read;

use flate2::read::{DeflateDecoder, ZlibDecoder};

use crate::error::{Error, Result};
use crate::lexer::hex_val;
use crate::object::{Dictionary, Object, Stream};

/// `/DecodeParms` for one filter.
#[derive(Clone, Copy, Debug)]
pub struct DecodeParams {
    pub predictor: i64,
    pub colors: i64,
    pub bits_per_component: i64,
    pub columns: i64,
    /// LZW `/EarlyChange`, 1 by default.
    pub early_change: i64,
}

impl Default for DecodeParams {
    fn default() -> Self {
        Self {
            predictor: 1,
            colors: 1,
            bits_per_component: 8,
            columns: 1,
            early_change: 1,
        }
    }
}

impl DecodeParams {
    pub fn from_dict(dict: &Dictionary) -> Self {
        let mut p = Self::default();
        if let Some(v) = dict.get_int("Predictor") {
            p.predictor = v;
        }
        if let Some(v) = dict.get_int("Colors") {
            p.colors = v;
        }
        if let Some(v) = dict.get_int("BitsPerComponent") {
            p.bits_per_component = v;
        }
        if let Some(v) = dict.get_int("Columns") {
            p.columns = v;
        }
        if let Some(v) = dict.get_int("EarlyChange") {
            p.early_change = v;
        }
        p
    }
}

/// Decode `data` with a single filter.
pub fn apply_filter(name: &[u8], data: &[u8], params: DecodeParams) -> Result<Vec<u8>> {
    match name {
        b"FlateDecode" | b"Fl" => flate_decode(data, params),
        b"ASCIIHexDecode" | b"AHx" => ascii_hex_decode(data),
        b"ASCII85Decode" | b"A85" => ascii85_decode(data),
        b"RunLengthDecode" | b"RL" => run_length_decode(data),
        b"LZWDecode" | b"LZW" => lzw_decode(data, params),
        other => Err(Error::unsupported(format!(
            "filter /{}",
            String::from_utf8_lossy(other)
        ))),
    }
}

impl Stream {
    /// Filter names, in application order.  `/Filter` may be a name or an array.
    pub fn filters(&self) -> Result<Vec<Vec<u8>>> {
        match self.dict.get("Filter") {
            None | Some(Object::Null) => Ok(Vec::new()),
            Some(Object::Name(name)) => Ok(vec![name.clone()]),
            Some(Object::Array(items)) => items
                .iter()
                .map(|item| {
                    item.as_name()
                        .map(|n| n.to_vec())
                        .ok_or_else(|| Error::document("non-name entry in /Filter array"))
                })
                .collect(),
            Some(_) => Err(Error::document("/Filter must be a name or an array")),
        }
    }

    /// Decode parameters, one per filter (defaults when absent).
    pub fn decode_params(&self) -> Result<Vec<DecodeParams>> {
        let filters = self.filters()?;
        let mut params = vec![DecodeParams::default(); filters.len()];
        match self.dict.get("DecodeParms") {
            None | Some(Object::Null) => {}
            Some(Object::Dict(dict)) => {
                if let Some(first) = params.first_mut() {
                    *first = DecodeParams::from_dict(dict);
                }
            }
            Some(Object::Array(items)) => {
                for (i, item) in items.iter().enumerate() {
                    if let Some(slot) = params.get_mut(i) {
                        match item {
                            Object::Dict(dict) => *slot = DecodeParams::from_dict(dict),
                            Object::Null => {}
                            _ => {
                                return Err(Error::document(
                                    "/DecodeParms array entries must be dictionaries or null",
                                ))
                            }
                        }
                    }
                }
            }
            Some(_) => {
                return Err(Error::document(
                    "/DecodeParms must be a dictionary or an array",
                ))
            }
        }
        Ok(params)
    }

    /// Apply all filters declared in the stream dictionary.
    pub fn decode(&self) -> Result<Vec<u8>> {
        let filters = self.filters()?;
        let params = self.decode_params()?;
        let mut data = self.data.clone();
        for (i, filter) in filters.iter().enumerate() {
            let p = params.get(i).copied().unwrap_or_default();
            data = apply_filter(filter, &data, p)?;
        }
        Ok(data)
    }
}

fn flate_decode(data: &[u8], params: DecodeParams) -> Result<Vec<u8>> {
    let inflated = inflate(data)?;
    apply_predictor(&inflated, params)
}

fn inflate(data: &[u8]) -> Result<Vec<u8>> {
    let mut out = Vec::new();
    match ZlibDecoder::new(data).read_to_end(&mut out) {
        Ok(_) => Ok(out),
        Err(zlib_error) => {
            // Some producers omit the two-byte zlib header; try raw deflate.
            out.clear();
            DeflateDecoder::new(data)
                .read_to_end(&mut out)
                .map_err(|_| Error::document(format!("flate decode failed: {zlib_error}")))?;
            Ok(out)
        }
    }
}

fn apply_predictor(data: &[u8], params: DecodeParams) -> Result<Vec<u8>> {
    match params.predictor {
        0 | 1 => Ok(data.to_vec()),
        2 => tiff_predictor(data, params),
        10..=15 => png_predictor(data, params),
        other => Err(Error::unsupported(format!(
            "flate predictor {other} (only 1, 2 and 10-15 are implemented)"
        ))),
    }
}

fn row_geometry(params: DecodeParams) -> Result<(usize, usize)> {
    let positive = |value: i64, limit: u64| -> Result<u64> {
        u64::try_from(value)
            .ok()
            .filter(|&v| v > 0 && v <= limit)
            .ok_or_else(|| {
                Error::document("predictor Colors/BitsPerComponent/Columns out of range")
            })
    };
    let colors = positive(params.colors, 1 << 16)?;
    let bits_per_component = positive(params.bits_per_component, 64)?;
    let columns = positive(params.columns, 1 << 24)?;

    let bits = colors
        .checked_mul(bits_per_component)
        .and_then(|v| v.checked_mul(columns))
        .filter(|&v| v <= (1 << 30))
        .ok_or_else(|| Error::document("predictor row is unreasonably large"))?;
    let row_size = ((bits + 7) / 8) as usize;
    let bpp = ((colors * bits_per_component + 7) / 8).max(1) as usize;
    Ok((row_size, bpp))
}

fn png_predictor(data: &[u8], params: DecodeParams) -> Result<Vec<u8>> {
    let (row_size, bpp) = row_geometry(params)?;
    let stride = row_size + 1; // filter type byte + row
    if data.len() % stride != 0 {
        return Err(Error::document(
            "PNG predictor data length is not a multiple of row size + 1",
        ));
    }
    let rows = data.len() / stride;
    let mut out = Vec::with_capacity(rows * row_size);
    let mut prev = vec![0u8; row_size];
    let mut cur = vec![0u8; row_size];

    for row in 0..rows {
        let offset = row * stride;
        let filter = data[offset];
        let src = &data[offset + 1..offset + stride];
        for i in 0..row_size {
            let a = if i >= bpp { cur[i - bpp] } else { 0 };
            let b = prev[i];
            let c = if i >= bpp { prev[i - bpp] } else { 0 };
            cur[i] = match filter {
                0 => src[i],
                1 => src[i].wrapping_add(a),
                2 => src[i].wrapping_add(b),
                3 => src[i].wrapping_add(((a as u16 + b as u16) / 2) as u8),
                4 => src[i].wrapping_add(paeth(a, b, c)),
                other => {
                    return Err(Error::document(format!(
                        "invalid PNG predictor filter type {other}"
                    )))
                }
            };
        }
        out.extend_from_slice(&cur);
        std::mem::swap(&mut prev, &mut cur);
    }
    Ok(out)
}

fn paeth(a: u8, b: u8, c: u8) -> u8 {
    let p = a as i16 + b as i16 - c as i16;
    let pa = (p - a as i16).abs();
    let pb = (p - b as i16).abs();
    let pc = (p - c as i16).abs();
    if pa <= pb && pa <= pc {
        a
    } else if pb <= pc {
        b
    } else {
        c
    }
}

fn tiff_predictor(data: &[u8], params: DecodeParams) -> Result<Vec<u8>> {
    let (row_size, _) = row_geometry(params)?;
    if params.bits_per_component != 8 {
        return Err(Error::unsupported(
            "TIFF predictor with BitsPerComponent != 8",
        ));
    }
    let bpp = params.colors as usize;
    if row_size % bpp != 0 || bpp == 0 {
        return Err(Error::document("invalid TIFF predictor row geometry"));
    }
    let mut out = data.to_vec();
    for row in out.chunks_mut(row_size) {
        for i in bpp..row.len() {
            row[i] = row[i].wrapping_add(row[i - bpp]);
        }
    }
    Ok(out)
}

fn ascii_hex_decode(data: &[u8]) -> Result<Vec<u8>> {
    let mut out = Vec::with_capacity(data.len() / 2);
    let mut hi: Option<u8> = None;
    for &b in data {
        if b == b'>' {
            break;
        }
        if b.is_ascii_whitespace() {
            continue;
        }
        let Some(v) = hex_val(b) else {
            return Err(Error::document(format!(
                "invalid character {:?} in ASCIIHexDecode data",
                b as char
            )));
        };
        match hi.take() {
            None => hi = Some(v),
            Some(h) => out.push((h << 4) | v),
        }
    }
    if let Some(h) = hi {
        out.push(h << 4);
    }
    Ok(out)
}

fn ascii85_decode(data: &[u8]) -> Result<Vec<u8>> {
    let mut out = Vec::with_capacity(data.len() * 4 / 5);
    let mut group = [0u8; 5];
    let mut n = 0usize;
    let mut i = 0usize;
    if data.starts_with(b"<~") {
        i = 2;
    }
    while i < data.len() {
        let b = data[i];
        i += 1;
        if b.is_ascii_whitespace() {
            continue;
        }
        if b == b'~' {
            break;
        }
        if b == b'z' && n == 0 {
            out.extend_from_slice(&[0, 0, 0, 0]);
            continue;
        }
        if !(b'!'..=b'u').contains(&b) {
            return Err(Error::document(format!(
                "invalid character {:?} in ASCII85Decode data",
                b as char
            )));
        }
        group[n] = b - b'!';
        n += 1;
        if n == 5 {
            let value = group
                .iter()
                .fold(0u32, |acc, &d| acc.wrapping_mul(85).wrapping_add(d as u32));
            out.extend_from_slice(&value.to_be_bytes());
            n = 0;
        }
    }
    if n > 0 {
        if n == 1 {
            return Err(Error::document(
                "truncated final group in ASCII85Decode data",
            ));
        }
        for slot in group.iter_mut().skip(n) {
            *slot = 84; // pad with 'u'
        }
        let value = group
            .iter()
            .fold(0u32, |acc, &d| acc.wrapping_mul(85).wrapping_add(d as u32));
        out.extend_from_slice(&value.to_be_bytes()[..n - 1]);
    }
    Ok(out)
}

fn run_length_decode(data: &[u8]) -> Result<Vec<u8>> {
    let mut out = Vec::new();
    let mut i = 0usize;
    while i < data.len() {
        let len = data[i];
        i += 1;
        match len {
            128 => break,
            0..=127 => {
                let count = len as usize + 1;
                let end = i + count;
                if end > data.len() {
                    return Err(Error::document("truncated RunLengthDecode literal run"));
                }
                out.extend_from_slice(&data[i..end]);
                i = end;
            }
            _ => {
                let count = 257 - len as usize;
                let Some(&byte) = data.get(i) else {
                    return Err(Error::document("truncated RunLengthDecode repeat run"));
                };
                i += 1;
                out.extend(std::iter::repeat(byte).take(count));
            }
        }
    }
    Ok(out)
}

struct BitReader<'a> {
    data: &'a [u8],
    bit: usize,
}

impl<'a> BitReader<'a> {
    fn new(data: &'a [u8]) -> Self {
        Self { data, bit: 0 }
    }

    fn read(&mut self, width: usize) -> Option<u32> {
        if self.bit + width > self.data.len() * 8 {
            return None;
        }
        let mut value = 0u32;
        for _ in 0..width {
            let byte = self.data[self.bit / 8];
            let shift = 7 - (self.bit % 8);
            value = (value << 1) | ((byte >> shift) & 1) as u32;
            self.bit += 1;
        }
        Some(value)
    }
}

fn lzw_decode(data: &[u8], params: DecodeParams) -> Result<Vec<u8>> {
    const CLEAR: u32 = 256;
    const EOD: u32 = 257;

    let early_change = params.early_change.clamp(0, 1) as usize;
    let mut table: Vec<Vec<u8>> = (0..256u32).map(|i| vec![i as u8]).collect();
    table.push(Vec::new()); // 256: clear
    table.push(Vec::new()); // 257: end of data

    let mut out = Vec::new();
    let mut reader = BitReader::new(data);
    let mut width = 9usize;
    let mut prev: Option<u32> = None;

    loop {
        let Some(code) = reader.read(width) else {
            break;
        };
        if code == CLEAR {
            table.truncate(258);
            width = 9;
            prev = None;
            continue;
        }
        if code == EOD {
            break;
        }

        let entry = if (code as usize) < table.len() {
            table[code as usize].clone()
        } else if let Some(prev_code) = prev {
            if code as usize == table.len() {
                let mut entry = table[prev_code as usize].clone();
                if let Some(&first) = entry.first() {
                    entry.push(first);
                }
                entry
            } else {
                return Err(Error::document("invalid LZW code"));
            }
        } else {
            return Err(Error::document("invalid LZW code at stream start"));
        };

        out.extend_from_slice(&entry);

        if let Some(prev_code) = prev {
            if table.len() < 4096 {
                let mut new_entry = table[prev_code as usize].clone();
                if let Some(&first) = entry.first() {
                    new_entry.push(first);
                }
                table.push(new_entry);
            }
        }
        prev = Some(code);

        while width < 12 && table.len() + early_change >= (1 << width) {
            width += 1;
        }
    }

    Ok(out)
}

#[cfg(test)]
mod tests {
    use super::*;
    use flate2::write::ZlibEncoder;
    use flate2::Compression;
    use std::io::Write;

    fn deflate(data: &[u8]) -> Vec<u8> {
        let mut encoder = ZlibEncoder::new(Vec::new(), Compression::default());
        encoder.write_all(data).unwrap();
        encoder.finish().unwrap()
    }

    #[test]
    fn flate_roundtrip() {
        let raw = b"hello hello hello hello";
        let decoded = apply_filter(b"FlateDecode", &deflate(raw), DecodeParams::default()).unwrap();
        assert_eq!(decoded, raw);
    }

    #[test]
    fn png_up_predictor() {
        // 2 rows of 3 bytes, filter type 2 (Up), second row adds first row.
        let encoded = [2u8, 1, 2, 3, 2, 1, 1, 1];
        let params = DecodeParams {
            predictor: 12,
            colors: 1,
            bits_per_component: 8,
            columns: 3,
            ..Default::default()
        };
        let decoded = png_predictor(&encoded, params).unwrap();
        assert_eq!(decoded, [1, 2, 3, 2, 3, 4]);
    }

    #[test]
    fn png_predictor_filter_types() {
        let params = DecodeParams {
            predictor: 15,
            colors: 1,
            bits_per_component: 8,
            columns: 3,
            ..Default::default()
        };
        // Sub: adds the reconstructed byte on the left.
        assert_eq!(png_predictor(&[1, 5, 2, 2], params).unwrap(), [5, 7, 9]);
        // Average: adds floor((left + above) / 2); the previous row is zero.
        assert_eq!(png_predictor(&[3, 5, 5, 6], params).unwrap(), [5, 7, 9]);
        // Paeth: picks the closest of left/above/upper-left.
        assert_eq!(
            png_predictor(&[4, 11, 10, 10], params).unwrap(),
            [11, 21, 31]
        );
        // Up: second row adds the first row.
        assert_eq!(
            png_predictor(&[0, 10, 20, 30, 2, 1, 2, 3], params).unwrap(),
            [10, 20, 30, 11, 22, 33]
        );
    }

    #[test]
    fn tiff_predictor_adds_previous_pixel() {
        let params = DecodeParams {
            predictor: 2,
            colors: 3,
            bits_per_component: 8,
            columns: 2,
            ..Default::default()
        };
        assert_eq!(
            tiff_predictor(&[1, 2, 3, 10, 10, 10], params).unwrap(),
            [1, 2, 3, 11, 12, 13]
        );
    }

    #[test]
    fn ascii_hex_and_85() {
        assert_eq!(ascii_hex_decode(b"48 65 6c 6C 6f>").unwrap(), b"Hello");
        assert_eq!(
            ascii85_decode(b"87cURD]i,\"Ebo80~>").unwrap(),
            b"Hello World!"
        );
    }

    #[test]
    fn lzw_reference_streams() {
        // Encoded with an independent implementation (PDF LZW, EarlyChange=1).
        let cases: &[(&[u8], &[u8])] = &[
            (
                b"TOBEORNOTTOBEORTOBEORNOT",
                &[
                    128, 21, 9, 228, 34, 41, 60, 164, 78, 39, 149, 32, 80, 72, 52, 46, 11, 7, 132,
                    192, 64,
                ],
            ),
            (b"-----A---B", &[128, 11, 96, 80, 34, 12, 12, 133, 1]),
            (
                b"abcabcabcabcabcabcabcabcabcabcabcabcabcabcabcabc",
                &[
                    128, 24, 76, 70, 56, 20, 18, 7, 5, 132, 65, 225, 80, 104, 100, 38, 27, 11, 130,
                    192, 64,
                ],
            ),
        ];
        for (raw, encoded) in cases {
            assert_eq!(
                lzw_decode(encoded, DecodeParams::default()).unwrap(),
                *raw,
                "LZW decode mismatch for {:?}",
                String::from_utf8_lossy(raw)
            );
        }
    }

    #[test]
    fn run_length() {
        assert_eq!(
            run_length_decode(&[2, b'a', b'b', b'c', 254, b'x', 128]).unwrap(),
            b"abcxxx"
        );
    }
}
