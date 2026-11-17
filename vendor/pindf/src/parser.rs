//! Recursive-descent parser for PDF objects.
//!
//! The C implementation uses a shift/reduce push-down automaton because it
//! streams one token at a time.  With a byte window in memory a plain
//! recursive parser is shorter, handles arbitrary nesting naturally and makes
//! ownership obvious: every parse function returns the `Object` it built.

use std::collections::VecDeque;

use crate::error::{Error, Result};
use crate::lexer::{Keyword, Lexer, Token, TokenKind};
use crate::object::{Dictionary, IndirectObject, Object, Ref, Stream};

/// Maximum nesting of arrays/dictionaries; protects against stack overflow on
/// malicious input.
pub const MAX_DEPTH: usize = 256;

pub struct Parser<'a> {
    lexer: Lexer<'a>,
    /// Lookahead buffer; at most 2 tokens are ever needed (`num gen R`).
    peeked: VecDeque<Token<'a>>,
    depth: usize,
}

impl<'a> Parser<'a> {
    pub fn new(data: &'a [u8]) -> Self {
        Self::with_lexer(Lexer::new(data))
    }

    pub fn at(data: &'a [u8], pos: usize) -> Self {
        Self::with_lexer(Lexer::at(data, pos))
    }

    fn with_lexer(lexer: Lexer<'a>) -> Self {
        Self {
            lexer,
            peeked: VecDeque::new(),
            depth: 0,
        }
    }

