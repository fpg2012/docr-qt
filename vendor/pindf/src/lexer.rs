//! Byte-oriented PDF lexer.
//!
//! Unlike the C version this works directly on an in-memory byte slice and
//! returns borrowed token text, so tokenizing a PDF performs no allocation
//! except for string payloads (literals and hex strings, which are decoded).
//! Whitespace, line breaks and comments are skipped here: no parser state ever
//! needs them.

use std::borrow::Cow;

use crate::error::{Error, Result};

/// PDF keywords that are meaningful for document structure.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Keyword {
    Obj,
    EndObj,
    Stream,
    EndStream,
    Xref,
    Trailer,
    StartXref,
    True,
    False,
    Null,
    /// `R` - indirect reference
    R,
    /// `n` - in-use xref entry
    N,
    /// `f` - free xref entry
    F,
}

pub fn keyword_from_bytes(word: &[u8]) -> Option<Keyword> {
    Some(match word {
        b"obj" => Keyword::Obj,
        b"endobj" => Keyword::EndObj,
        b"stream" => Keyword::Stream,
        b"endstream" => Keyword::EndStream,
        b"xref" => Keyword::Xref,
        b"trailer" => Keyword::Trailer,
        b"startxref" => Keyword::StartXref,
        b"true" => Keyword::True,
        b"false" => Keyword::False,
        b"null" => Keyword::Null,
        b"R" => Keyword::R,
        b"n" => Keyword::N,
        b"f" => Keyword::F,
        _ => return None,
    })
}

#[derive(Clone, Debug, PartialEq)]
pub enum TokenKind<'a> {
    Int(i64),
    Real(f64),
    Keyword(Keyword),
    /// An unrecognized regular token (content stream operator, ...).
    Word(&'a [u8]),
    /// Name without the leading `/`, `#XX` escapes already decoded.
    Name(Cow<'a, [u8]>),
    /// Literal string, escapes decoded.
    LiteralString(Vec<u8>),
    /// Hex string, decoded to bytes.
    HexString(Vec<u8>),
    ArrayStart,
    ArrayEnd,
    DictStart,
    DictEnd,
    /// A delimiter that is invalid in object syntax (`)`, `{`, `}`, lone `>`).
    Delim(u8),
    Eof,
}

#[derive(Clone, Debug)]
pub struct Token<'a> {
    pub kind: TokenKind<'a>,
    /// Byte offset of the first character of the token.
    pub start: usize,
    /// Byte offset one past the last character of the token.
    pub end: usize,
}

impl Token<'_> {
    pub fn is_keyword(&self, kw: Keyword) -> bool {
        self.kind == TokenKind::Keyword(kw)
    }

    pub fn is_eof(&self) -> bool {
        matches!(self.kind, TokenKind::Eof)
    }

    pub fn describe(&self) -> String {
        match &self.kind {
            TokenKind::Int(v) => format!("integer {v}"),
            TokenKind::Real(v) => format!("real {v}"),
            TokenKind::Keyword(k) => format!("keyword {k:?}"),
            TokenKind::Word(w) => format!("word {:?}", String::from_utf8_lossy(w)),
            TokenKind::Name(n) => format!("name /{}", String::from_utf8_lossy(n.as_ref())),
            TokenKind::LiteralString(_) => "literal string".into(),
            TokenKind::HexString(_) => "hex string".into(),
            TokenKind::ArrayStart => "'['".into(),
            TokenKind::ArrayEnd => "']'".into(),
            TokenKind::DictStart => "'<<'".into(),
            TokenKind::DictEnd => "'>>'".into(),
            TokenKind::Delim(b) => format!("{:?}", *b as char),
            TokenKind::Eof => "end of input".into(),
        }
    }
}

pub(crate) fn is_whitespace(b: u8) -> bool {
    matches!(b, b'\0' | b'\t' | b'\n' | b'\x0c' | b'\r' | b' ')
}

pub(crate) fn is_delimiter(b: u8) -> bool {
    matches!(
        b,
        b'(' | b')' | b'<' | b'>' | b'[' | b']' | b'{' | b'}' | b'/' | b'%'
    )
}

fn is_regular(b: u8) -> bool {
    !is_whitespace(b) && !is_delimiter(b)
}

