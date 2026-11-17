use std::io::Write;
use std::process::ExitCode;

use pindf::serialize;
use pindf::Document;

fn main() -> ExitCode {
    let args: Vec<String> = std::env::args().collect();
    if args.len() < 2 || args.len() > 3 {
        eprintln!("usage: pindf-json <input.pdf> [output.json]");
        return ExitCode::FAILURE;
    }
    match run(&args[1], args.get(2).map(String::as_str)) {
        Ok(()) => ExitCode::SUCCESS,
        Err(error) => {
            eprintln!("pindf-json: {error}");
            ExitCode::FAILURE
        }
    }
}

fn run(input: &str, output: Option<&str>) -> pindf::Result<()> {
    let mut doc = Document::open(input)?;
    let mut json = String::new();
    json.push_str("{\"objs\":[");

    for num in 0..doc.xref().size() as u32 {
        if num > 0 {
            json.push_str(",\n");
        }
        match doc.indirect_object(num) {
            Ok(ind) => serialize::write_json_indirect(&mut json, ind),
            Err(_) => json.push_str("null"),
        }
    }

    json.push_str("],\n\"trailer\":");
    serialize::write_json_dictionary(&mut json, doc.trailer());
    json.push_str("}\n");

    match output {
        Some(path) => {
            let mut file = std::fs::File::create(path)?;
            file.write_all(json.as_bytes())?;
        }
        None => print!("{json}"),
    }
    Ok(())
}
