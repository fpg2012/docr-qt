//! Error type shared by the whole crate.

use std::fmt;

/// Result alias used throughout the crate.
pub type Result<T> = std::result::Result<T, Error>;

/// Everything that can go wrong while parsing or writing a PDF.
#[derive(Debug)]
pub enum Error {
    /// Underlying I/O error (opening a file, ...).
    Io(std::io::Error),
    /// Malformed PDF syntax at a byte offset of the source buffer.
    Syntax { offset: usize, message: String },
    /// Structurally valid syntax but an inconsistent document (bad xref, ...).
    Document(String),
    /// A feature that is recognized but intentionally not implemented.
    Unsupported(String),
}

impl Error {
    pub fn syntax(offset: usize, message: impl Into<String>) -> Self {
        Error::Syntax {
            offset,
            message: message.into(),
        }
    }

    pub fn document(message: impl Into<String>) -> Self {
        Error::Document(message.into())
    }

    pub fn unsupported(message: impl Into<String>) -> Self {
        Error::Unsupported(message.into())
    }

    /// Byte offset carried by a syntax error, if any.
    pub(crate) fn offset(&self) -> Option<usize> {
        match self {
            Error::Syntax { offset, .. } => Some(*offset),
            _ => None,
        }
    }

    /// Shift positions that were recorded relative to a read window to
    /// absolute file offsets.
    pub(crate) fn rebase(self, base: u64) -> Self {
        match self {
            Error::Syntax { offset, message } => Error::Syntax {
                offset: (base as usize).saturating_add(offset),
                message,
            },
            other => other,
        }
    }

    /// Whether an error could be caused by the parse window ending too early.
    ///
    /// The lexer/parser report EOF (and unterminated strings/streams) at the
    /// end of the buffer, so a failure close to the window boundary is retried
    /// with more data.  Errors in the middle are real syntax errors.
    pub(crate) fn is_near_end(&self, window_len: usize) -> bool {
        self.offset()
            .is_some_and(|offset| offset.saturating_add(128) >= window_len)
    }
}

impl fmt::Display for Error {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Error::Io(e) => write!(f, "I/O error: {e}"),
            Error::Syntax { offset, message } => {
                write!(f, "syntax error at byte {offset}: {message}")
            }
            Error::Document(message) => write!(f, "invalid document: {message}"),
            Error::Unsupported(message) => write!(f, "unsupported PDF feature: {message}"),
        }
    }
}

impl std::error::Error for Error {
    fn source(&self) -> Option<&(dyn std::error::Error + 'static)> {
        match self {
            Error::Io(e) => Some(e),
            _ => None,
        }
    }
}

impl From<std::io::Error> for Error {
    fn from(e: std::io::Error) -> Self {
        Error::Io(e)
    }
}
