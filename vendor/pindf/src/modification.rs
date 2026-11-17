//! Incremental modification set.
//!
//! The C version keeps a sorted singly-linked list with a dummy head.  A
//! `BTreeMap` keyed by object number provides the same ordering, replacement
//! and range iteration without manual node ownership.

use std::collections::btree_map;

use crate::object::{IndirectObject, Object};

#[derive(Clone, Debug, Default)]
pub struct Modification {
    objects: std::collections::BTreeMap<u32, IndirectObject>,
}

impl Modification {
    pub fn new() -> Self {
        Self::default()
    }

    /// Insert or replace the object with the same number.
    pub fn insert(&mut self, ind: IndirectObject) -> Option<IndirectObject> {
        self.objects.insert(ind.num, ind)
    }

    /// Insert an object by number (generation 0).
    pub fn add(&mut self, num: u32, object: Object) -> Option<IndirectObject> {
        self.insert(IndirectObject::new(num, object))
    }

    pub fn remove(&mut self, num: u32) -> Option<IndirectObject> {
        self.objects.remove(&num)
    }

    pub fn get(&self, num: u32) -> Option<&IndirectObject> {
        self.objects.get(&num)
    }

    pub fn get_mut(&mut self, num: u32) -> Option<&mut IndirectObject> {
        self.objects.get_mut(&num)
    }

    pub fn contains(&self, num: u32) -> bool {
        self.objects.contains_key(&num)
    }

    pub fn len(&self) -> usize {
        self.objects.len()
    }

    pub fn is_empty(&self) -> bool {
        self.objects.is_empty()
    }

    pub fn max_obj_num(&self) -> Option<u32> {
        self.objects.keys().next_back().copied()
    }

    /// Objects in ascending object-number order.
    pub fn iter(&self) -> btree_map::Iter<'_, u32, IndirectObject> {
        self.objects.iter()
    }

    pub fn clear(&mut self) {
        self.objects.clear();
    }
}
