//! PDF object model.
//!
//! Every object owns its children: arrays are `Vec<Object>`, dictionaries own
//! both keys and values, streams own their raw bytes.  Consequently `Clone`
//! is a deep copy and dropping an object frees the whole subtree; there is no
//! manual `destroy` step and no aliasing between the trailer and the cache.

use std::collections::BTreeMap;

/// A PDF name, stored **without** the leading `/` and without `#XX` escapes.
pub type Name = Vec<u8>;

/// A PDF dictionary.
///
/// Keys are names without the leading `/`; values are arbitrary objects.
/// `BTreeMap` gives duplicate-key replacement, deterministic iteration and
/// `O(log n)` lookup without keeping two parallel vectors in sync.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct Dictionary(BTreeMap<Name, Object>);

impl Dictionary {
    pub fn new() -> Self {
        Self(BTreeMap::new())
    }

    pub fn get(&self, key: impl AsRef<[u8]>) -> Option<&Object> {
        self.0.get(key.as_ref())
    }

    pub fn get_mut(&mut self, key: impl AsRef<[u8]>) -> Option<&mut Object> {
        self.0.get_mut(key.as_ref())
    }

    /// Insert a key/value pair, returning the previous value if any.
    pub fn insert(&mut self, key: impl AsRef<[u8]>, value: Object) -> Option<Object> {
        self.0.insert(key.as_ref().to_vec(), value)
    }

    pub fn remove(&mut self, key: impl AsRef<[u8]>) -> Option<Object> {
        self.0.remove(key.as_ref())
    }

    pub fn contains_key(&self, key: impl AsRef<[u8]>) -> bool {
        self.0.contains_key(key.as_ref())
    }

    pub fn len(&self) -> usize {
        self.0.len()
    }

    pub fn is_empty(&self) -> bool {
        self.0.is_empty()
    }

    pub fn iter(&self) -> impl Iterator<Item = (&Name, &Object)> {
        self.0.iter()
    }

    pub fn keys(&self) -> impl Iterator<Item = &Name> {
        self.0.keys()
    }

    pub fn get_int(&self, key: impl AsRef<[u8]>) -> Option<i64> {
        self.get(key)?.as_int()
    }

    pub fn get_name(&self, key: impl AsRef<[u8]>) -> Option<&[u8]> {
        self.get(key)?.as_name()
    }

    pub fn get_dict(&self, key: impl AsRef<[u8]>) -> Option<&Dictionary> {
        self.get(key)?.as_dict()
    }

    pub fn get_array(&self, key: impl AsRef<[u8]>) -> Option<&[Object]> {
        self.get(key)?.as_array()
    }
}

impl FromIterator<(Name, Object)> for Dictionary {
    fn from_iter<T: IntoIterator<Item = (Name, Object)>>(iter: T) -> Self {
        Self(iter.into_iter().collect())
    }
}

impl IntoIterator for Dictionary {
    type Item = (Name, Object);
    type IntoIter = std::collections::btree_map::IntoIter<Name, Object>;

    fn into_iter(self) -> Self::IntoIter {
        self.0.into_iter()
    }
}

impl<'a> IntoIterator for &'a Dictionary {
    type Item = (&'a Name, &'a Object);
    type IntoIter = std::collections::btree_map::Iter<'a, Name, Object>;

    fn into_iter(self) -> Self::IntoIter {
        self.0.iter()
    }
}

/// An indirect reference `num gen R`.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub struct Ref {
    pub num: u32,
    pub gen: u16,
}

impl Ref {
    pub fn new(num: u32, gen: u16) -> Self {
        Self { num, gen }
    }
}

/// A stream: a dictionary plus the raw (still encoded) payload.
#[derive(Clone, Debug, PartialEq)]
pub struct Stream {
    pub dict: Dictionary,
    /// Raw stream bytes, exactly `/Length` bytes (or up to `endstream`).
    pub data: Vec<u8>,
    /// Offset of the first data byte in the source file.
    pub data_offset: u64,
}

