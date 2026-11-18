//! The global material records (`GGlobalMaterialRecords` and the functions
//! around it in `CppSource/Shading/MaterialLibrary.cpp`): a process-wide
//! array of eight-byte records, an active flag and a value, that the
//! performance options write and the materials read (record 0 enables the
//! detail passes, 1 the atmosphere, 2 the secondary texture; the
//! surface materials use 4).
//!
//! The reference keeps the array in a static that the first use creates
//! with five inactive records of value `0xFFFFFFFF`, and `Shading.dll`
//! activates records 2, 1, 3 and 0 (value 1) while it registers its
//! components. Here it is a value the caller owns and passes to the
//! materials (decision T11 makes process state explicit the same way for
//! the random generator).
//!
//! The reference reads and writes past the end of the array for an index
//! it has not grown to, and grows it without bound; here such a read or
//! write fails, and an index from [`MAX_RECORDS`] on is refused
//! (divergence D306).

/// The value of a record nobody has set.
pub const UNSET: u32 = 0xFFFF_FFFF;

/// Records the array starts with.
pub const INITIAL_RECORDS: usize = 5;

/// Indices from this one on are refused (the reference would try to
/// allocate up to 32 GiB).
pub const MAX_RECORDS: u32 = 0x0010_0000;

/// One record (`FGlobalMaterialRecord`).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct GlobalRecord {
    /// `bActive`: set by every write through [`GlobalRecords::set_record`]
    /// and [`GlobalRecords::activate`].
    pub active: bool,
    /// `Value`.
    pub value: u32,
}

const EMPTY: GlobalRecord = GlobalRecord {
    active: false,
    value: UNSET,
};

/// The record array.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct GlobalRecords {
    records: Vec<GlobalRecord>,
}

impl Default for GlobalRecords {
    fn default() -> Self {
        Self::new()
    }
}

impl GlobalRecords {
    /// The array as its first use leaves it (`GlobalMaterialRecords`):
    /// [`INITIAL_RECORDS`] inactive records of value [`UNSET`].
    #[must_use]
    pub fn new() -> GlobalRecords {
        GlobalRecords {
            records: vec![EMPTY; INITIAL_RECORDS],
        }
    }

    /// The array after `Shading.dll` registered its components
    /// (`RegisterMaterialComponents`): records 2, 1, 3 and 0 activated.
    #[must_use]
    pub fn registered() -> GlobalRecords {
        let mut records = GlobalRecords::new();
        records.initialize();
        records
    }

    /// The records, index 0 first.
    #[must_use]
    pub fn records(&self) -> &[GlobalRecord] {
        &self.records
    }

    /// Grows the array through `index` with inactive [`UNSET`] records
    /// (`ReserveGlobalRecords` and `ResizeGlobalRecords`). False for an
    /// index from [`MAX_RECORDS`] on.
    fn grow(&mut self, index: u32) -> bool {
        if index >= MAX_RECORDS {
            return false;
        }
        let index = index as usize;
        if self.records.len() <= index {
            self.records.resize(index + 1, EMPTY);
        }
        true
    }

    /// `ActivateMaterialGlobalRecord`: grows the array through `index`,
    /// then sets the record active with value 1.
    pub fn activate(&mut self, index: u32) -> bool {
        self.set_record(index, 1)
    }

    /// `InitializeMaterialGlobalRecords`: records 2, 1, 3 and 0 activated.
    pub fn initialize(&mut self) {
        for index in [2, 1, 3, 0] {
            self.activate(index);
        }
    }

    /// `FMaterialLibrary::SetGlobalRecord` (slot 23): grows the array
    /// through `index`, then sets the record active with `value`. False for
    /// an index the port refuses.
    pub fn set_record(&mut self, index: u32, value: u32) -> bool {
        if !self.grow(index) {
            return false;
        }
        self.records[index as usize] = GlobalRecord {
            active: true,
            value,
        };
        true
    }

    /// `FMaterialLibrary::SetGlobalRecordValue` (slot 24): replaces the
    /// value of an existing record; the active flag stays. False past the
    /// end (the reference writes there).
    pub fn set_value(&mut self, index: u32, value: u32) -> bool {
        match self.records.get_mut(index as usize) {
            Some(record) => {
                record.value = value;
                true
            }
            None => false,
        }
    }

    /// `FMaterialLibrary::GetGlobalRecordValue` (slot 25) and
    /// `GetMaterialGlobalRecordValue`: the value of an existing record;
    /// `None` past the end (the reference reads there).
    #[must_use]
    pub fn value(&self, index: u32) -> Option<u32> {
        self.records.get(index as usize).map(|r| r.value)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn registration_activates_four_records() {
        let records = GlobalRecords::registered();
        let values: Vec<_> = records
            .records()
            .iter()
            .map(|r| (r.active, r.value))
            .collect();
        assert_eq!(
            values,
            [(true, 1), (true, 1), (true, 1), (true, 1), (false, UNSET)]
        );
    }

    #[test]
    fn growth_fills_with_unset_records() {
        let mut records = GlobalRecords::new();
        assert!(records.set_record(9, 7));
        assert_eq!(records.records().len(), 10);
        assert_eq!(records.value(8), Some(UNSET));
        assert_eq!(records.value(9), Some(7));
        assert!(!records.set_value(10, 1));
        assert_eq!(records.value(10), None);
        assert!(records.set_value(8, 3));
        assert_eq!(
            records.records()[8],
            GlobalRecord {
                active: false,
                value: 3
            }
        );
        assert!(!records.set_record(MAX_RECORDS, 1));
    }
}
