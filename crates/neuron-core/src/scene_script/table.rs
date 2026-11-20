//! Lua 3.2 values and tables (`lobject.h`, `ltable.c` in
//! `CppSource/SceneScript/lua32/src`).
//!
//! A Lua 3.2 table has no array part: every entry lives in one node vector
//! sized from a list of primes, placed by double hashing. `foreach` visits
//! the nodes in vector order, so the order a script's entities and events
//! are built in follows this layout. It is reproduced exactly for keys
//! whose hash is their value, numbers. The reference hashes strings,
//! tables and functions by their address, which no port can reproduce; here
//! a string hashes by its content (`hash_s` of `lstring.c`), a table by its
//! index in the VM and a predefined function or file handle by its name
//! (divergence D200).

use std::rc::Rc;

use crate::num;

/// A table in a [`Vm`](super::Vm), by index.
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub struct TableId(pub(super) u32);

impl TableId {
    /// The index of the table in the VM.
    #[must_use]
    pub fn index(self) -> usize {
        self.0 as usize
    }
}

/// The kind of a value the reference VM predefines and a script can only
/// copy.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum NativeKind {
    /// A C function or closure of the base, math or I/O library.
    Function,
    /// A file handle of the I/O library.
    Userdata,
}

/// A predefined function or file handle, named by the global that holds it
/// (file handles by the stream).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Native {
    pub kind: NativeKind,
    pub name: &'static str,
}

/// A Lua 3.2 value.
#[derive(Clone, Debug, PartialEq)]
pub enum Value {
    Nil,
    Number(f64),
    /// Strings are interned in Lua, so equal bytes are the same string.
    String(Rc<[u8]>),
    Table(TableId),
    Native(Native),
}

impl Value {
    #[must_use]
    pub fn is_nil(&self) -> bool {
        matches!(self, Value::Nil)
    }

    #[must_use]
    pub fn as_table(&self) -> Option<TableId> {
        match self {
            Value::Table(id) => Some(*id),
            _ => None,
        }
    }

    #[must_use]
    pub fn as_number(&self) -> Option<f64> {
        match self {
            Value::Number(x) => Some(*x),
            _ => None,
        }
    }

    #[must_use]
    pub fn as_bytes(&self) -> Option<&[u8]> {
        match self {
            Value::String(s) => Some(s),
            _ => None,
        }
    }

    /// The name `type` gives the value.
    #[must_use]
    pub fn type_name(&self) -> &'static str {
        match self {
            Value::Nil => "nil",
            Value::Number(_) => "number",
            Value::String(_) => "string",
            Value::Table(_) => "table",
            Value::Native(Native {
                kind: NativeKind::Function,
                ..
            }) => "function",
            Value::Native(Native {
                kind: NativeKind::Userdata,
                ..
            }) => "userdata",
        }
    }

    pub(super) fn remap(&mut self, map: &[u32]) {
        if let Value::Table(id) = self {
            id.0 = map[id.index()];
        }
    }
}

/// `luaO_equalval` for two values of one type: numbers by value (NaN equals
/// nothing), strings by content, the rest by identity.
fn same_type(a: &Value, b: &Value) -> bool {
    match (a, b) {
        (Value::Native(x), Value::Native(y)) => x.kind == y.kind,
        _ => std::mem::discriminant(a) == std::mem::discriminant(b),
    }
}

/// `hash_s` of `lstring.c`, in 32-bit `unsigned long` arithmetic.
fn hash_bytes(s: &[u8]) -> u32 {
    s.iter().fold(0u32, |h, &b| {
        h ^ (h << 5).wrapping_add(h >> 2).wrapping_add(u32::from(b))
    })
}

/// The `hashindex` of a key: never negative. `None` for nil, which the
/// reference reports as "unexpected type to index table".
fn hash_index(key: &Value) -> Option<i64> {
    let h: i32 = match key {
        Value::Nil => return None,
        // `(long int)nvalue(ref)`: MSVC's `_ftol`.
        Value::Number(x) => num::trunc_to_i32_via_i64(*x),
        Value::String(s) => hash_bytes(s).cast_signed(),
        Value::Table(id) => id.0.cast_signed(),
        Value::Native(n) => hash_bytes(n.name.as_bytes()).cast_signed(),
    };
    let h = i64::from(h);
    Some(if h >= 0 { h } else { -(h + 1) })
}

