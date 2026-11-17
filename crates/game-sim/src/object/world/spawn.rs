//! Object creation: `CObject::Alloc` and `open`, the kinds' `init` and
//! `init_physics`, `CEqObj::init` (collision groups, equipment from the
//! loadout, shields, power), `CShip::init` (controllers, guns, thrust
//! power) and `CSolar::init`; equipment mounting (`CExternalEquip::Connect`,
//! `CAttachedEquip::Connect`, the barrels of `CELauncher::Connect`, the
//! nozzles of `CEEngine::CEEngine`).

use neuron_core::hash::name_crc32;
use neuron_core::math::{Matrix, Vector};
use neuron_core::physics::{BodyDesc, CONTROLLER_PRIORITY, CreateParms, Geometry};
use neuron_core::scene::{InstanceId, connect_hardpoints};

use crate::data::archetype::equipment::{Equipment, EquipmentKind};
use crate::data::archetype::root::Root;
use crate::equip::flight::Nozzle;
use crate::equip::launcher::Barrel;
use crate::equip::shield::{Shield, ShieldStatus};
use crate::equip::{Attachment, Equip, EquipKind, SubObjId, types};
use crate::object::csys::rotate_world_to_object;
use crate::object::projectile::{
    BeamData, CounterMeasureData, GuidedData, MineData, Projectile, drag_damping, itoa_radix24,
};
use crate::object::ship::ShipData;
use crate::object::solar::{SolarData, body_damping};
use crate::object::{
    ArchGroup, DockingPoint, EngineObject, Equipped, KindData, ObjHandle, Object, ObjectKind,
    PartMapEntry, Physical, Simple, asteroid, loot::LootData,
};

use super::{ControllerRef, World};

/// Why an object could not be made.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum SpawnError {
    /// No archetype of the right kind has the id.
    UnknownArchetype(u32),
    /// The system is not loaded.
    UnknownSystem(u32),
}

impl core::fmt::Display for SpawnError {
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        match self {
            Self::UnknownArchetype(id) => write!(f, "no archetype {id:#010x}"),
            Self::UnknownSystem(id) => write!(f, "system {id:#010x} is not loaded"),
        }
    }
}

impl std::error::Error for SpawnError {}

/// What a ship is made from (`CShip::CreateParms` as the server fills it).
#[derive(Clone, Debug, PartialEq)]
pub struct ShipSpawn {
    /// The ship archetype.
    pub archetype: u32,
    /// The loadout (0 none).
    pub loadout: u32,
    /// The position.
    pub position: Vector,
    /// The orientation.
    pub orientation: Matrix,
    /// The initial velocity.
    pub velocity: Vector,
    /// The initial angular velocity (world space).
    pub angular_velocity: Vector,
    /// The owning player (0 for an NPC).
    pub owner_player: u32,
    /// The hit points, -1 for the archetype's.
    pub hit_pts: f32,
    /// The object id; `None` allocates a dynamic one.
    pub id: Option<u32>,
}

impl ShipSpawn {
    /// A ship at rest with full health and a dynamic id.
    #[must_use]
    pub fn new(archetype: u32, loadout: u32, position: Vector, orientation: Matrix) -> Self {
        Self {
            archetype,
            loadout,
            position,
            orientation,
            velocity: Vector::ZERO,
            angular_velocity: Vector::ZERO,
            owner_player: 0,
            hit_pts: -1.0,
            id: None,
        }
    }
}

/// What a solar is made from.
#[derive(Clone, Debug, PartialEq)]
pub struct SolarSpawn {
    /// The solar archetype.
    pub archetype: u32,
    /// The loadout (0 none).
    pub loadout: u32,
    /// The nickname id; also the object id (`CreateID(nickname)`).
    pub nickname: u32,
    /// The position.
    pub position: Vector,
    /// The orientation.
    pub orientation: Matrix,
    /// The spin (world space); a still solar is fixed.
    pub angular_velocity: Vector,
    /// Destructible.
    pub destructible: bool,
    /// The base it docks with.
    pub dock_with: u32,
    /// The system a gate leads to.
    pub dest_system: u32,
    /// The atmosphere range.
    pub atmosphere_range: f32,
}

/// What a loot container is made from.
#[derive(Clone, Debug, PartialEq)]
pub struct LootSpawn {
    /// The container archetype (an equipment `LootCrate` or the contents'
    /// loot appearance).
    pub archetype: u32,
    /// The contents.
    pub contents: u32,
    /// The units.
    pub units: u32,
    /// The contents' hit points.
    pub contents_hit_pts: f32,
    /// The owner (0 none).
    pub owner: u32,
    /// The position.
    pub position: Vector,
    /// The velocity.
    pub velocity: Vector,
}

/// What a projectile is made from (`FServerBulletRecord`,
/// `FServerLaunchRecord`).
#[derive(Clone, Debug, PartialEq)]
pub struct ProjectileSpawn {
    /// The munition, mine or counter-measure archetype.
    pub archetype: u32,
    /// The owner's id.
    pub owner: u32,
    /// The position.
    pub position: Vector,
    /// The orientation.
    pub orientation: Matrix,
    /// The velocity.
    pub velocity: Vector,
    /// A guided munition's target.
    pub target: Option<(ObjHandle, SubObjId)>,
}

fn simple_for(root: &Root) -> Simple {
    Simple {
        max_hit_pts: root.hit_pts,
        type_bits: root.object_type,
        ..Simple::default()
    }
}

fn hp_name(
    strings: &crate::data::string_cache::StringCache,
    s: crate::data::string_cache::CacheString,
) -> Vec<u8> {
    strings.get(s).to_vec()
}

impl World {
    fn require_system(&self, system: u32) -> Result<(), SpawnError> {
        if self.physics.world(system).is_none() {
            return Err(SpawnError::UnknownSystem(system));
        }
        Ok(())
    }