pub(crate) fn hex_val(b: u8) -> Option<u8> {
    match b {
        b'0'..=b'9' => Some(b - b'0'),
        b'a'..=b'f' => Some(b - b'a' + 10),
        b'A'..=b'F' => Some(b - b'A' + 10),
        _ => None,
    }
}

pub struct Lexer<'a> {
    data: &'a [u8],
    pos: usize,
}

impl<'a> Lexer<'a> {
    pub fn new(data: &'a [u8]) -> Self {
        Self { data, pos: 0 }
    }

    pub fn at(data: &'a [u8], pos: usize) -> Self {
        Self {
            data,
            pos: pos.min(data.len()),
        }
    }

    pub fn data(&self) -> &'a [u8] {
        self.data
    }

    pub fn pos(&self) -> usize {
        self.pos
    }

    pub fn set_pos(&mut self, pos: usize) {
        self.pos = pos.min(self.data.len());
    }

    /// Bytes `[start, start + len)` of the source, or an error if out of range.
    pub fn stream_bytes(&self, start: usize, len: usize) -> Result<&'a [u8]> {
        self.data
            .get(start..start.saturating_add(len))
            .ok_or_else(|| Error::syntax(start, "stream length exceeds file size"))
    }

    fn skip_trivia(&mut self) {
        loop {
            match self.data.get(self.pos).copied() {
                Some(b'\0' | b'\t' | b'\n' | b'\x0c' | b'\r' | b' ') => self.pos += 1,
                Some(b'%') => {
                    while let Some(&b) = self.data.get(self.pos) {
                        if b == b'\n' || b == b'\r' {
                            break;
                        }
                        self.pos += 1;
                    }
                }
                _ => break,
            }
        }
    }

    /// Next significant token, skipping whitespace and comments.
    pub fn next_token(&mut self) -> Result<Token<'a>> {
        self.skip_trivia();
        let start = self.pos;
        let Some(&b) = self.data.get(start) else {
            return Ok(Token {
                kind: TokenKind::Eof,
                start,
                end: start,
            });
        };

        let kind = match b {
            b'[' => {
                self.pos += 1;
                TokenKind::ArrayStart
            }
            b']' => {
                self.pos += 1;
                TokenKind::ArrayEnd
            }
            b'<' => {
                if self.data.get(start + 1) == Some(&b'<') {
                    self.pos += 2;
                    TokenKind::DictStart
                } else {
                    TokenKind::HexString(self.read_hex_string()?)
                }
            }
            b'>' => {
                if self.data.get(start + 1) == Some(&b'>') {
                    self.pos += 2;
                    TokenKind::DictEnd
                } else {
                    self.pos += 1;
                    TokenKind::Delim(b'>')
                }
            }
            b')' | b'{' | b'}' => {
                self.pos += 1;
                TokenKind::Delim(b)
            }
            b'(' => TokenKind::LiteralString(self.read_literal_string()?),
            b'/' => {
                self.pos += 1;
                TokenKind::Name(self.read_name())
            }
            b'+' | b'-' | b'.' | b'0'..=b'9' => self.read_number(start)?,
            _ => {
                let word = self.read_regular();
                match keyword_from_bytes(word) {
                    Some(kw) => TokenKind::Keyword(kw),
                    None => TokenKind::Word(word),
                }
            }
        };

        Ok(Token {
            kind,
            start,
            end: self.pos,
        })
    }

    fn read_regular(&mut self) -> &'a [u8] {
        let start = self.pos;
        while self.data.get(self.pos).copied().is_some_and(is_regular) {
            self.pos += 1;
        }
        &self.data[start..self.pos]
    }

    fn read_name(&mut self) -> Cow<'a, [u8]> {
        let start = self.pos;
        // Fast path: no `#XX` escape, borrow directly from the input.
        let mut has_escape = false;
        while let Some(&b) = self.data.get(self.pos) {
            if !is_regular(b) {
                break;
            }
            if b == b'#' {
                has_escape = true;
            }
            self.pos += 1;
        }
        let raw = &self.data[start..self.pos];
        if !has_escape {
            return Cow::Borrowed(raw);
        }

        // Slow path: decode `#XX` escapes (PDF 7.3.5).
        let mut out = Vec::with_capacity(raw.len());
        let mut i = 0;
        while i < raw.len() {
            if raw[i] == b'#' && i + 2 < raw.len() {
                if let (Some(hi), Some(lo)) = (hex_val(raw[i + 1]), hex_val(raw[i + 2])) {
                    out.push((hi << 4) | lo);
                    i += 3;
                    continue;
                }
            }
            out.push(raw[i]);
            i += 1;
        }
        Cow::Owned(out)
    }

    fn read_number(&mut self, start: usize) -> Result<TokenKind<'a>> {
        while let Some(&b) = self.data.get(self.pos) {
            if b.is_ascii_digit() || matches!(b, b'+' | b'-' | b'.') {
                self.pos += 1;
            } else {
                break;
            }
        }
        let raw = &self.data[start..self.pos];
        let text =
            std::str::from_utf8(raw).map_err(|_| Error::syntax(start, "invalid byte in number"))?;
        if raw.contains(&b'.') {
            text.parse::<f64>()
                .map(TokenKind::Real)
                .map_err(|_| Error::syntax(start, format!("invalid real number {text:?}")))
        } else {
            match text.parse::<i64>() {
                Ok(v) => Ok(TokenKind::Int(v)),
                // Out-of-range integers are still numbers; keep the value.
                Err(_) => text
                    .parse::<f64>()
                    .map(TokenKind::Real)
                    .map_err(|_| Error::syntax(start, format!("invalid number {text:?}"))),
            }
        }
    }

    fn read_hex_string(&mut self) -> Result<Vec<u8>> {
        self.pos += 1; // '<'
        let mut out = Vec::new();
        let mut hi: Option<u8> = None;
        loop {
            let Some(&b) = self.data.get(self.pos) else {
                return Err(Error::syntax(self.pos, "unterminated hex string"));
            };
            self.pos += 1;
            if b == b'>' {
                break;
            }
            if is_whitespace(b) {
                continue;
            }
            let Some(v) = hex_val(b) else {
                return Err(Error::syntax(
                    self.pos - 1,
                    format!("invalid hex digit {:?}", b as char),
                ));
            };
            match hi.take() {
                None => hi = Some(v),
                Some(h) => out.push((h << 4) | v),
            }
        }
        if let Some(h) = hi {
            out.push(h << 4); // odd number of digits: pad with 0
        }
        Ok(out)
    }

    fn read_literal_string(&mut self) -> Result<Vec<u8>> {
        self.pos += 1; // '('
        let mut out = Vec::new();
        let mut depth = 1usize;
        loop {
            let Some(&b) = self.data.get(self.pos) else {
                return Err(Error::syntax(self.pos, "unterminated literal string"));
            };
            self.pos += 1;
            match b {
                b'\\' => {
                    let Some(&e) = self.data.get(self.pos) else {
                        return Err(Error::syntax(self.pos, "unterminated escape in string"));
                    };
                    self.pos += 1;
                    match e {
                        b'n' => out.push(b'\n'),
                        b'r' => out.push(b'\r'),
                        b't' => out.push(b'\t'),
                        b'b' => out.push(0x08),
                        b'f' => out.push(0x0c),
                        b'(' | b')' | b'\\' => out.push(e),
                        b'\r' => {
                            // line continuation: CR or CRLF is dropped
                            if self.data.get(self.pos) == Some(&b'\n') {
                                self.pos += 1;
                            }
                        }
                        b'\n' => {}
                        b'0'..=b'7' => {
                            let mut value = (e - b'0') as u32;
                            for _ in 0..2 {
                                match self.data.get(self.pos) {
                                    Some(&d @ b'0'..=b'7') => {
                                        value = value * 8 + (d - b'0') as u32;
                                        self.pos += 1;
                                    }
                                    _ => break,
                                }
                            }
                            out.push(value as u8);
                        }
                        // Unknown escapes: the backslash is ignored.
                        other => out.push(other),
                    }
                }
                b'(' => {
                    depth += 1;
                    out.push(b);
                }
                b')' => {
                    depth -= 1;
                    if depth == 0 {
                        return Ok(out);
                    }
                    out.push(b);
                }
                _ => out.push(b),
            }
        }
    }
}