/// `dimensions` of `lobject.c`.
const DIMENSIONS: [i64; 22] = [
    5, 11, 23, 47, 97, 197, 397, 797, 1597, 3203, 6421, 12853, 25717, 51437, 102811, 205619,
    411233, 822433, 1644817, 3289613, 6579211, 13158023,
];

/// `luaO_redimension`: the first size above `old`; `None` where the
/// reference raises "table overflow".
fn redimension(old: i64) -> Option<usize> {
    DIMENSIONS
        .iter()
        .find(|&&d| d > old)
        .map(|&d| usize::try_from(d).expect("dimensions are positive"))
}

/// Why a table store failed.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum TableError {
    /// The key is nil.
    NilKey,
    /// The table would exceed the largest size.
    Overflow,
}

#[derive(Clone, Debug)]
struct Node {
    key: Value,
    value: Value,
}

const EMPTY: Node = Node {
    key: Value::Nil,
    value: Value::Nil,
};

/// A Lua 3.2 table.
#[derive(Clone, Debug)]
pub struct Table {
    nodes: Vec<Node>,
    /// `nuse`: nodes with a key, including keys whose value is nil.
    used: usize,
}

impl Table {
    /// `luaH_new(n)`: room for `n` entries without growing.
    pub(super) fn new(n: i64) -> Result<Table, TableError> {
        let size = redimension(n * 3 / 2).ok_or(TableError::Overflow)?;
        Ok(Table {
            nodes: vec![EMPTY; size],
            used: 0,
        })
    }

    /// `luaH_present`: the node holding `key`, or the empty node where it
    /// would go.
    fn present(&self, key: &Value, h: i64) -> usize {
        let size = self.nodes.len() as i64;
        let mut at = h % size;
        loop {
            let node = &self.nodes[at as usize];
            let stop = if same_type(&node.key, key) {
                node.key == *key
            } else {
                node.key.is_nil()
            };
            if stop {
                return at as usize;
            }
            at += (h & (size - 2)) + 1;
            if at >= size {
                at -= size;
            }
        }
    }

    /// `luaH_get`: the value of `key`, nil when absent.
    #[must_use]
    pub fn get(&self, key: &Value) -> &Value {
        match hash_index(key) {
            Some(h) => &self.nodes[self.present(key, h)].value,
            None => &Value::Nil,
        }
    }

    /// The value of a string key.
    #[must_use]
    pub fn field(&self, key: &str) -> &Value {
        self.get(&Value::String(Rc::from(key.as_bytes())))
    }

    /// The value of an integer key.
    #[must_use]
    pub fn int(&self, i: i32) -> &Value {
        self.get(&Value::Number(f64::from(i)))
    }

    /// `luaH_set`.
    pub(super) fn set(&mut self, key: Value, value: Value) -> Result<(), TableError> {
        let h = hash_index(&key).ok_or(TableError::NilKey)?;
        let mut at = self.present(&key, h);
        if !self.nodes[at].key.is_nil() {
            self.nodes[at].value = value;
            return Ok(());
        }
        if self.used as i64 * 3 > self.nodes.len() as i64 * 2 {
            self.rehash()?;
            at = self.present(&key, h);
        }
        self.used += 1;
        self.nodes[at] = Node { key, value };
        Ok(())
    }

    /// `rehash`: a vector sized for the entries whose value is not nil,
    /// filled in the order of the old vector.
    fn rehash(&mut self) -> Result<(), TableError> {
        let live = self.nodes.iter().filter(|n| !n.value.is_nil()).count() as i64;
        let size = redimension((live + 1) * 2).ok_or(TableError::Overflow)?;
        let old = std::mem::replace(&mut self.nodes, vec![EMPTY; size]);
        self.used = 0;
        for node in old {
            if !node.value.is_nil() {
                let h = hash_index(&node.key).expect("stored keys are not nil");
                let at = self.present(&node.key, h);
                self.nodes[at] = node;
                self.used += 1;
            }
        }
        Ok(())
    }

