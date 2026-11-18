//! The levels of detail of a render archetype (`FLodSet`, `LodSetResize`,
//! `LodSetAssign` and `LodSetPick` in `RenderManager.cpp`).

use super::{HANDLE_NONE, Handle};

/// One level: the plugin's handle and the plugin (`FLodEntry`). A level
/// no plugin has claimed yet is `{HANDLE_NONE, None}`.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct LodEntry {
    /// The handle the plugin's `Claim` returned.
    pub handle: Handle,
    /// The plugin's index in the render manager's list.
    pub plugin: Option<usize>,
}

impl LodEntry {
    /// The entry `LodSetResize` seeds.
    pub const EMPTY: Self = Self {
        handle: HANDLE_NONE,
        plugin: None,
    };
}

/// An archetype's levels (`FLodSet`): `entries.len()` is `LodCount`,
/// `switch` holds one distance more.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct LodSet {
    /// The levels.
    pub entries: Vec<LodEntry>,
    /// The level drawn last; `entries.len()` for none.
    pub current: i32,
    /// `bFlag`: the root level selection (`SetArchetypeRootLodSelection`).
    pub root_selection: bool,
    /// The switch distances: level `i` is drawn up to `switch[i + 1]`.
    pub switch: Vec<f32>,
}

impl LodSet {
    /// The level count (`LodCount`).
    #[must_use]
    pub fn count(&self) -> i32 {
        i32::try_from(self.entries.len()).unwrap_or(i32::MAX)
    }

    /// `LodSetResize`: both arrays dropped, the current level 0, then
    /// `count` empty entries and `count + 1` switch distances. The
    /// reference leaves the distances uninitialised; they are 0 here
    /// (divergence D352).
    pub fn resize(&mut self, count: usize) {
        self.entries.clear();
        self.switch.clear();
        self.current = 0;
        if count == 0 {
            return;
        }
        self.entries = vec![LodEntry::EMPTY; count];
        self.switch = vec![0.0; count + 1];
    }

    /// `LodSetAssign`: the source's levels, distances and flag.
    pub fn assign(&mut self, source: &LodSet) {
        self.resize(source.entries.len());
        if !self.entries.is_empty() {
            self.entries.copy_from_slice(&source.entries);
            self.switch.copy_from_slice(&source.switch);
        }
        self.root_selection = source.root_selection;
    }

    /// `LodSetPick`: none for a negative distance; else the first level
    /// whose upper distance is at least `distance` (a NaN matches none).
    #[must_use]
    pub fn pick(&self, distance: f32) -> Option<usize> {
        if distance < 0.0 {
            return None;
        }
        (0..self.entries.len()).find(|&index| self.switch[index + 1] >= distance)
    }
}

#[cfg(test)]
mod tests {
    use super::LodSet;

    #[test]
    fn pick_takes_the_first_upper_distance_reached() {
        let mut set = LodSet::default();
        set.resize(2);
        set.switch.copy_from_slice(&[0.0, 10.0, 20.0]);
        assert_eq!(set.pick(-0.5), None);
        assert_eq!(set.pick(0.0), Some(0));
        assert_eq!(set.pick(10.0), Some(0));
        assert_eq!(set.pick(10.5), Some(1));
        assert_eq!(set.pick(20.5), None);
        assert_eq!(set.pick(f32::NAN), None);
    }
}
