//! Byte source for a document.
//!
//! A document parsed from memory keeps its `Vec<u8>`; a document opened from a
//! path keeps only a file handle and reads windows on demand, mirroring the C
//! version's `fseek`/`fread` behaviour instead of loading the whole file.

use std::borrow::Cow;
use std::cell::RefCell;
use std::fs::File;
use std::io::{Read, Seek, SeekFrom, Write};
use std::path::Path;

use crate::error::{Error, Result};

const COPY_CHUNK: usize = 1024 * 1024;

pub(crate) enum Source {
    Memory(Vec<u8>),
    File { file: RefCell<File>, len: u64 },
}

impl Source {
    pub(crate) fn from_bytes(data: Vec<u8>) -> Self {
        Source::Memory(data)
    }

    pub(crate) fn open(path: impl AsRef<Path>) -> Result<Self> {
        let file = File::open(path)?;
        let len = file.metadata()?.len();
        Ok(Source::File {
            file: RefCell::new(file),
            len,
        })
    }

    pub(crate) fn len(&self) -> u64 {
        match self {
            Source::Memory(data) => data.len() as u64,
            Source::File { len, .. } => *len,
        }
    }

    /// Read at most `max_len` bytes starting at `offset`.
    ///
    /// Memory-backed documents return a borrowed slice; file-backed documents
    /// read exactly that window.  The returned buffer is shorter only at EOF.
    pub(crate) fn read_range(&self, offset: u64, max_len: usize) -> Result<Cow<'_, [u8]>> {
        let len = self.len();
        if offset > len {
            return Err(Error::document(format!(
                "offset {offset} is past the end of the file (length {len})"
            )));
        }
        let available = (len - offset).min(max_len as u64) as usize;
        match self {
            Source::Memory(data) => {
                let start = offset as usize;
                Ok(Cow::Borrowed(&data[start..start + available]))
            }
            Source::File { file, .. } => {
                let mut buffer = vec![0u8; available];
                let mut file = file.borrow_mut();
                file.seek(SeekFrom::Start(offset))?;
                file.read_exact(&mut buffer)?;
                Ok(Cow::Owned(buffer))
            }
        }
    }

    /// Read the complete source into memory.
    pub(crate) fn read_all(&self) -> Result<Vec<u8>> {
        let mut out = Vec::with_capacity(self.len().min(usize::MAX as u64) as usize);
        let mut offset = 0u64;
        while offset < self.len() {
            let chunk = self.read_range(offset, COPY_CHUNK)?;
            if chunk.is_empty() {
                break;
            }
            out.extend_from_slice(&chunk);
            offset += chunk.len() as u64;
        }
        Ok(out)
    }

    /// Copy the source to `out` in bounded chunks (used by `save`).
    pub(crate) fn copy_to(&self, out: &mut impl Write) -> Result<()> {
        let mut offset = 0u64;
        while offset < self.len() {
            let chunk = self.read_range(offset, COPY_CHUNK)?;
            if chunk.is_empty() {
                break;
            }
            out.write_all(&chunk)?;
            offset += chunk.len() as u64;
        }
        Ok(())
    }
}