    /// The entries whose value is not nil, in the order `foreach` visits
    /// them.
    pub fn entries(&self) -> impl Iterator<Item = (&Value, &Value)> {
        self.nodes
            .iter()
            .filter(|n| !n.value.is_nil())
            .map(|n| (&n.key, &n.value))
    }

    /// The size of the node vector (`nhash`).
    #[must_use]
    pub fn capacity(&self) -> usize {
        self.nodes.len()
    }

    pub(super) fn remap(&mut self, map: &[u32]) {
        for node in &mut self.nodes {
            node.key.remap(map);
            node.value.remap(map);
        }
    }

    pub(super) fn children(&self) -> impl Iterator<Item = TableId> {
        self.nodes
            .iter()
            .flat_map(|n| [n.key.as_table(), n.value.as_table()])
            .flatten()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn n(x: f64) -> Value {
        Value::Number(x)
    }

    fn keys(t: &Table) -> Vec<f64> {
        t.entries().map(|(k, _)| k.as_number().unwrap()).collect()
    }

    #[test]
    fn presized_lists_iterate_in_key_order() {
        // SETLIST stores the last item first; the layout is by value.
        let mut t = Table::new(3).unwrap();
        assert_eq!(t.capacity(), 5);
        for k in (1..=3).rev() {
            t.set(n(f64::from(k)), n(10.0 * f64::from(k))).unwrap();
        }
        assert_eq!(keys(&t), [1.0, 2.0, 3.0]);
        assert_eq!(t.int(2), &n(20.0));
        assert_eq!(t.int(4), &Value::Nil);
    }

    #[test]
    fn growth_follows_the_reference() {
        // An empty constructor gets 5 nodes; the fifth insertion rehashes
        // to redimension((4 + 1) * 2) = 11, filled in the old vector order.
        let mut t = Table::new(0).unwrap();
        for k in [7.0, 2.0, 12.0, 5.0, 3.0] {
            t.set(n(k), n(1.0)).unwrap();
        }
        assert_eq!(t.capacity(), 11);
        // 7 % 11, 2, 12 % 11 = 1, 5, 3.
        assert_eq!(keys(&t), [12.0, 2.0, 3.0, 5.0, 7.0]);
    }

    #[test]
    fn collisions_probe_by_double_hashing() {
        let mut t = Table::new(2).unwrap(); // 5 nodes
        t.set(n(1.0), n(1.0)).unwrap(); // node 1
        t.set(n(6.0), n(6.0)).unwrap(); // 6 % 5 = 1, step (6 & 3) + 1 = 3: node 4
        t.set(n(11.0), n(11.0)).unwrap(); // node 1, step 4: node 0
        assert_eq!(keys(&t), [11.0, 1.0, 6.0]);
    }

    #[test]
    fn nil_values_keep_their_key_until_a_rehash() {
        let mut t = Table::new(0).unwrap();
        t.set(n(1.0), Value::Nil).unwrap();
        assert_eq!(t.used, 1);
        assert_eq!(t.entries().count(), 0);
        assert_eq!(t.set(Value::Nil, n(1.0)), Err(TableError::NilKey));
    }

    #[test]
    fn negative_and_fractional_keys_hash_like_ftol() {
        assert_eq!(hash_index(&n(-1.0)), Some(0));
        assert_eq!(hash_index(&n(-2.5)), Some(1));
        assert_eq!(hash_index(&n(2.9)), Some(2));
        assert_eq!(hash_index(&n(f64::NAN)), Some(0));
        assert_eq!(hash_index(&n(4_294_967_296.0 + 3.0)), Some(3));
    }

    #[test]
    fn strings_hash_by_content() {
        assert_eq!(hash_bytes(b""), 0);
        assert_eq!(hash_bytes(b"a"), 97);
        let mut t = Table::new(1).unwrap();
        t.set(Value::String(Rc::from(&b"pos"[..])), n(1.0)).unwrap();
        assert_eq!(t.field("pos"), &n(1.0));
        assert_eq!(t.field("orient"), &Value::Nil);
    }
}