    /// `CObject::Alloc`, `Root::create_instance` and `open`: the object
    /// with its instance tree and part map.
    fn base_object(&mut self, kind: ObjectKind, root: &Root, data: KindData) -> Object {
        let path = self.data.strings.get(root.da_archetype).to_vec();
        let instance = self.create_scene_instance(&path);
        let mut physical = Physical::default();
        for i in self.instance_tree(instance) {
            let (_, crc) = self.scene.instance_name(Some(i));
            physical.part_map.push(PartMapEntry {
                part_id: crc,
                instance: Some(i),
                shield_part: false,
            });
        }
        Object {
            kind,
            archetype: root.nickname,
            arch_mass: root.mass,
            engine: EngineObject {
                instance,
                ..EngineObject::default()
            },
            physical,
            simple: simple_for(root),
            equipped: None,
            data,
            dead: false,
        }
    }

    /// `CSimple::init`'s common part: id, owner, hit points (−1: the
    /// archetype's; above it: clamped; negative: 0.5, as the reference
    /// reports and does), frame.
    fn init_simple(
        obj: &mut Object,
        system: u32,
        id: u32,
        owner: u32,
        hit_pts: f32,
        position: Vector,
        orientation: Matrix,
    ) {
        obj.simple.id = id;
        obj.simple.system = Some(system);
        obj.simple.owner_player = owner;
        let max = obj.simple.max_hit_pts;
        obj.simple.hit_pts = if hit_pts == -1.0 {
            max
        } else if !(hit_pts < 0.0) {
            if !(max < hit_pts) { hit_pts } else { max }
        } else {
            0.5
        };
        obj.engine.position = position;
        obj.engine.orientation = orientation;
        obj.physical.system = system;
    }

    /// `PhySys::BuildIVP_Real` (or `BuildIVP_Sphere` with `sphere`): the
    /// body from the root part's surface (the default part when the
    /// archetype has none), at the object's frame.
    fn build_body(&mut self, h: ObjHandle, parms: CreateParms, sphere: Option<f32>, fixed: bool) {
        let Some(obj) = self.objects.get(h) else {
            return;
        };
        let system = obj.physical.system;
        let root_part = obj.physical.part_map.first().map_or(0, |p| p.part_id);
        let archetype = obj.archetype;
        let (position, orientation) = (obj.engine.position, obj.engine.orientation);
        let path = self.archetype_path(obj.kind, archetype);
        let geometry = match sphere {
            Some(radius) => Geometry::ball(radius),
            None => {
                let parts = self.surface_parts(archetype, &path);
                World::part_geometry(parts.as_ref(), root_part).0
            }
        };
        let enable = parms.enable_collisions;
        let pin = !self.config.single_player;
        let Some(world) = self.physics.world_mut(system) else {
            return;
        };
        let body = world.create_body(BodyDesc {
            user_key: h.to_key(),
            geometry,
            parms,
            position,
            orientation,
            pin_mass_center: pin,
        });
        world.set_collision_state(body, enable);
        if fixed {
            // The construction template's fixed flag (+0x1c): the body
            // takes no part in the simulation (divergence register).
            world.deactivate(body);
        }
        if let Some(obj) = self.objects.get_mut(h) {
            obj.physical.body = Some(body);
        }
    }

    fn archetype_path(&self, kind: ObjectKind, archetype: u32) -> Vec<u8> {
        let a = &self.data.archetypes;
        let root = match kind {
            ObjectKind::Ship => a.ships.get(&archetype).map(|s| s.eq_obj.root),
            ObjectKind::Solar => a.solars.get(&archetype).map(|s| s.eq_obj.root),
            ObjectKind::Asteroid => a.asteroids.get(&archetype).map(|s| s.root),
            ObjectKind::DynamicAsteroid => a.dynamic_asteroids.get(&archetype).copied(),
            _ => a.equipment.get(&archetype).map(|e| e.root),
        };
        root.map(|r| self.data.strings.get(r.da_archetype).to_vec())
            .unwrap_or_default()
    }

    /// The hardpoint ids of the part an instance stands for (a hardpoint in
    /// that list makes a non-physical attachment).
    fn part_hardpoint_ids(&mut self, owner: ObjHandle, instance: Option<InstanceId>) -> Vec<u32> {
        let Some(obj) = self.objects.get(owner) else {
            return Vec::new();
        };
        let part = obj.inst_to_part(instance);
        let (kind, archetype) = (obj.kind, obj.archetype);
        let path = self.archetype_path(kind, archetype);
        let parts = self.surface_parts(archetype, &path);
        World::part_geometry(parts.as_ref(), part).1
    }

