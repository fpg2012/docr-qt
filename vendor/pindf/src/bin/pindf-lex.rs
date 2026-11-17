use std::process::ExitCode;

use pindf::lexer::{Lexer, TokenKind};

fn main() -> ExitCode {
    let Some(path) = std::env::args().nth(1) else {
        eprintln!("usage: pindf-lex <file.pdf>");
        return ExitCode::FAILURE;
    };
    match run(&path) {
        Ok(()) => ExitCode::SUCCESS,
        Err(error) => {
            eprintln!("pindf-lex: {error}");
            ExitCode::FAILURE
        }
    }
}

fn run(path: &str) -> pindf::Result<()> {
    let data = std::fs::read(path)?;
    let mut lexer = Lexer::new(&data);
    // Track /Length so binary stream payloads are skipped instead of being
    // tokenized as PDF syntax (the same job the C lexer_test did by hand).
    let mut length_state = 0u8;
    let mut stream_len = 0usize;
    loop {
        let token = lexer.next_token()?;
        match &token.kind {
            TokenKind::Eof => break,
            TokenKind::Int(v) => print!("{v} "),
            TokenKind::Real(v) => print!("{v} "),
            TokenKind::Keyword(k) => print!("{k:?} "),
            TokenKind::Word(w) => print!("{} ", String::from_utf8_lossy(w)),
            TokenKind::Name(n) => print!("/{} ", String::from_utf8_lossy(n.as_ref())),
            TokenKind::LiteralString(s) => {
                print!("({}) ", String::from_utf8_lossy(s))
            }
            TokenKind::HexString(s) => {
                print!("<{} bytes of hex> ", s.len())
            }
            TokenKind::ArrayStart => print!("[ "),
            TokenKind::ArrayEnd => print!("] "),
            TokenKind::DictStart => print!("<< "),
            TokenKind::DictEnd => print!(">> "),
            TokenKind::Delim(b) => print!("{} ", *b as char),
        }

        match &token.kind {
            TokenKind::Name(name) if name.as_ref() == b"Length" && length_state == 0 => {
                length_state = 1;
            }
            TokenKind::Int(value) if length_state == 1 && *value >= 0 => {
                stream_len = *value as usize;
                length_state = 2;
            }
            TokenKind::Keyword(pindf::lexer::Keyword::Stream) if length_state == 2 => {
                let mut start = token.end;
                while matches!(data.get(start), Some(b' ' | b'\t')) {
                    start += 1;
                }
                if data.get(start) == Some(&b'\r') {
                    start += 1;
                }
                if data.get(start) == Some(&b'\n') {
                    start += 1;
                }
                lexer.set_pos(start.saturating_add(stream_len));
                print!("<stream {stream_len}> ");
                length_state = 0;
            }
            TokenKind::Keyword(pindf::lexer::Keyword::Obj | pindf::lexer::Keyword::EndObj) => {
                length_state = 0;
            }
            _ => {
                // Keep a parsed length across other dictionary entries; a bare
                // /Length not followed by an integer is not a usable length.
                if length_state == 1 {
                    length_state = 0;
                }
            }
        }
    }
    println!();
    Ok(())
}
