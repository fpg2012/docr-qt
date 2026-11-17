//! Serialization back to PDF syntax and to JSON.
//!
//! Output is appended to a `Vec<u8>` (PDF) or `String` (JSON), so there is no
//! caller-supplied fixed buffer to overflow and no reliance on NUL termination.

use std::fmt::Write as _;

use crate::lexer::is_delimiter;
use crate::object::{Dictionary, IndirectObject, Object};

/// Serialize a direct object.
pub fn object_to_bytes(obj: &Object) -> Vec<u8> {
    let mut out = Vec::new();
    write_object(&mut out, obj);
    out
}

pub fn object_to_string(obj: &Object) -> String {
    String::from_utf8_lossy(&object_to_bytes(obj)).into_owned()
}

pub fn write_object(out: &mut Vec<u8>, obj: &Object) {
    match obj {
        Object::Null => out.extend_from_slice(b"null"),
        Object::Bool(true) => out.extend_from_slice(b"true"),
        Object::Bool(false) => out.extend_from_slice(b"false"),
        Object::Int(v) => out.extend_from_slice(v.to_string().as_bytes()),
        Object::Real(v) => write_real(out, *v),
        Object::Name(name) => write_name(out, name),
        Object::String(bytes) => write_literal_string(out, bytes),
        Object::HexString(bytes) => write_hex_string(out, bytes),
        Object::Array(items) => {
            out.extend_from_slice(b"[ ");
            for item in items {
                write_object(out, item);
                out.push(b' ');
            }
            out.push(b']');
        }
        Object::Dict(dict) => write_dictionary(out, dict),
        Object::Stream(stream) => {
            write_dictionary(out, &stream.dict);
            out.extend_from_slice(b"\r\nstream\r\n");
            out.extend_from_slice(&stream.data);
            out.extend_from_slice(b"\r\nendstream");
        }
        Object::Reference(r) => {
            out.extend_from_slice(format!("{} {} R", r.num, r.gen).as_bytes());
        }
    }
}

pub fn write_dictionary(out: &mut Vec<u8>, dict: &Dictionary) {
    out.extend_from_slice(b"<< ");
    for (key, value) in dict.iter() {
        write_name(out, key);
        out.push(b' ');
        write_object(out, value);
        out.push(b' ');
    }
    out.extend_from_slice(b">>");
}

/// Serialize `num gen obj ... endobj`.
pub fn write_indirect(out: &mut Vec<u8>, ind: &IndirectObject) {
    out.extend_from_slice(format!("{} {} obj\r\n", ind.num, ind.gen).as_bytes());
    write_object(out, &ind.object);
    out.extend_from_slice(b"\r\nendobj\r\n");
}

pub fn indirect_to_bytes(ind: &IndirectObject) -> Vec<u8> {
    let mut out = Vec::new();
    write_indirect(&mut out, ind);
    out
}

/// Names may contain any byte except delimiters/whitespace; everything else is
/// written as `#XX` (PDF 7.3.5).
fn write_name(out: &mut Vec<u8>, name: &[u8]) {
    const HEX: &[u8; 16] = b"0123456789ABCDEF";
    out.push(b'/');
    for &b in name {
        if (0x21..=0x7e).contains(&b) && !is_delimiter(b) && b != b'#' {
            out.push(b);
        } else {
            out.push(b'#');
            out.push(HEX[(b >> 4) as usize]);
            out.push(HEX[(b & 0x0f) as usize]);
        }
    }
}

fn write_literal_string(out: &mut Vec<u8>, bytes: &[u8]) {
    out.push(b'(');
    for &b in bytes {
        match b {
            b'\\' | b'(' | b')' => {
                out.push(b'\\');
                out.push(b);
            }
            b'\n' => out.extend_from_slice(b"\\n"),
            b'\r' => out.extend_from_slice(b"\\r"),
            b'\t' => out.extend_from_slice(b"\\t"),
            0x08 => out.extend_from_slice(b"\\b"),
            0x0c => out.extend_from_slice(b"\\f"),
            _ => out.push(b),
        }
    }
    out.push(b')');
}

fn write_hex_string(out: &mut Vec<u8>, bytes: &[u8]) {
    const HEX: &[u8; 16] = b"0123456789ABCDEF";
    out.push(b'<');
    for &b in bytes {
        out.push(HEX[(b >> 4) as usize]);
        out.push(HEX[(b & 0x0f) as usize]);
    }
    out.push(b'>');
}