    /// `CExternalEquip::Connect` and `CAttachedEquip::Connect`: the mount
    /// on the owner's instance tree, the item's model joined at the
    /// hardpoint, and for launchers their barrels. False when the
    /// hardpoint is missing (the item then reads as destroyed).
    fn connect_equip(
        &mut self,
        owner: ObjHandle,
        equip: &mut Equip,
        hardpoint: &[u8],
        arch: &Equipment,
    ) -> bool {
        let Some(root) = self.objects.get(owner).and_then(|o| o.engine.instance) else {
            if let Some(m) = &mut equip.mount {
                m.hardpoint = hardpoint.to_vec();
            }
            return false;
        };
        let Some((instance, data)) = self.find_hardpoint(Some(root), hardpoint) else {
            if let Some(m) = &mut equip.mount {
                m.hardpoint = hardpoint.to_vec();
                m.instance = None;
            }
            return false;
        };
        if let Some(m) = &mut equip.mount {
            m.hardpoint = hardpoint.to_vec();
            m.connected = true;
            m.instance = Some(instance);
            m.position = data.position();
            m.orientation = data.orientation();
        }
        if equip.attached.is_none() {
            return true;
        }
        let hp_crc = name_crc32(hardpoint);
        let non_physical = self
            .part_hardpoint_ids(owner, Some(instance))
            .contains(&hp_crc);
        let path = self.data.strings.get(arch.root.da_archetype).to_vec();
        let model = self.create_scene_instance(&path);
        if let Some(model) = model {
            // `CNonPhysAttachment::Connect` / `CPhysAttachment::Connect`
            // join the model at its `HPConnect`.
            connect_hardpoints(
                &mut self.scene,
                Some(instance),
                hp_crc,
                Some(model),
                name_crc32(b"HPConnect"),
            );
            self.instances.insert(model, owner);
        }
        let attachment = if non_physical {
            Attachment::NonPhysical {
                instance: model,
                hp_crc,
                collisions: true,
            }
        } else {
            Attachment::Physical {
                object: owner,
                instance: model,
            }
        };
        if let Some(a) = &mut equip.attached {
            a.attachment = Some(attachment);
        }
        // `CELauncher::Connect`: the `HPFire0N` barrels of the model.
        let barrels = &mut match &mut equip.kind {
            EquipKind::Gun(g) => Some(&mut g.launcher.barrels),
            EquipKind::MineDropper(l) | EquipKind::CmDropper(l) => Some(&mut l.barrels),
            _ => None,
        };
        if let Some(list) = barrels {
            list.clear();
            for n in 1..=8u8 {
                let name = [b'H', b'P', b'F', b'i', b'r', b'e', b'0', b'0' + n];
                match self.find_hardpoint(model, &name) {
                    Some((i, d)) => list.push(Barrel {
                        instance: Some(i),
                        position: d.position(),
                        orientation: d.orientation(),
                    }),
                    None => break,
                }
            }
        }
        true
    }

    /// `CEqObj::add_item`: an equipped item (allocated, its hit points
    /// set, an external one connected) or a cargo stack.
    fn add_item(
        &mut self,
        owner: ObjHandle,
        desc: &crate::data::equip_desc::EquipDesc,
        ship: bool,
    ) {
        let Some(arch) = self.data.archetypes.equipment.get(&desc.arch_id).cloned() else {
            return;
        };
        if desc.equipped == 0 {
            let count = u32::try_from(desc.count).unwrap_or(0);
            let cargo = Equip::cargo(desc.id, &arch, desc.mission, desc.health, count);
            if let Some(eq) = self
                .objects
                .get_mut(owner)
                .and_then(|o| o.equipped.as_deref_mut())
            {
                eq.equipment.add(cargo);
            }
            return;
        }
        let projectile = match &arch.kind {
            EquipmentKind::Gun(g) => Some(g.launcher.projectile_archetype),
            EquipmentKind::MineDropper(l) => Some(l.projectile_archetype),
            EquipmentKind::CounterMeasureDropper { launcher, .. } => {
                Some(launcher.projectile_archetype)
            }
            _ => None,
        }
        .and_then(|id| self.data.archetypes.equipment.get(&id).cloned());
        let Some(mut equip) = Equip::alloc(desc.id, &arch, desc.mission, ship, projectile.as_ref())
        else {
            return;
        };
        equip.set_hit_points((f64::from(desc.health) * f64::from(arch.root.hit_pts)) as f32);
        let internal = desc.is_internal(&self.data.strings);
        if !internal && equip.mount.is_some() {
            let hp = hp_name(&self.data.strings, desc.hardpoint);
            self.connect_equip(owner, &mut equip, &hp, &arch);
        }
        if let EquipKind::Engine(engine) = &mut equip.kind {
            self.find_nozzles(owner, engine);
        }
        if let Some(eq) = self
            .objects
            .get_mut(owner)
            .and_then(|o| o.equipped.as_deref_mut())
        {
            eq.equipment.add(equip);
        }
    }

    /// `CEEngine::CEEngine`'s nozzle search: `HpEngine0<N>` for N up to the
    /// ship archetype's `num_exhaust_nozzles`.
    fn find_nozzles(&mut self, owner: ObjHandle, engine: &mut crate::equip::flight::EngineState) {
        let Some(obj) = self.objects.get(owner) else {
            return;
        };
        let count = self
            .data
            .archetypes
            .ships
            .get(&obj.archetype)
            .map_or(0, |s| s.num_exhaust_nozzles);
        let root = obj.engine.instance;
        for index in 1..=count {
            let name = format!("HpEngine0{index}");
            if let Some((instance, data)) = self.find_hardpoint(root, name.as_bytes()) {
                let sub = self.inst_to_sub_obj_id(owner, Some(instance));
                engine.nozzles.push(Nozzle {
                    sub_obj_id: sub,
                    index,
                    position: data.position(),
                    orientation: data.orientation(),
                });
            }
        }
    }

    /// `CEqObj::inst_to_subobj_id`: the collision group whose tree holds the
    /// instance, else the attached item mounted there, else the root.
    #[must_use]
    pub fn inst_to_sub_obj_id(&self, h: ObjHandle, instance: Option<InstanceId>) -> SubObjId {
        let Some(obj) = self.objects.get(h) else {
            return 0;
        };
        let Some(eq) = obj.equipped.as_deref() else {
            return crate::equip::SUBOBJ_ID_ROOT;
        };
        if let Some(g) = eq
            .arch_groups
            .iter()
            .find(|g| g.root_instance.is_some() && g.root_instance == instance)
        {
            return g.id;
        }
        let id = eq.equipment.inst_to_sub_obj_id(instance);
        if id != 0 {
            return id;
        }
        crate::equip::SUBOBJ_ID_ROOT
    }

