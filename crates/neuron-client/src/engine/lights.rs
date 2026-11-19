//! The engine lights' registered list (`engine_light.cpp`): the lights the
//! scene's light pass found near what it draws are linked into one list
//! and enabled on the pipeline (`LinkIntoRegisteredList`, from
//! `EngineLightApplyNearby`); `EngineLightClearRegisteredList` switches
//! every linked light off and empties the list. `Win3DBeginDraw` and a
//! static control's draw clear it, so the interface is never lit by the
//! scene's lights.
//!
//! A light is its key on the device's light manager (the reference keys the
//! pipeline's light by the address of its `D3DLIGHT8`; divergence D205).
//! The light pass that links the space scene's and the rooms' lights is
//! the game's (P5.4, P5.9); until then the list stays empty.

use crate::render::ff::lights::Lights;

/// `G_LightListHead` to `G_LightListTail` with `G_LightListCount`: the
/// linked lights in link order.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct EngineLights {
    linked: Vec<u64>,
}

impl EngineLights {
    /// The linked lights in link order.
    #[must_use]
    pub fn linked(&self) -> &[u64] {
        &self.linked
    }

    /// `LinkIntoRegisteredList`: the light appended and enabled (nothing for
    /// a light already linked: its flag bit 4 is set).
    pub fn link(&mut self, lights: &mut Lights, key: u64) {
        if self.linked.contains(&key) {
            return;
        }
        self.linked.push(key);
        lights.set_light_enable(key, true);
    }

    /// `UnlinkFromList`: the light out of the list, its enable untouched.
    pub fn unlink(&mut self, key: u64) {
        self.linked.retain(|&k| k != key);
    }

    /// `EngineLightClearRegisteredList`: every linked light disabled in
    /// list order, the list emptied.
    pub fn clear_registered(&mut self, lights: &mut Lights) {
        for key in self.linked.drain(..) {
            lights.set_light_enable(key, false);
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn the_clear_disables_every_linked_light() {
        let mut lights = Lights::default();
        let mut list = EngineLights::default();
        list.link(&mut lights, 7);
        list.link(&mut lights, 9);
        list.link(&mut lights, 7);
        assert_eq!(list.linked(), [7, 9]);
        assert!(lights.light_enable(7) && lights.light_enable(9));
        list.clear_registered(&mut lights);
        assert!(list.linked().is_empty());
        assert!(!lights.light_enable(7) && !lights.light_enable(9));
    }
}
