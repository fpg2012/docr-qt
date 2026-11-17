use std::process::ExitCode;

use pindf::{Dictionary, Document, Error, Modification, Object, Ref};

fn main() -> ExitCode {
    let args: Vec<String> = std::env::args().collect();
    if args.len() != 3 {
        eprintln!("usage: pindf-outline <input.pdf> <output.pdf>");
        return ExitCode::FAILURE;
    }
    match run(&args[1], &args[2]) {
        Ok(()) => ExitCode::SUCCESS,
        Err(error) => {
            eprintln!("pindf-outline: {error}");
            ExitCode::FAILURE
        }
    }
}

/// Append a single outline item to the PDF, mirroring the C
/// `doc_save_modif_test` example.
fn run(input: &str, output: &str) -> pindf::Result<()> {
    let mut doc = Document::open(input)?;

    let root_ref = doc
        .trailer()
        .get("Root")
        .and_then(Object::as_ref)
        .ok_or_else(|| Error::document("trailer has no /Root reference"))?;
    let mut root = resolve_dict(&mut doc, root_ref)?;

    let pages_ref = root
        .get("Pages")
        .and_then(Object::as_ref)
        .ok_or_else(|| Error::document("catalog has no /Pages reference"))?;
    let page_ref =
        first_page(&mut doc, pages_ref)?.ok_or_else(|| Error::document("could not find a page"))?;

    let mut next_obj = (doc.xref().size() as u32)
        .saturating_sub(1)
        .max(doc.modification().max_obj_num().unwrap_or(0));

    let mut modif = Modification::new();

    // Reuse /Outlines if it exists, otherwise create it and update the catalog.
    let (outlines_num, outlines_gen, mut outlines) =
        match root.get("Outlines").and_then(Object::as_ref) {
            Some(reference) => (
                reference.num,
                reference.gen,
                resolve_dict(&mut doc, reference)?,
            ),
            None => {
                next_obj += 1;
                let num = next_obj;
                let mut outlines = Dictionary::new();
                outlines.insert("Type", Object::name("Outlines"));
                outlines.insert("Count", Object::Int(0));
                modif.add(num, Object::Dict(outlines.clone()));

                root.insert("Outlines", Object::Reference(Ref::new(num, 0)));
                modif.add(root_ref.num, Object::Dict(root));
                (num, 0, outlines)
            }
        };

    // New outline item.
    next_obj += 1;
    let item_num = next_obj;
    let mut item = Dictionary::new();
    item.insert("Title", Object::text("Random Outline"));
    item.insert(
        "Parent",
        Object::Reference(Ref::new(outlines_num, outlines_gen)),
    );
    item.insert(
        "Dest",
        Object::Array(vec![
            Object::Reference(page_ref),
            Object::name("XYZ"),
            Object::Int(0),
            Object::Int(0),
            Object::Int(0),
        ]),
    );

    // Link the previous last sibling to the new item.
    if let Some(last_ref) = outlines.get("Last").and_then(Object::as_ref) {
        let mut last = resolve_dict(&mut doc, last_ref)?;
        last.insert("Next", Object::Reference(Ref::new(item_num, 0)));
        item.insert("Prev", Object::Reference(last_ref));
        modif.add(last_ref.num, Object::Dict(last));
    }

    outlines.insert("Last", Object::Reference(Ref::new(item_num, 0)));
    if !outlines.contains_key("First") {
        outlines.insert("First", Object::Reference(Ref::new(item_num, 0)));
    }
    let count = outlines.get_int("Count").unwrap_or(0);
    outlines.insert(
        "Count",
        Object::Int(if count < 0 { count - 1 } else { count + 1 }),
    );

    modif.add(item_num, Object::Dict(item));
    modif.add(outlines_num, Object::Dict(outlines));
    doc.set_modification(modif);
    doc.save(output)?;

    println!(
        "added outline item {item_num} to {output} ({} changed objects)",
        doc.modification().len()
    );
    Ok(())
}

fn resolve_dict(doc: &mut Document, reference: Ref) -> pindf::Result<Dictionary> {
    doc.resolve(reference)?
        .as_dict()
        .cloned()
        .ok_or_else(|| Error::document(format!("object {} is not a dictionary", reference.num)))
}

fn first_page(doc: &mut Document, mut pages: Ref) -> pindf::Result<Option<Ref>> {
    for _ in 0..64 {
        let dict = resolve_dict(doc, pages)?;
        let Some(kid) = dict
            .get_array("Kids")
            .and_then(|kids| kids.first())
            .and_then(Object::as_ref)
        else {
            return Ok(None);
        };
        let kid_dict = resolve_dict(doc, kid)?;
        match kid_dict.get_name("Type") {
            Some(b"Page") => return Ok(Some(kid)),
            Some(b"Pages") => pages = kid,
            _ => return Ok(None),
        }
    }
    Ok(None)
}