    /// `CEqObj::link_shields`: each shield link whose generators are fitted
    /// becomes a shield at the link's hardpoint with those generators.
    fn link_shields(
        &mut self,
        owner: ObjHandle,
        links: &[crate::data::archetype::eq_obj::ShieldLink],
    ) {
        let now = self.time;
        for link in links {
            let mut shield_slot: Option<SubObjId> = None;
            for gen_hp in &link.generator_hps {
                let hp = hp_name(&self.data.strings, *gen_hp);
                let Some(obj) = self.objects.get(owner) else {
                    return;
                };
                let Some(eq) = obj.equipped.as_deref() else {
                    return;
                };
                let Some(slot) = eq.equipment.find_by_hardpoint(&hp) else {
                    continue;
                };
                let Some(gen_equip) = eq.equipment.get(slot) else {
                    continue;
                };
                let EquipKind::ShieldGenerator(gen_state) = &gen_equip.kind else {
                    continue;
                };
                if gen_state.linked_shield.is_some() {
                    continue;
                }
                let mut generator = gen_state.clone();
                generator.nickname = gen_equip.arch_id;
                let gen_id = gen_equip.id;
                if shield_slot.is_none() {
                    let Some(arch) = self
                        .data
                        .archetypes
                        .equipment
                        .get(&link.shield_arch)
                        .cloned()
                    else {
                        return;
                    };
                    let mut shield = Equip::new(
                        link.equip_id,
                        &arch,
                        types::SHIELD,
                        EquipKind::Shield(Shield::default()),
                        false,
                    );
                    // `CEShield::CEShield`: fate 3, `Activate(true)`.
                    if let Some(a) = &mut shield.attached {
                        a.fate = crate::equip::Fate::HealthProxy;
                    }
                    let link_hp = hp_name(&self.data.strings, link.hardpoint);
                    self.connect_equip(owner, &mut shield, &link_hp, &arch);
                    shield.activate_base(true);
                    let status = shield_status(&shield);
                    let mut events = Vec::new();
                    if let EquipKind::Shield(s) = &mut shield.kind {
                        s.activate(status, now, &mut events);
                    }
                    shield.events.extend(events);
                    if let Some(eq) = self
                        .objects
                        .get_mut(owner)
                        .and_then(|o| o.equipped.as_deref_mut())
                    {
                        eq.equipment.add(shield);
                    }
                    shield_slot = Some(link.equip_id);
                }
                let Some(shield_id) = shield_slot else {
                    continue;
                };
                // `LinkShield` / `AddGen`.
                let Some(eq) = self
                    .objects
                    .get_mut(owner)
                    .and_then(|o| o.equipped.as_deref_mut())
                else {
                    return;
                };
                if let Some(EquipKind::ShieldGenerator(g)) =
                    eq.equipment.by_id_mut(gen_id).map(|e| &mut e.kind)
                {
                    g.linked_shield = Some(shield_id);
                }
                if let Some(shield) = eq.equipment.by_id_mut(shield_id) {
                    let status = shield_status(shield);
                    let mut events = Vec::new();
                    if let EquipKind::Shield(s) = &mut shield.kind {
                        s.add_gen(gen_id, &generator, status, now, &mut events);
                    }
                    shield.events.extend(events);
                }
            }
        }
    }

    /// `PhySys::Controller::Register` for one of an object's controllers.
    fn register(&mut self, h: ObjHandle, which: ControllerRef) {
        let Some((system, body)) = self.body(h) else {
            return;
        };
        let key = self.next_controller_key();
        let Some(world) = self.physics.world_mut(system) else {
            return;
        };
        let Some(handle) = world.register_controller(body, key, CONTROLLER_PRIORITY) else {
            return;
        };
        self.controllers.insert(key, (h, which));
        if let Some(obj) = self.objects.get_mut(h) {
            obj.physical.controllers.push((which, handle, key));
        }
    }

    /// `ControlObject` for an item's controller (`EnableController`).
    pub(crate) fn register_equip_controller(&mut self, h: ObjHandle, id: SubObjId) {
        self.register(h, ControllerRef::Equip(id));
    }

    /// `PhySys::Controller::UnRegister`.
    pub(crate) fn unregister(&mut self, h: ObjHandle, which: ControllerRef) {
        let Some(obj) = self.objects.get_mut(h) else {
            return;
        };
        let system = obj.physical.system;
        let mut gone = Vec::new();
        obj.physical.controllers.retain(|(w, handle, key)| {
            if *w == which {
                gone.push((*handle, *key));
                false
            } else {
                true
            }
        });
        for (handle, key) in gone {
            if let Some(world) = self.physics.world_mut(system) {
                world.unregister_controller(handle);
            }
            self.controllers.remove(&key);
        }
    }

