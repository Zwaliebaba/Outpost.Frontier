//! `CLoot` (`CppSource/Common/common_cloot.cpp`): a container floating in
//! space with its contents.

use super::projectile::itoa_radix24;

/// `CLoot` (+0xe4..+0x104).
#[derive(Clone, Debug, Default, PartialEq)]
pub struct LootData {
    /// +0xe4 the owner's id (0 none).
    pub owner_id: u32,
    /// +0xe8 the name override (0: the contents' name).
    pub name_id: i32,
    /// +0xec the contents' archetype id.
    pub contents_arch: u32,
    /// +0xf0 the units.
    pub units: u32,
    /// +0xf4 the contents' hit points.
    pub contents_hit_pts: f32,
    /// +0xf8 the time only the owner may take it.
    pub owner_safe_time: f32,
    /// +0xfc AI ships may tractor it.
    pub can_ai_tractor: bool,
    /// +0xfd mission loot.
    pub temporary: bool,
}

impl LootData {
    /// `CLoot::init`'s own part: an owned container starts its safe time.
    #[must_use]
    pub fn new(
        owner_id: u32,
        contents_arch: u32,
        units: u32,
        contents_hit_pts: f32,
        owner_safe_time: f32,
    ) -> Self {
        Self {
            owner_id,
            name_id: 0,
            contents_arch,
            units,
            contents_hit_pts,
            owner_safe_time: if owner_id != 0 { owner_safe_time } else { 0.0 },
            can_ai_tractor: true,
            temporary: false,
        }
    }

    /// The body's first collision group: the owner's id, else its own
    /// (`_itoa(..., 24)`).
    #[must_use]
    pub fn body_name(&self, id: u32) -> Vec<u8> {
        itoa_radix24(if self.owner_id != 0 {
            self.owner_id
        } else {
            id
        })
    }

    /// `CLoot::update`'s safe time: the new collision group (its own id)
    /// once the owner's time runs out.
    pub fn update(&mut self, dt: f32, id: u32) -> Option<Vec<u8>> {
        if self.owner_safe_time > 0.0 {
            self.owner_safe_time -= dt;
            if self.owner_safe_time <= 0.0 {
                return Some(itoa_radix24(id));
            }
        }
        None
    }

    /// `get_volume`: units times the contents' volume.
    #[must_use]
    pub fn volume(&self, contents_volume: f32) -> f32 {
        (f64::from(self.units) * f64::from(contents_volume)) as f32
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn owner_time_hands_the_group_back() {
        let mut l = LootData::new(7, 1, 3, 10.0, 0.5);
        assert_eq!(l.body_name(99), b"7");
        assert_eq!(l.update(0.25, 99), None);
        assert_eq!(l.update(0.25, 99), Some(b"43".to_vec()));
        assert_eq!(l.update(0.25, 99), None);
        assert_eq!(LootData::new(0, 1, 1, 1.0, 0.5).owner_safe_time, 0.0);
        assert_eq!(l.volume(2.5), 7.5);
    }
}
