use std::process::ExitCode;

use pindf::{Document, Error, Object, XrefEntry};

fn main() -> ExitCode {
    let args: Vec<String> = std::env::args().collect();
    let decode = args.iter().any(|arg| arg == "--decode");
    let Some(path) = args.iter().skip(1).find(|arg| !arg.starts_with("--")) else {
        eprintln!("usage: pindf-check <file.pdf> [--decode]");
        return ExitCode::FAILURE;
    };

    match run(path, decode) {
        Ok(()) => ExitCode::SUCCESS,
        Err(error) => {
            eprintln!("pindf-check: {error}");
            ExitCode::FAILURE
        }
    }
}

/// Load every in-use object and report the result.  With `--decode`, all
/// streams are decoded as well.
fn run(path: &str, decode: bool) -> pindf::Result<()> {
    let mut doc = Document::open(path)?;
    println!("file:        {path}");
    println!(
        "version:     {}   xref size: {}   startxref: {}",
        doc.version(),
        doc.xref().size(),
        doc.xref_offset()
    );
    println!("xref:        {} section(s)", doc.xref().sections().len());
    if doc.is_encrypted() {
        println!("warning:     trailer contains /Encrypt (strings/streams are encrypted)");
    }

    if let Some(root) = doc.trailer().get("Root").and_then(Object::as_ref) {
        if let Ok(catalog) = doc.resolve(root) {
            if let Some(pages) = catalog
                .as_dict()
                .and_then(|dict| dict.get("Pages"))
                .and_then(Object::as_ref)
            {
                if let Ok(pages) = doc.resolve(pages) {
                    if let Some(count) = pages.as_dict().and_then(|dict| dict.get_int("Count")) {
                        println!("pages:       {count}");
                    }
                }
            }
        }
    }

    let mut in_use = 0usize;
    let mut loaded = 0usize;
    let mut errors = 0usize;
    let mut streams = 0usize;
    let mut decoded = 0usize;
    let mut decoded_bytes = 0usize;

    for num in 0..doc.xref().size() as u32 {
        match doc.xref().get(num) {
            None | Some(XrefEntry::Free { .. }) => continue,
            Some(_) => {}
        }
        in_use += 1;
        match doc.indirect_object(num) {
            Ok(ind) => {
                loaded += 1;
                if decode {
                    if let Some(stream) = ind.object.as_stream() {
                        streams += 1;
                        match stream.decode() {
                            Ok(data) => {
                                decoded += 1;
                                decoded_bytes += data.len();
                            }
                            Err(error) => {
                                errors += 1;
                                eprintln!("object {num}: stream decode failed: {error}");
                            }
                        }
                    }
                }
            }
            Err(error) => {
                errors += 1;
                eprintln!("object {num}: {error}");
            }
        }
    }

    println!("in-use:      {in_use}   loaded: {loaded}   errors: {errors}");
    if decode {
        println!("streams:     {streams}   decoded: {decoded}   decoded bytes: {decoded_bytes}");
    }

    if errors == 0 {
        Ok(())
    } else {
        Err(Error::document(format!("{errors} object(s) failed")))
    }
}