    /// `CEqObj::init`'s equipment part: the collision groups, the loadout's
    /// items, the shields, the power.
    fn init_equipped(
        &mut self,
        h: ObjHandle,
        eq_arch: &crate::data::archetype::eq_obj::EqObj,
        loadout: u32,
        ship: bool,
    ) {
        let mut groups = Vec::new();
        let tree: Vec<(InstanceId, u32)> = self
            .objects
            .get(h)
            .map(|o| {
                o.physical
                    .part_map
                    .iter()
                    .filter_map(|p| p.instance.map(|i| (i, p.part_id)))
                    .collect()
            })
            .unwrap_or_default();
        for g in &eq_arch.collision_groups {
            let name = self.data.strings.get(g.obj).to_vec();
            let mut group = ArchGroup::new(g, name.clone());
            let crc = name_crc32(&name);
            group.root_instance = tree.iter().find(|(_, part)| *part == crc).map(|(i, _)| *i);
            group.instances = self.instance_tree(group.root_instance);
            groups.push(group);
        }
        let docking_points = eq_arch
            .docking_spheres
            .iter()
            .map(|s| DockingPoint {
                hardpoint: s.hardpoint.clone(),
                radius: s.radius,
                dock_type: s.dock_type,
                anim: s.anim.clone(),
                active: false,
            })
            .collect();
        if let Some(obj) = self.objects.get_mut(h) {
            obj.equipped = Some(Box::new(Equipped {
                vibe: -1,
                behavior_index: -1,
                arch_groups: groups,
                docking_points,
                ..Equipped::default()
            }));
        }
        if let Some(l) = self.data.loadouts.get(loadout).cloned() {
            for item in &l.items {
                self.add_item(h, item, ship);
            }
        }
        let has_gen = self
            .objects
            .get(h)
            .and_then(|o| o.equipped.as_deref())
            .is_some_and(|e| e.equipment.find_first(types::SHIELD_GEN).is_some());
        if has_gen {
            self.link_shields(h, &eq_arch.shield_links);
        }
        if let Some(eq) = self
            .objects
            .get_mut(h)
            .and_then(|o| o.equipped.as_deref_mut())
        {
            let mut max = 0.0f32;
            for slot in eq.equipment.traverse(types::POWER, false) {
                if let Some(e) = eq.equipment.get(slot) {
                    let add = match &e.kind {
                        EquipKind::Power(p) if !e.is_destroyed() => p.capacity,
                        _ => 0.0,
                    };
                    max += add;
                }
            }
            eq.max_power = max;
            eq.power = max;
        }
    }

    /// Makes a ship (`pub::SpaceObj::Create` through `Ship::create`,
    /// `CShip::init` and `StarSystem`'s add): body, equipment, controllers
    /// registered in the reference's order (engine and thrusters as they
    /// are made, then steering, nudge, strafe and the ship), and, unless it
    /// is a player's ship in single player, collisions on.
    ///
    /// # Errors
    ///
    /// An unknown archetype or system.
    pub fn spawn_ship(&mut self, system: u32, s: &ShipSpawn) -> Result<ObjHandle, SpawnError> {
        self.require_system(system)?;
        let arch = self
            .data
            .archetypes
            .ships
            .get(&s.archetype)
            .cloned()
            .ok_or(SpawnError::UnknownArchetype(s.archetype))?;
        let flight = crate::equip::flight::ShipFlightArch::of(&arch);
        let data = KindData::Ship(Box::new(ShipData::new(flight)));
        let mut obj = self.base_object(ObjectKind::Ship, &arch.eq_obj.root, data);
        let id = s.id.unwrap_or_else(|| self.allocate_id());
        World::init_simple(
            &mut obj,
            system,
            id,
            s.owner_player,
            s.hit_pts,
            s.position,
            s.orientation,
        );
        let h = self.insert(system, obj);
        self.sync_instance(h);
        // `CShip::init_physics`.
        let root = &arch.eq_obj.root;
        let constants = self.physics.constants().clone();
        let parms = CreateParms {
            mass: root.mass,
            linear_damping: constants.default_linear_damping,
            angular_damping: constants.default_angular_damping,
            name: itoa_radix24(id),
            enable_collisions: false,
            inertia: root
                .has_rotation_inertia
                .then(|| Vector::from_words(root.rotation_inertia)),
        };
        self.build_body(h, parms, None, false);
        self.add_initial_velocity(h, s.velocity, s.angular_velocity);
        self.init_equipped(h, &arch.eq_obj, s.loadout, true);
        // Engines and thrusters register as they are made
        // (`CEEngine::CEEngine` through `EnableController`,
        // `CEThruster::CEThruster` through `ControlObject`).
        let mut equip_controllers = Vec::new();
        if let Some(eq) = self.objects.get(h).and_then(|o| o.equipped.as_deref()) {
            for e in eq.equipment.iter() {
                match &e.kind {
                    EquipKind::Engine(_) if e.is_active() => equip_controllers.push(e.id),
                    EquipKind::Thruster(_) => equip_controllers.push(e.id),
                    _ => {}
                }
            }
        }
        for id in equip_controllers {
            self.register(h, ControllerRef::Equip(id));
            if let Some(eq) = self
                .objects
                .get_mut(h)
                .and_then(|o| o.equipped.as_deref_mut())
                && let Some(e) = eq.equipment.by_id_mut(id)
            {
                match &mut e.kind {
                    EquipKind::Engine(s) => s.controller_enabled = true,
                    EquipKind::Thruster(t) => t.controller_enabled = true,
                    _ => {}
                }
            }
        }
        // `CShip::init`: `enable_controllers`, then the guns, the thrust
        // power, steering and nudge, the docking points.
        for which in [
            ControllerRef::Steering,
            ControllerRef::Nudge,
            ControllerRef::Strafe,
            ControllerRef::Ship,
        ] {
            self.register(h, which);
        }
        if let Some(obj) = self.objects.get_mut(h) {
            let owner = obj.simple.owner_player;
            let (Some(eq), KindData::Ship(ship)) = (obj.equipped.as_deref_mut(), &mut obj.data)
            else {
                return Ok(h);
            };
            ship.flight.steering.controller_enabled = true;
            ship.flight.nudge.controller_enabled = true;
            ship.flight.strafe.controller_enabled = true;
            for slot in eq.equipment.traverse(types::GUN, false) {
                if let Some(e) = eq.equipment.get_mut(slot)
                    && let EquipKind::Gun(g) = &e.kind
                    && !g.launcher.projectile.is_some_and(|p| p.requires_ammo)
                {
                    e.activate_base(true);
                }
            }
            let mut max = ship.flight.max_thrust_power;
            for slot in eq.equipment.traverse(types::POWER, false) {
                if let Some(e) = eq.equipment.get(slot) {
                    let cap = match &e.kind {
                        EquipKind::Power(p) if !e.is_destroyed() => p.thrust_capacity,
                        _ => 0.0,
                    };
                    max = (f64::from(cap) + f64::from(max)) as f32;
                }
            }
            ship.flight.max_thrust_power = max;
            ship.flight.thrust_power = max;
            ship.flight.steering.active = true;
            ship.flight.nudge.active = true;
            ship.is_player = owner != 0;
        }
        if !self.config.single_player || s.owner_player == 0 {
            self.set_collisions(h, true);
        }
        Ok(h)
    }