fn format_real(v: f64) -> String {
    if !v.is_finite() {
        return "0".into();
    }
    let mut text = format!("{v}");
    if text.contains(['e', 'E']) {
        // PDF has no exponent notation; use fixed point and trim.
        text = format!("{v:.10}");
        while text.ends_with('0') {
            text.pop();
        }
        if text.ends_with('.') {
            text.push('0');
        }
    } else if !text.contains('.') {
        text.push_str(".0"); // keep the object a real, not an integer
    }
    text
}

fn write_real(out: &mut Vec<u8>, v: f64) {
    out.extend_from_slice(format_real(v).as_bytes());
}

// ---------------------------------------------------------------- JSON dump

pub fn write_json_object(out: &mut String, obj: &Object) {
    match obj {
        Object::Null => out.push_str("null"),
        Object::Bool(v) => {
            let _ = write!(out, "{v}");
        }
        Object::Int(v) => {
            let _ = write!(out, "{v}");
        }
        Object::Real(v) => out.push_str(&format_real(*v)),
        Object::Name(name) => write_json_name(out, name),
        Object::String(bytes) => write_json_string(out, bytes),
        Object::HexString(bytes) => {
            out.push_str("{\"type\": \"hex\",\"str\":\"");
            for b in bytes {
                let _ = write!(out, "{b:02x}");
            }
            out.push_str("\"}");
        }
        Object::Array(items) => {
            out.push('[');
            for (i, item) in items.iter().enumerate() {
                if i > 0 {
                    out.push(',');
                }
                write_json_object(out, item);
            }
            out.push(']');
        }
        Object::Dict(dict) => write_json_dictionary(out, dict),
        Object::Stream(stream) => {
            let _ = write!(
                out,
                "{{\"type\":\"stream\",\"content_offset\":{}",
                stream.data_offset
            );
            out.push_str(",\"dict\":");
            write_json_dictionary(out, &stream.dict);
            let _ = write!(out, ",\"len\":\"{}\"", stream.data.len());
            out.push('}');
        }
        Object::Reference(r) => {
            let _ = write!(out, "{{\"type\": \"ref\",\"a\":{},\"b\":{}}}", r.num, r.gen);
        }
    }
}

pub fn write_json_dictionary(out: &mut String, dict: &Dictionary) {
    out.push('{');
    for (i, (key, value)) in dict.iter().enumerate() {
        if i > 0 {
            out.push(',');
        }
        write_json_name(out, key);
        out.push(':');
        write_json_object(out, value);
    }
    out.push('}');
}

/// JSON representation of an indirect object (the C dump format).
pub fn write_json_indirect(out: &mut String, ind: &IndirectObject) {
    let _ = write!(
        out,
        "{{\"type\": \"ind\",\"a\":{},\"b\":{},\"obj\":",
        ind.num, ind.gen
    );
    write_json_object(out, &ind.object);
    out.push('}');
}

pub fn to_json(obj: &Object) -> String {
    let mut out = String::new();
    write_json_object(&mut out, obj);
    out
}

pub fn indirect_to_json(ind: &IndirectObject) -> String {
    let mut out = String::new();
    write_json_indirect(&mut out, ind);
    out
}

fn write_json_name(out: &mut String, name: &[u8]) {
    out.push('"');
    out.push('/');
    write_json_escaped(out, name);
    out.push('"');
}

fn write_json_string(out: &mut String, bytes: &[u8]) {
    out.push('"');
    write_json_escaped(out, bytes);
    out.push('"');
}

fn write_json_escaped(out: &mut String, bytes: &[u8]) {
    for &b in bytes {
        match b {
            b'"' => out.push_str("\\\""),
            b'\\' => out.push_str("\\\\"),
            b'\n' => out.push_str("\\n"),
            b'\r' => out.push_str("\\r"),
            b'\t' => out.push_str("\\t"),
            0x08 => out.push_str("\\b"),
            0x0c => out.push_str("\\f"),
            0x20..=0x7e => out.push(b as char),
            _ => {
                let _ = write!(out, "\\u{b:04x}");
            }
        }
    }
}
