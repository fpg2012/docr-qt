//! `pindf` - a small PDF parsing and incremental-update library.
//!
//! This is a Rust re-implementation of the C library `libpindf`.  The overall
//! pipeline is the same (lexer -> object parser -> xref/doc -> lazy loading),
//! but ownership is handled by the type system:
//!
//! * [`Object`] is a tree that owns its children; `Clone` is a deep copy and
//!   dropping the document frees everything.
//! * [`Document`] owns the file bytes and the object cache; parsing state
//!   (lexer/parser) never escapes the call that needs it.
//! * Errors are returned as [`Error`] with byte offsets instead of negative
//!   codes and out-parameters.
//!
//! # Example
//!
//! ```no_run
//! use pindf::{Document, Object};
//!
//! let mut doc = Document::open("input.pdf")?;
//! println!("version: {}", doc.version());
//! if let Some(root) = doc.trailer().get("Root").and_then(Object::as_ref) {
//!     let catalog = doc.resolve(root)?;
//!     println!("catalog: {:?}", catalog);
//! }
//! # Ok::<(), pindf::Error>(())
//! ```

pub mod document;
pub mod error;
pub mod lexer;
pub mod modification;
pub mod object;
pub mod parser;
pub mod serialize;
pub mod stream;
pub mod xref;

mod source;
mod util;

pub use document::Document;
pub use error::{Error, Result};
pub use modification::Modification;
pub use object::{Dictionary, IndirectObject, Object, Ref, Stream};
pub use xref::{XRef, XrefEntry, XrefSection};

/// Current crate version.
pub const VERSION: &str = env!("CARGO_PKG_VERSION");