    fn add_initial_velocity(&mut self, h: ObjHandle, velocity: Vector, angular_velocity: Vector) {
        let Some(obj) = self.objects.get(h) else {
            return;
        };
        let (system, Some(body)) = (obj.physical.system, obj.physical.body) else {
            return;
        };
        let os = rotate_world_to_object(&obj.engine.orientation, angular_velocity);
        if let Some(world) = self.physics.world_mut(system) {
            world.add_to_velocity(body, velocity);
            world.add_to_angular_velocity_os(body, os);
        }
    }

    /// `CObject::enable_collisions_r`: the collision state of the object's
    /// body.
    pub fn set_collisions(&mut self, h: ObjHandle, enable: bool) {
        if let Some((system, body)) = self.body(h)
            && let Some(world) = self.physics.world_mut(system)
        {
            world.set_collision_state(body, enable);
        }
    }

    /// Makes a solar (`Solar::create`, `CSolar::init`, `init_physics`):
    /// fixed unless it spins, a sphere for a `.sph` model.
    ///
    /// # Errors
    ///
    /// An unknown archetype or system.
    pub fn spawn_solar(&mut self, system: u32, s: &SolarSpawn) -> Result<ObjHandle, SpawnError> {
        self.require_system(system)?;
        let arch = self
            .data
            .archetypes
            .solars
            .get(&s.archetype)
            .cloned()
            .ok_or(SpawnError::UnknownArchetype(s.archetype))?;
        let mut solar = SolarData::new(s.nickname);
        solar.destructible = s.destructible;
        solar.loadout = s.loadout;
        solar.dest_system = s.dest_system;
        solar.atmosphere_range = s.atmosphere_range;
        let mut obj = self.base_object(
            ObjectKind::Solar,
            &arch.eq_obj.root,
            KindData::Solar(Box::new(solar)),
        );
        World::init_simple(
            &mut obj,
            system,
            s.nickname,
            0,
            -1.0,
            s.position,
            s.orientation,
        );
        if (obj.simple.type_bits & 0x84) != 0
            && let KindData::Solar(sd) = &mut obj.data
        {
            sd.visit_value = 1;
        }
        let h = self.insert(system, obj);
        self.sync_instance(h);
        let (fixed, linear, angular) = body_damping(s.angular_velocity);
        let parms = CreateParms {
            mass: arch.eq_obj.root.mass,
            linear_damping: linear,
            angular_damping: Vector::new(angular, angular, angular),
            name: itoa_radix24(s.nickname),
            enable_collisions: true,
            inertia: None,
        };
        let path = self
            .data
            .strings
            .get(arch.eq_obj.root.da_archetype)
            .to_ascii_lowercase();
        let sphere = path
            .ends_with(b".sph")
            .then_some(arch.solar_radius)
            .filter(|r| *r > 0.0);
        if !path.ends_with(b".sph") || sphere.is_some() {
            self.build_body(h, parms, sphere, fixed);
        }
        if !fixed
            && let Some((system, body)) = self.body(h)
            && let Some(world) = self.physics.world_mut(system)
        {
            world.add_to_angular_velocity_os(body, s.angular_velocity);
        }
        self.init_equipped(h, &arch.eq_obj, s.loadout, false);
        if let Some(eq) = self
            .objects
            .get_mut(h)
            .and_then(|o| o.equipped.as_deref_mut())
        {
            eq.docking_point_base = s.dock_with;
            if !self.config.single_player {
                eq.weapons_offline = true;
            }
        }
        self.set_collisions(h, true);
        Ok(h)
    }

    /// The solars of a system file's `[Object]` sections, made as the
    /// server's `StarSystem::load` makes them (position, rotation,
    /// archetype, loadout, base, gate). Returns the objects made.
    pub fn load_system_solars(&mut self, system: u32) -> Vec<ObjHandle> {
        self.load_system_solars_where(system, |_| true)
    }

    /// [`World::load_system_solars`] for the objects whose nickname id
    /// `keep` accepts.
    pub fn load_system_solars_where(
        &mut self,
        system: u32,
        keep: impl Fn(u32) -> bool,
    ) -> Vec<ObjHandle> {
        self.load_system(system);
        let Some(sys) = self.data.universe.systems.get(&system).cloned() else {
            return Vec::new();
        };
        let Some(assets) = self.assets.clone() else {
            return Vec::new();
        };
        let path = self
            .data
            .universe
            .system_file_path(self.data.strings.get(sys.file));
        let Some(mut reader) = crate::data::open_ini(&assets.host, &path) else {
            return Vec::new();
        };
        let mut out = Vec::new();
        while reader.read_header() {
            if !reader.is_header(b"Object") {
                continue;
            }
            let mut spawn = SolarSpawn {
                archetype: 0,
                loadout: 0,
                nickname: 0,
                position: Vector::ZERO,
                orientation: Matrix::IDENTITY,
                angular_velocity: Vector::ZERO,
                destructible: false,
                dock_with: 0,
                dest_system: 0,
                atmosphere_range: 0.0,
            };
            while reader.read_value() {
                if reader.is_value(b"nickname") {
                    spawn.nickname = neuron_core::id::create_id(&reader.value_string());
                } else if reader.is_value(b"pos") {
                    spawn.position = Vector::new(
                        crate::data::value_f32(&mut reader, 0),
                        crate::data::value_f32(&mut reader, 1),
                        crate::data::value_f32(&mut reader, 2),
                    );
                } else if reader.is_value(b"rotate") {
                    spawn.orientation = crate::data::zone::euler_matrix(Vector::new(
                        crate::data::value_f32(&mut reader, 0),
                        crate::data::value_f32(&mut reader, 1),
                        crate::data::value_f32(&mut reader, 2),
                    ));
                } else if reader.is_value(b"archetype") {
                    spawn.archetype = neuron_core::id::create_id(&reader.value_string());
                } else if reader.is_value(b"loadout") {
                    spawn.loadout = neuron_core::id::create_id(&reader.value_string());
                } else if reader.is_value(b"dock_with") {
                    spawn.dock_with = neuron_core::id::create_id(&reader.value_string());
                } else if reader.is_value(b"atmosphere_range") {
                    spawn.atmosphere_range = crate::data::value_f32(&mut reader, 0);
                }
            }
            if spawn.nickname == 0
                || !keep(spawn.nickname)
                || !self.data.archetypes.solars.contains_key(&spawn.archetype)
            {
                continue;
            }
            if let Ok(h) = self.spawn_solar(system, &spawn) {
                out.push(h);
            }
        }
        out
    }