impl Stream {
    pub fn new(dict: Dictionary, data: Vec<u8>, data_offset: u64) -> Self {
        Self {
            dict,
            data,
            data_offset,
        }
    }
}

/// An indirect object definition `num gen obj ... endobj`.
#[derive(Clone, Debug, PartialEq)]
pub struct IndirectObject {
    pub num: u32,
    pub gen: u16,
    pub object: Object,
}

impl IndirectObject {
    pub fn new(num: u32, object: Object) -> Self {
        Self {
            num,
            gen: 0,
            object,
        }
    }
}

/// A direct PDF object.
#[derive(Clone, Debug, PartialEq)]
pub enum Object {
    Null,
    Bool(bool),
    Int(i64),
    Real(f64),
    /// Name; the leading `/` has been stripped and `#XX` escapes decoded.
    Name(Name),
    /// Literal string `(...)`; escapes have been decoded.
    String(Vec<u8>),
    /// Hexadecimal string `<...>`; decoded to bytes.
    HexString(Vec<u8>),
    Array(Vec<Object>),
    Dict(Dictionary),
    Stream(Stream),
    Reference(Ref),
}

impl Object {
    pub fn int(value: i64) -> Self {
        Object::Int(value)
    }

    pub fn name(value: impl AsRef<[u8]>) -> Self {
        Object::Name(value.as_ref().to_vec())
    }

    /// A literal string from text bytes.
    pub fn text(value: impl AsRef<[u8]>) -> Self {
        Object::String(value.as_ref().to_vec())
    }

    pub fn as_int(&self) -> Option<i64> {
        match self {
            Object::Int(v) => Some(*v),
            _ => None,
        }
    }

    pub fn as_real(&self) -> Option<f64> {
        match self {
            Object::Real(v) => Some(*v),
            Object::Int(v) => Some(*v as f64),
            _ => None,
        }
    }

    pub fn as_bool(&self) -> Option<bool> {
        match self {
            Object::Bool(v) => Some(*v),
            _ => None,
        }
    }

    pub fn as_name(&self) -> Option<&[u8]> {
        match self {
            Object::Name(v) => Some(v),
            _ => None,
        }
    }

    /// Bytes of a literal or hexadecimal string.
    pub fn as_bytes(&self) -> Option<&[u8]> {
        match self {
            Object::String(v) | Object::HexString(v) => Some(v),
            _ => None,
        }
    }

    pub fn as_array(&self) -> Option<&[Object]> {
        match self {
            Object::Array(v) => Some(v),
            _ => None,
        }
    }

    pub fn as_dict(&self) -> Option<&Dictionary> {
        match self {
            Object::Dict(d) => Some(d),
            _ => None,
        }
    }

    pub fn as_stream(&self) -> Option<&Stream> {
        match self {
            Object::Stream(s) => Some(s),
            _ => None,
        }
    }

    pub fn as_stream_mut(&mut self) -> Option<&mut Stream> {
        match self {
            Object::Stream(s) => Some(s),
            _ => None,
        }
    }

    /// The reference target, if this object is an indirect reference.
    pub fn as_ref(&self) -> Option<Ref> {
        match self {
            Object::Reference(r) => Some(*r),
            _ => None,
        }
    }

    pub fn is_null(&self) -> bool {
        matches!(self, Object::Null)
    }

    pub fn type_name(&self) -> &'static str {
        match self {
            Object::Null => "null",
            Object::Bool(_) => "boolean",
            Object::Int(_) => "integer",
            Object::Real(_) => "real",
            Object::Name(_) => "name",
            Object::String(_) => "string",
            Object::HexString(_) => "hex string",
            Object::Array(_) => "array",
            Object::Dict(_) => "dictionary",
            Object::Stream(_) => "stream",
            Object::Reference(_) => "reference",
        }
    }
}
