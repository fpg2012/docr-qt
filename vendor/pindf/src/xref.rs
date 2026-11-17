//! Cross-reference table.
//!
//! `entries` is a flat array indexed by object number.  An entry is `None`
//! while no xref section covers that object.  Sections are remembered for
//! diagnostics and for `/Index` construction when saving.  Sections are parsed
//! newest-first, so "first write wins" is exactly the PDF incremental-update
//! rule (newer definitions override older ones).

/// One xref entry.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum XrefEntry {
    /// Free object (`f` entry, type 0 in an xref stream).
    Free { gen: u16 },
    /// Uncompressed object at `offset` (`n` entry, type 1).
    Normal { offset: u64, gen: u16 },
    /// Object stored inside an object stream (type 2).
    Compressed { stream_num: u32, index: u32 },
}

impl XrefEntry {
    pub fn gen(&self) -> u16 {
        match self {
            XrefEntry::Free { gen } | XrefEntry::Normal { gen, .. } => *gen,
            XrefEntry::Compressed { .. } => 0,
        }
    }
}

/// A contiguous run of entries (`first_obj`, `len`) as written in an xref
/// subsection or in `/Index`.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct XrefSection {
    pub first_obj: u32,
    pub len: u32,
}

#[derive(Clone, Debug, Default)]
pub struct XRef {
    entries: Vec<Option<XrefEntry>>,
    sections: Vec<XrefSection>,
    /// `/Size` from the newest trailer; may be larger than `entries.len()`.
    size: usize,
}

impl XRef {
    pub fn new(size: usize) -> Self {
        Self {
            entries: Vec::new(),
            sections: Vec::new(),
            size,
        }
    }

    /// Declared number of objects (`/Size`), at least as large as the number of
    /// entries actually seen.
    pub fn size(&self) -> usize {
        self.size.max(self.entries.len())
    }

    pub fn is_empty(&self) -> bool {
        self.sections.is_empty()
    }

    pub fn sections(&self) -> &[XrefSection] {
        &self.sections
    }

    pub fn get(&self, num: u32) -> Option<XrefEntry> {
        self.entries.get(num as usize).copied().flatten()
    }

    pub fn contains(&self, num: u32) -> bool {
        self.get(num).is_some()
    }

    /// Record a section header.  Called while parsing, before its entries.
    pub fn add_section(&mut self, first_obj: u32, len: u32) {
        self.sections.push(XrefSection { first_obj, len });
        self.size = self.size.max(first_obj as usize + len as usize);
    }

    /// Set an entry unless a newer section already defined it.
    ///
    /// Returns `true` if the entry was stored.
    pub fn set(&mut self, num: u32, entry: XrefEntry) -> bool {
        let idx = num as usize;
        if idx + 1 > self.entries.len() {
            self.entries.resize(idx + 1, None);
        }
        if self.entries[idx].is_some() {
            return false;
        }
        self.entries[idx] = Some(entry);
        self.size = self.size.max(idx + 1);
        true
    }

    /// Reserve space for `size` objects without allocating entries eagerly.
    pub fn set_declared_size(&mut self, size: usize) {
        self.size = self.size.max(size);
    }

    /// Iterate over all defined entries with their object numbers.
    pub fn iter(&self) -> impl Iterator<Item = (u32, XrefEntry)> + '_ {
        self.entries
            .iter()
            .enumerate()
            .filter_map(|(i, e)| e.map(|e| (i as u32, e)))
    }
}