    /// Makes a loot container (`Loot::create`, `CLoot::init`,
    /// `init_physics`).
    ///
    /// # Errors
    ///
    /// An unknown archetype or system.
    pub fn spawn_loot(&mut self, system: u32, s: &LootSpawn) -> Result<ObjHandle, SpawnError> {
        self.require_system(system)?;
        let arch = self
            .data
            .archetypes
            .equipment
            .get(&s.archetype)
            .cloned()
            .ok_or(SpawnError::UnknownArchetype(s.archetype))?;
        let safe = self.data.constants.general.loot_owner_safe_time;
        let data = KindData::Loot(LootData::new(
            s.owner,
            s.contents,
            s.units,
            s.contents_hit_pts,
            safe,
        ));
        let mut obj = self.base_object(ObjectKind::Loot, &arch.root, data);
        obj.simple.type_bits = 0x1000_0000;
        let id = self.allocate_id();
        World::init_simple(&mut obj, system, id, 0, -1.0, s.position, Matrix::IDENTITY);
        let name = match &obj.data {
            KindData::Loot(l) => l.body_name(id),
            _ => Vec::new(),
        };
        let h = self.insert(system, obj);
        self.sync_instance(h);
        let constants = self.physics.constants().clone();
        let parms = CreateParms {
            mass: arch.root.mass,
            linear_damping: constants.default_linear_damping,
            angular_damping: constants.default_angular_damping,
            name,
            enable_collisions: true,
            inertia: None,
        };
        self.build_body(h, parms, None, false);
        self.add_initial_velocity(h, s.velocity, Vector::ZERO);
        Ok(h)
    }

    /// `StarSystem::create_bullet`: a `CBeam` at the muzzle, moved as a
    /// ray each step, its collision group the owner's id.
    ///
    /// # Errors
    ///
    /// An unknown archetype or system.
    pub fn create_bullet(
        &mut self,
        system: u32,
        s: &ProjectileSpawn,
    ) -> Result<ObjHandle, SpawnError> {
        self.require_system(system)?;
        let arch = self
            .data
            .archetypes
            .equipment
            .get(&s.archetype)
            .cloned()
            .ok_or(SpawnError::UnknownArchetype(s.archetype))?;
        let p = arch
            .projectile()
            .copied()
            .ok_or(SpawnError::UnknownArchetype(s.archetype))?;
        let beam = BeamData {
            projectile: Projectile::new(s.owner, p.lifetime, p.owner_safe_time),
            munition: s.archetype,
            collision_group: itoa_radix24(s.owner),
            velocity: s.velocity,
            ..BeamData::default()
        };
        // A bullet has no model on the server side: no instance tree.
        let mut obj = Object {
            kind: ObjectKind::Beam,
            archetype: arch.root.nickname,
            arch_mass: arch.root.mass,
            engine: EngineObject::default(),
            physical: Physical::default(),
            simple: simple_for(&arch.root),
            equipped: None,
            data: KindData::Beam(beam),
            dead: false,
        };
        let id = self.allocate_id();
        World::init_simple(&mut obj, system, id, 0, -1.0, s.position, s.orientation);
        Ok(self.insert(system, obj))
    }

    fn projectile_body(
        &mut self,
        h: ObjHandle,
        mass: f32,
        linear: f32,
        owner: u32,
        velocity: Vector,
    ) {
        let constants = self.physics.constants().clone();
        let parms = CreateParms {
            mass,
            linear_damping: linear,
            angular_damping: constants.default_angular_damping,
            name: itoa_radix24(owner),
            enable_collisions: true,
            inertia: None,
        };
        self.build_body(h, parms, None, false);
        if let Some((system, body)) = self.body(h)
            && let Some(world) = self.physics.world_mut(system)
        {
            world.set_single_contact(body, true);
        }
        self.add_initial_velocity(h, velocity, Vector::ZERO);
    }