    pub fn data(&self) -> &'a [u8] {
        self.lexer.data()
    }

    /// Position of the lexer (i.e. just after the last token read or peeked).
    pub fn pos(&self) -> usize {
        self.lexer.pos()
    }

    pub fn seek(&mut self, pos: usize) {
        self.peeked.clear();
        self.lexer.set_pos(pos);
    }

    pub fn next_token(&mut self) -> Result<Token<'a>> {
        match self.peeked.pop_front() {
            Some(token) => Ok(token),
            None => self.lexer.next_token(),
        }
    }

    pub fn peek(&mut self) -> Result<Token<'a>> {
        self.peek_nth(0)
    }

    /// Peek at the `n`-th token ahead (0 = the next token).
    pub fn peek_nth(&mut self, n: usize) -> Result<Token<'a>> {
        while self.peeked.len() <= n {
            let token = self.lexer.next_token()?;
            self.peeked.push_back(token);
        }
        Ok(self.peeked[n].clone())
    }

    pub fn expect_keyword(&mut self, kw: Keyword) -> Result<Token<'a>> {
        let token = self.next_token()?;
        if token.is_keyword(kw) {
            Ok(token)
        } else {
            Err(Error::syntax(
                token.start,
                format!("expected keyword {kw:?}, found {}", token.describe()),
            ))
        }
    }

    pub fn expect_int(&mut self) -> Result<i64> {
        let token = self.next_token()?;
        match token.kind {
            TokenKind::Int(v) => Ok(v),
            _ => Err(Error::syntax(
                token.start,
                format!("expected integer, found {}", token.describe()),
            )),
        }
    }

    /// Parse one direct object.
    pub fn parse_object(&mut self) -> Result<Object> {
        if self.depth >= MAX_DEPTH {
            return Err(Error::syntax(
                self.peek()?.start,
                "maximum object nesting depth exceeded",
            ));
        }
        self.depth += 1;
        let result = self.parse_object_inner();
        self.depth -= 1;
        result
    }

    fn parse_object_inner(&mut self) -> Result<Object> {
        let token = self.next_token()?;
        match token.kind {
            TokenKind::Int(num) => {
                // `num gen R` needs two tokens of lookahead.
                if let TokenKind::Int(gen) = self.peek()?.kind {
                    if self.peek_nth(1)?.is_keyword(Keyword::R)
                        && (0..=u32::MAX as i64).contains(&num)
                        && (0..=u16::MAX as i64).contains(&gen)
                    {
                        self.next_token()?; // gen
                        self.next_token()?; // R
                        return Ok(Object::Reference(Ref::new(num as u32, gen as u16)));
                    }
                }
                Ok(Object::Int(num))
            }
            TokenKind::Real(v) => Ok(Object::Real(v)),
            TokenKind::Name(name) => Ok(Object::Name(name.into_owned())),
            TokenKind::LiteralString(bytes) => Ok(Object::String(bytes)),
            TokenKind::HexString(bytes) => Ok(Object::HexString(bytes)),
            TokenKind::Keyword(Keyword::True) => Ok(Object::Bool(true)),
            TokenKind::Keyword(Keyword::False) => Ok(Object::Bool(false)),
            TokenKind::Keyword(Keyword::Null) => Ok(Object::Null),
            TokenKind::ArrayStart => self.parse_array(),
            TokenKind::DictStart => self.parse_dict(),
            TokenKind::Eof => Err(Error::syntax(token.start, "unexpected end of input")),
            _ => Err(Error::syntax(
                token.start,
                format!(
                    "unexpected {} where an object was expected",
                    token.describe()
                ),
            )),
        }
    }

    fn parse_array(&mut self) -> Result<Object> {
        let mut items = Vec::new();
        loop {
            let token = self.peek()?;
            match token.kind {
                TokenKind::ArrayEnd => {
                    self.next_token()?;
                    return Ok(Object::Array(items));
                }
                TokenKind::Eof => {
                    return Err(Error::syntax(token.start, "unterminated array"));
                }
                _ => items.push(self.parse_object()?),
            }
        }
    }

    fn parse_dict(&mut self) -> Result<Object> {
        let mut dict = Dictionary::new();
        loop {
            let token = self.peek()?;
            match token.kind {
                TokenKind::DictEnd => {
                    self.next_token()?;
                    return Ok(Object::Dict(dict));
                }
                TokenKind::Eof => {
                    return Err(Error::syntax(token.start, "unterminated dictionary"));
                }
                TokenKind::Name(_) => {
                    self.next_token()?;
                    let key = match token.kind {
                        TokenKind::Name(name) => name.into_owned(),
                        _ => unreachable!(),
                    };
                    let value = self.parse_object()?;
                    // Duplicate keys: the last definition wins, as in the C
                    // implementation's set_value().
                    dict.insert(key, value);
                }
                _ => {
                    return Err(Error::syntax(
                        token.start,
                        format!("dictionary key must be a name, found {}", token.describe()),
                    ))
                }
            }
        }
    }

    /// Parse `num gen obj ... endobj`.
    pub fn parse_indirect_object(&mut self) -> Result<IndirectObject> {
        let num_pos = self.peek()?.start;
        let num = self.expect_int()?;
        let gen = self.expect_int()?;
        if !(0..=u32::MAX as i64).contains(&num) {
            return Err(Error::syntax(num_pos, "object number out of range"));
        }
        if !(0..=u16::MAX as i64).contains(&gen) {
            return Err(Error::syntax(num_pos, "generation number out of range"));
        }
        self.expect_keyword(Keyword::Obj)?;

        let object = match self.parse_object()? {
            Object::Dict(dict) if self.peek()?.is_keyword(Keyword::Stream) => {
                let kw = self.next_token()?;
                Object::Stream(self.read_stream(dict, kw.end)?)
            }
            other => other,
        };

        let token = self.peek()?;
        if token.is_keyword(Keyword::EndObj) {
            self.next_token()?;
        } else {
            return Err(Error::syntax(
                token.start,
                format!("expected 'endobj', found {}", token.describe()),
            ));
        }

        Ok(IndirectObject {
            num: num as u32,
            gen: gen as u16,
            object,
        })
    }

    /// Read stream bytes starting at `keyword_end` (just after `stream`).
    fn read_stream(&mut self, dict: Dictionary, keyword_end: usize) -> Result<Stream> {
        let data = self.lexer.data();
        let start = skip_stream_eol(data, keyword_end);

        // Preferred: trust /Length when it lands right before `endstream`.
        let declared_end = dict
            .get_int("Length")
            .and_then(|v| usize::try_from(v).ok())
            .and_then(|len| start.checked_add(len))
            .filter(|&end| end <= data.len())
            .filter(|&end| followed_by_endstream(data, end));

        let end = match declared_end {
            Some(end) => end,
            None => {
                // Fallback for indirect/wrong /Length: scan for `endstream`.
                let idx = crate::util::find_subslice(&data[start..], b"endstream")
                    .ok_or_else(|| Error::syntax(data.len(), "missing 'endstream' keyword"))?;
                let mut end = start + idx;
                // Remove one EOL that belongs to the keyword, not the data.
                if end > start && data[end - 1] == b'\n' {
                    end -= 1;
                    if end > start && data[end - 1] == b'\r' {
                        end -= 1;
                    }
                } else if end > start && data[end - 1] == b'\r' {
                    end -= 1;
                }
                end
            }
        };

        let bytes = data[start..end].to_vec();
        let after = crate::util::find_subslice(&data[end..], b"endstream")
            .map(|i| end + i + b"endstream".len())
            .unwrap_or(end);
        self.seek(after);

        Ok(Stream::new(dict, bytes, start as u64))
    }
}

fn skip_stream_eol(data: &[u8], pos: usize) -> usize {
    let mut pos = pos;
    // Tolerate stray spaces between `stream` and the EOL (seen in the wild).
    while matches!(data.get(pos), Some(b' ' | b'\t')) {
        pos += 1;
    }
    match data.get(pos) {
        Some(b'\r') => {
            pos += 1;
            if data.get(pos) == Some(&b'\n') {
                pos += 1;
            }
            pos
        }
        Some(b'\n') => pos + 1,
        // No EOL at all: be lenient and start right here.
        _ => pos,
    }
}

fn followed_by_endstream(data: &[u8], pos: usize) -> bool {
    let mut pos = pos;
    if pos > data.len() {
        return false;
    }
    while matches!(
        data.get(pos),
        Some(b'\0' | b'\t' | b'\n' | b'\x0c' | b'\r' | b' ')
    ) {
        pos += 1;
    }
    data[pos..].starts_with(b"endstream")
}
