use std::process::ExitCode;

use pindf::{Document, Object};

fn main() -> ExitCode {
    let args: Vec<String> = std::env::args().collect();
    let decode = args.iter().any(|arg| arg == "--decode");
    let positional: Vec<&String> = args
        .iter()
        .skip(1)
        .filter(|a| !a.starts_with("--"))
        .collect();
    if positional.len() != 2 {
        eprintln!("usage: pindf-object <file.pdf> <object-number> [--decode]");
        return ExitCode::FAILURE;
    }
    let number = match positional[1].parse::<u32>() {
        Ok(n) => n,
        Err(_) => {
            eprintln!("invalid object number: {}", positional[1]);
            return ExitCode::FAILURE;
        }
    };
    match run(positional[0], number, decode) {
        Ok(()) => ExitCode::SUCCESS,
        Err(error) => {
            eprintln!("pindf-object: {error}");
            ExitCode::FAILURE
        }
    }
}

/// Read and print a single object without touching the rest of the file.
fn run(path: &str, number: u32, decode: bool) -> pindf::Result<()> {
    let mut doc = Document::open(path)?;
    println!("source length: {}", doc.source_len());

    let ind = doc.indirect_object(number)?;
    println!("{} {} obj", ind.num, ind.gen);
    match &ind.object {
        Object::Stream(stream) => {
            let filters = stream
                .filters()?
                .into_iter()
                .map(|f| String::from_utf8_lossy(&f).into_owned())
                .collect::<Vec<_>>();
            println!(
                "stream: data_offset={} raw_len={} filters={:?}",
                stream.data_offset,
                stream.data.len(),
                filters
            );
            if decode {
                let decoded = stream.decode()?;
                println!("decoded length: {}", decoded.len());
            }
            println!(
                "dict: {}",
                pindf::serialize::to_json(&Object::Dict(stream.dict.clone()))
            );
        }
        other => println!("{}", pindf::serialize::to_json(other)),
    }
    Ok(())
}