    /// A guided munition (`CGuided::init`, `init_physics`: no damping).
    ///
    /// # Errors
    ///
    /// An unknown archetype or system.
    pub fn create_guided(
        &mut self,
        system: u32,
        s: &ProjectileSpawn,
    ) -> Result<ObjHandle, SpawnError> {
        self.require_system(system)?;
        let arch = self
            .data
            .archetypes
            .equipment
            .get(&s.archetype)
            .cloned()
            .ok_or(SpawnError::UnknownArchetype(s.archetype))?;
        let EquipmentKind::Munition(m) = arch.kind.clone() else {
            return Err(SpawnError::UnknownArchetype(s.archetype));
        };
        let guided = GuidedData {
            projectile: Projectile::new(
                s.owner,
                m.projectile.lifetime,
                m.projectile.owner_safe_time,
            ),
            munition: s.archetype,
            target: s.target.map(|t| t.0),
            sub_target: s.target.map_or(0, |t| t.1),
            motor: (m.motor != 0)
                .then(|| self.data.archetypes.motors.get(&m.motor).cloned())
                .flatten(),
            seeker_elapsed: 0.0,
            seeker_range: m.seeker_range,
            seeker_fov_cos: m.seeker_fov_cos,
        };
        let mut obj = self.base_object(ObjectKind::Guided, &arch.root, KindData::Guided(guided));
        let id = self.allocate_id();
        World::init_simple(&mut obj, system, id, 0, -1.0, s.position, s.orientation);
        let h = self.insert(system, obj);
        self.sync_instance(h);
        let constants = self.physics.constants().clone();
        let parms = CreateParms {
            mass: arch.root.mass,
            linear_damping: 0.0,
            angular_damping: Vector::ZERO,
            name: itoa_radix24(s.owner),
            enable_collisions: true,
            inertia: None,
        };
        let _ = constants;
        self.build_body(h, parms, None, false);
        self.add_initial_velocity(h, s.velocity, Vector::ZERO);
        Ok(h)
    }

    /// A mine (`CMine::init_physics`: linear damping drag over mass).
    ///
    /// # Errors
    ///
    /// An unknown archetype or system.
    pub fn create_mine(
        &mut self,
        system: u32,
        s: &ProjectileSpawn,
    ) -> Result<ObjHandle, SpawnError> {
        self.require_system(system)?;
        let arch = self
            .data
            .archetypes
            .equipment
            .get(&s.archetype)
            .cloned()
            .ok_or(SpawnError::UnknownArchetype(s.archetype))?;
        let EquipmentKind::Mine {
            projectile,
            linear_drag,
            detonation_dist,
            seek_dist,
            acceleration,
            top_speed,
        } = arch.kind.clone()
        else {
            return Err(SpawnError::UnknownArchetype(s.archetype));
        };
        let data = MineData {
            projectile: Projectile::new(s.owner, projectile.lifetime, projectile.owner_safe_time),
            linear_drag,
            detonation_dist,
            seek_dist,
            acceleration,
            top_speed,
        };
        let mut obj = self.base_object(ObjectKind::Mine, &arch.root, KindData::Mine(data));
        let id = self.allocate_id();
        World::init_simple(&mut obj, system, id, 0, -1.0, s.position, s.orientation);
        let h = self.insert(system, obj);
        self.sync_instance(h);
        self.projectile_body(
            h,
            arch.root.mass,
            drag_damping(linear_drag, arch.root.mass),
            s.owner,
            s.velocity,
        );
        Ok(h)
    }

    /// A counter-measure (`CCounterMeasure::init_physics`).
    ///
    /// # Errors
    ///
    /// An unknown archetype or system.
    pub fn create_counter_measure(
        &mut self,
        system: u32,
        s: &ProjectileSpawn,
    ) -> Result<ObjHandle, SpawnError> {
        self.require_system(system)?;
        let arch = self
            .data
            .archetypes
            .equipment
            .get(&s.archetype)
            .cloned()
            .ok_or(SpawnError::UnknownArchetype(s.archetype))?;
        let EquipmentKind::CounterMeasure {
            projectile,
            linear_drag,
            range,
            diversion_pctg,
        } = arch.kind.clone()
        else {
            return Err(SpawnError::UnknownArchetype(s.archetype));
        };
        let data = CounterMeasureData {
            projectile: Projectile::new(s.owner, projectile.lifetime, projectile.owner_safe_time),
            linear_drag,
            range,
            diversion_pctg,
        };
        let mut obj = self.base_object(
            ObjectKind::CounterMeasure,
            &arch.root,
            KindData::CounterMeasure(data),
        );
        let id = self.allocate_id();
        World::init_simple(&mut obj, system, id, 0, -1.0, s.position, s.orientation);
        let h = self.insert(system, obj);
        self.sync_instance(h);
        self.projectile_body(
            h,
            arch.root.mass,
            drag_damping(linear_drag, arch.root.mass),
            s.owner,
            s.velocity,
        );
        Ok(h)
    }

    /// A field rock (`CAsteroid::init`, `init_physics`: mass 1000, damping
    /// 1e6, fixed, group "Asteroid").
    ///
    /// # Errors
    ///
    /// An unknown archetype or system.
    pub fn spawn_asteroid(
        &mut self,
        system: u32,
        archetype: u32,
        position: Vector,
        orientation: Matrix,
        rock: asteroid::AsteroidData,
    ) -> Result<ObjHandle, SpawnError> {
        self.require_system(system)?;
        let arch = self
            .data
            .archetypes
            .asteroids
            .get(&archetype)
            .cloned()
            .ok_or(SpawnError::UnknownArchetype(archetype))?;
        let mut rock = rock;
        rock.collision_group_id = system;
        let mut obj = self.base_object(
            ObjectKind::Asteroid,
            &arch.root,
            KindData::Asteroid(rock.clone()),
        );
        let id = rock.asteroid_id;
        World::init_simple(&mut obj, system, id, 0, -1.0, position, orientation);
        let h = self.insert(system, obj);
        self.sync_instance(h);
        let (mass, damping, name) = asteroid::ASTEROID_BODY;
        let parms = CreateParms {
            mass,
            linear_damping: damping,
            angular_damping: Vector::new(damping, damping, damping),
            name: name.to_vec(),
            enable_collisions: true,
            inertia: None,
        };
        self.build_body(h, parms, None, true);
        Ok(h)
    }
}

/// The shield state machine's view of its item.
pub(crate) fn shield_status(e: &Equip) -> ShieldStatus {
    ShieldStatus {
        active: e.is_active(),
        disabled: false,
        destroyed: e.is_destroyed(),
        attached: e.attached.as_ref().is_some_and(|a| a.attachment.is_some()),
    }
}
