//! An equipped object's items as `EquipDesc`s (`CEquip::GetEquipDesc` and
//! its overrides, `Common/common_cequip.cpp:382`, `common_cecargo.cpp:178`,
//! `common_ceshield.cpp:259`), which the server's loot drops and Content
//! read.

use crate::data::equip_desc::EquipDesc;
use crate::equip::{EquipKind, SubObjId};
use crate::object::ObjHandle;

use super::World;

impl World {
    /// `GetEquipDesc` of one item of an object:
    /// - a shield gives none;
    /// - a cargo stack gives none when empty, else its stack count, its
    ///   health as hit points over the archetype's (`HitPts /
    ///   Arch->HitPts`), not equipped, and its pod's hardpoint or the cargo
    ///   bay;
    /// - any other item gives none when destroyed, else count 1, equipped,
    ///   its relative health, and its mount's hardpoint for an external
    ///   item or the cargo bay for an internal one.
    pub fn equip_desc(&mut self, h: ObjHandle, id: SubObjId) -> Option<EquipDesc> {
        let eq = self.objects.get(h)?.equipped.as_deref()?;
        let e = eq.equipment.by_id(id)?;
        let mut desc = EquipDesc::new(&self.strings);
        desc.id = e.id;
        desc.arch_id = e.arch_id;
        desc.mission = e.mission;
        let hardpoint: Option<Vec<u8>> = match &e.kind {
            EquipKind::Shield(_) => return None,
            EquipKind::Cargo(c) => {
                if c.count == 0 {
                    return None;
                }
                let arch_hit_pts = self
                    .data
                    .archetypes
                    .equipment
                    .get(&e.arch_id)
                    .map_or(0.0, |a| a.root.hit_pts);
                desc.health = (f64::from(c.hit_pts) / f64::from(arch_hit_pts)) as f32;
                desc.count = i32::try_from(c.count).unwrap_or(i32::MAX);
                desc.equipped = 0;
                c.pod
                    .and_then(|pod| eq.equipment.by_id(pod))
                    .and_then(|pod| pod.mount.as_ref())
                    .map(|m| m.hardpoint.clone())
            }
            _ => {
                if e.is_destroyed() {
                    return None;
                }
                desc.health = e.relative_health() as f32;
                desc.count = 1;
                desc.equipped = 1;
                // `CExternalEquip` has a hardpoint; `CInternalEquip` is in
                // the cargo bay (`make_internal`).
                e.mount.as_ref().map(|m| m.hardpoint.clone())
            }
        };
        desc.hardpoint = match hardpoint {
            Some(name) => self.strings.alloc(name.as_slice(), false),
            None => self.strings.cargo_bay(),
        };
        Some(desc)
    }
}
