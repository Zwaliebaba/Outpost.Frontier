//! The asteroid fields of a loaded system in the world: the server's load
//! (`SrvAsteroidSystem::load`), `CmnAsteroid::Update` inside the server's
//! sub-step (`StarSystem_Update`, `Server/IServerImpl.cpp:3851`), the
//! server's change pass (`SrvAsteroid_UpdateSystem`,
//! `FSrvAsteroidField::update`, `FSrvAsteroidCube::apply_marker`) and the
//! field mines (`MedMine`: `arm`, `update`, `added`, `deleted`).
//!
//! The observers are every ship of the system that is not on a trade lane:
//! the server walks its whole ship list (`IObjDB::GetNext` with the mask
//! `0x7f0000`, which covers every ship type), players or not, so an NPC
//! populates the cubes around it as a player does. Each observer fills at
//! most the first two fields it is near, in load order.
//!
//! A rock's object is made when its cube is made (`CAsteroidCube::init`:
//! `CAsteroid::init` and its fixed body). The server lists it (the
//! `MedAsteroid` or `MedMine` of the cube's markers) in the same pass, and
//! unlists it once the cube went far; the object and its body go at the
//! next walk's flush, when the cube is dropped. An object whose rock id
//! repeats an earlier one of its cube is never listed, as in the
//! reference, whose server record is keyed by the rock id.

use neuron_core::math::{Matrix, Vector};
use neuron_core::physics::{Geometry, PhantomDesc};

use crate::data::asteroid_field::{AsteroidField, FieldBlock, FieldCx};
use crate::data::{Reports, open_ini};
use crate::object::asteroid::{AsteroidData, MineArm};
use crate::object::field::{Cube, FieldRuntime};
use crate::object::{KindData, ObjHandle, damage};

use super::ServerHooks;

use super::{TickList, World, WorldEvent};

fn e(v: f32) -> f64 {
    f64::from(v)
}

/// At most `capacity - 1` bytes (`SrvAsteroid_CopyBounded`).
fn bounded(text: &[u8], capacity: usize) -> Vec<u8> {
    text[..text.len().min(capacity - 1)].to_vec()
}

/// `DamageCause` 7, the literal `MedMine::update` gives its blast.
const CAUSE_MINE: damage::DamageCause = 7;

impl World {
    /// The server's asteroid fields of a loaded system
    /// (`CmnAsteroid::Load` and `SrvAsteroid_LoadSystem`, from
    /// `StarSystem::load` outside single player): every `[Asteroids]`
    /// section of the system file with a `file` and a `zone`, its `[Field]`
    /// block (`cube_size`, and `fill_dist` and `empty_cube_frequency`
    /// truncated to integers, 0 when absent), then the field file from its
    /// start. A field whose zone is missing is reported and left out (the
    /// server faults on it). Loads once per system; returns the number of
    /// fields. Without assets there are none.
    pub fn load_asteroid_fields(&mut self, system: u32) -> usize {
        self.load_system(system);
        if let Some(fields) = self.asteroid_fields.get(&system) {
            return fields.len();
        }
        let mut fields = Vec::new();
        let sys = self.data.universe.systems.get(&system).cloned();
        if let (Some(sys), Some(assets)) = (sys, self.assets.clone()) {
            let path = self
                .data
                .universe
                .system_file_path(self.strings.get(sys.file));
            // The field loads set the loot of the zones they name; the
            // world keeps the fields, not the zones' loot (P3.3 drops it).
            let mut universe = self.data.universe.clone();
            let mut reports = Reports::default();
            if let Some(mut reader) = open_ini(&assets.host, &path) {
                while reader.read_header() {
                    if !reader.is_header(b"Asteroids") {
                        continue;
                    }
                    let (mut file, mut zone_name) = (Vec::new(), Vec::new());
                    while reader.read_value() {
                        if reader.is_value(b"file") {
                            file = bounded(&reader.value_string(), 0x100);
                        } else if reader.is_value(b"zone") {
                            zone_name = bounded(&reader.value_string(), 0x40);
                        }
                    }
                    if file.is_empty() || zone_name.is_empty() {
                        continue;
                    }
                    let zone = neuron_core::id::create_id(&zone_name);
                    if !universe.zones.contains_key(&zone) {
                        reports.push(
                            "Server\\Space\\SrvAsteroid.cpp",
                            0xa1,
                            format!(
                                "*** ERROR: AsteroidField linked to missing Zone ({})",
                                crate::data::lossy(&zone_name)
                            ),
                        );
                        continue;
                    }
                    let mut field = AsteroidField::default();
                    field.set_system(system);
                    field.set_zone(Some(zone));
                    if let Some(mut field_reader) = open_ini(&assets.host, &file) {
                        let start = field_reader.state();
                        let block = FieldBlock::read(&mut field_reader, field.cube_size);
                        field_reader.set_state(&start);
                        let mut cx = FieldCx {
                            universe: &mut universe,
                            archetypes: &self.data.archetypes,
                            reports: &mut reports,
                        };
                        field.load(&mut field_reader, block.cube_size, &mut cx);
                        field.fill_dist = block.fill_dist;
                        field.empty_freq = block.empty_cube_frequency as f32;
                    }
                    fields.push(FieldRuntime::new(field));
                }
            }
        }
        let count = fields.len();
        self.asteroid_fields.insert(system, fields);
        count
    }

    /// The asteroid fields of a system, in load order.
    #[must_use]
    pub fn asteroid_fields(&self, system: u32) -> &[FieldRuntime] {
        self.asteroid_fields
            .get(&system)
            .map_or(&[], |f| f.as_slice())
    }

    /// The observers of `CAsteroidSystem::update`: every live ship of the
    /// system not on a trade lane, in list order, with its position and
    /// its negated forward axis (the third column of its rotation).
    fn field_observers(&self, system: u32) -> Vec<(Vector, Vector)> {
        self.list(system, TickList::Ships)
            .into_iter()
            .filter_map(|h| self.objects.get(h))
            .filter(|o| !o.dead)
            .filter(|o| !o.as_ship().is_some_and(|s| s.using_tradelane))
            .map(|o| {
                let m = &o.engine.orientation.m;
                (o.engine.position, Vector::new(-m[0][2], -m[1][2], -m[2][2]))
            })
            .collect()
    }

    /// `CmnAsteroid::Update(system, step)` then `SrvAsteroid_UpdateSystem`:
    /// the field walk around the given observers and the server's change
    /// pass. [`World::step`] runs it with [`World::field_observers`];
    /// a replay can give the observers itself.
    pub fn update_asteroid_fields_with(
        &mut self,
        system: u32,
        observers: &[(Vector, Vector)],
        dt: f32,
    ) {
        let Some(mut fields) = self.asteroid_fields.remove(&system) else {
            return;
        };
        // `flush_changes` on every field: the far cubes' objects go.
        for field in &mut fields {
            for cube in field.flush_changes() {
                self.drop_cube_objects(&cube);
            }
        }
        // The observer scan: each fills the first two fields it is near.
        for &(pos, dir) in observers {
            let mut populated = 0;
            for (index, field) in fields.iter_mut().enumerate() {
                let zone = field
                    .field
                    .zone
                    .and_then(|z| self.data.universe.zones.get(&z));
                if !field.near_field(zone, pos) {
                    continue;
                }
                populated += 1;
                // The reference reads the zone without a null test; a field
                // without one is not loaded (`load_asteroid_fields`).
                if let Some(zone) = zone.cloned() {
                    let made = field.populate_asteroids(&zone, pos, dir);
                    for id in made {
                        self.spawn_cube_objects(system, field, index, id);
                    }
                }
                if populated >= 2 {
                    break;
                }
            }
        }
        for field in &mut fields {
            field.update(dt);
        }
        // `SrvAsteroid_UpdateSystem`: the markers of every field.
        for field in &mut fields {
            self.apply_field_changes(system, field);
        }
        self.asteroid_fields.insert(system, fields);
    }

    /// The field walk around the system's ships.
    pub(crate) fn update_asteroid_fields(&mut self, system: u32, dt: f32) {
        if !self.asteroid_fields.contains_key(&system) {
            return;
        }
        let observers = self.field_observers(system);
        self.update_asteroid_fields_with(system, &observers, dt);
    }

    /// `CAsteroidCube::init`'s spawn: one object per rock of a new cube
    /// (`CObject::Alloc`, `open`, `CAsteroid::init` with its fixed body).
    fn spawn_cube_objects(
        &mut self,
        system: u32,
        field: &mut FieldRuntime,
        index: usize,
        cube_id: u32,
    ) {
        let Some(cube) = field.cubes.get(&cube_id) else {
            return;
        };
        let rocks = cube.rocks.clone();
        let mut objects = Vec::with_capacity(rocks.len());
        for rock in rocks {
            let data = AsteroidData {
                asteroid_id: rock.asteroid_id,
                cube_id,
                mine: rock.mine,
                instant_kill: rock.instant_kill,
                collision_group_id: system,
                field: u32::try_from(index).ok(),
                listed: true,
                armed: None,
            };
            if let Ok(h) = self.spawn_asteroid(
                system,
                rock.arch,
                rock.id,
                rock.position,
                rock.orientation,
                data,
            ) {
                objects.push(h);
            }
        }
        if let Some(cube) = field.cubes.get_mut(&cube_id) {
            cube.objects = objects;
        }
    }

    /// `~CAsteroidCube`: the rocks' objects are released; no destruction
    /// event (the rocks are culled, not destroyed).
    fn drop_cube_objects(&mut self, cube: &Cube) {
        for &h in &cube.objects {
            self.disarm_mine(h);
            self.retire(h);
        }
    }

    /// `FSrvAsteroidField::update`: each marker of a new cube lists its
    /// rock unless the cube's record already holds that rock id (a mine is
    /// armed: `create_med_mine`, `arm`); each marker of a far cube unlists
    /// it (`destroy_notify`; a mine's phantom goes). Records left empty are
    /// dropped.
    fn apply_field_changes(&mut self, system: u32, field: &mut FieldRuntime) {
        for marker in field.changes() {
            let record = field.server.entry(marker.cube).or_default();
            let found = record.get(&marker.asteroid_id).copied();
            if !marker.cube_active {
                if let Some(h) = found {
                    record.remove(&marker.asteroid_id);
                    self.disarm_mine(h);
                    self.set_listed(system, h, false);
                }
                continue;
            }
            if found.is_some() {
                continue;
            }
            let object = field.cubes.get(&marker.cube).and_then(|c| {
                c.rocks
                    .iter()
                    .position(|r| r.asteroid_id == marker.asteroid_id)
                    .and_then(|i| c.objects.get(i).copied())
            });
            let Some(h) = object else {
                continue;
            };
            record.insert(marker.asteroid_id, h);
            if marker.mine {
                self.arm_mine(h);
            }
        }
        // The rocks the server never listed (a repeated rock id) leave the
        // tick list too.
        for cube in field.cubes.values() {
            let record = field.server.get(&cube.id);
            for &h in &cube.objects {
                let listed = record.is_some_and(|r| r.values().any(|x| *x == h));
                if !listed && self.is_listed(h) {
                    self.set_listed(system, h, false);
                }
            }
        }
        field.server.retain(|_, r| !r.is_empty());
    }

    fn is_listed(&self, h: ObjHandle) -> bool {
        matches!(
            self.objects.get(h).map(|o| &o.data),
            Some(KindData::Asteroid(a)) if a.listed
        )
    }

    /// Takes a rock off the server's asteroid list (`ListC0`) or puts it
    /// back; the object and its body stay.
    fn set_listed(&mut self, system: u32, h: ObjHandle, on: bool) {
        let Some(KindData::Asteroid(a)) = self.objects.get_mut(h).map(|o| &mut o.data) else {
            return;
        };
        if a.listed == on {
            return;
        }
        a.listed = on;
        if let Some(list) = self
            .systems
            .get_mut(&system)
            .and_then(|s| s.lists.get_mut(&TickList::Asteroids))
        {
            if on {
                list.push(h);
            } else {
                list.retain(|x| *x != h);
            }
        }
    }

    /// `MedMine::arm(detect_radius, system)`: a ball phantom of the
    /// archetype's detection radius at the mine, merged into its body,
    /// collisions on; the explosion, its offset and the recharge time from
    /// the archetype (the timer starts full).
    fn arm_mine(&mut self, h: ObjHandle) {
        let Some(o) = self.objects.get(h) else {
            return;
        };
        let Some(arch) = self.data.archetypes.asteroids.get(&o.archetype) else {
            return;
        };
        let (radius, explosion, offset, recharge) = (
            arch.detect_radius,
            arch.root.explosion_arch,
            arch.explosion_offset,
            arch.recharge_time,
        );
        let position = o.engine.position;
        let Some((system, body)) = self.body(h) else {
            return;
        };
        let Some(world) = self.physics.world_mut(system) else {
            return;
        };
        let phantom = world.create_phantom(PhantomDesc {
            geometry: Geometry::ball(radius),
            position,
            orientation: Matrix::IDENTITY,
            name: Vec::new(),
            listener: true,
        });
        world.attach_phantom(phantom, body, position);
        world.set_phantom_collision_state(phantom, true);
        if let Some(KindData::Asteroid(a)) = self.objects.get_mut(h).map(|o| &mut o.data) {
            a.armed = Some(MineArm {
                phantom,
                intruders: 0,
                explosion,
                explosion_offset: offset,
                recharge_time: recharge,
                recharge_timer: recharge,
                direction: Vector::ZERO,
            });
        }
    }

    /// `~MedMine`: `PhySys::DestroyPhantom`.
    fn disarm_mine(&mut self, h: ObjHandle) {
        let Some(o) = self.objects.get_mut(h) else {
            return;
        };
        let system = o.physical.system;
        let KindData::Asteroid(a) = &mut o.data else {
            return;
        };
        if let Some(arm) = a.armed.take()
            && let Some(world) = self.physics.world_mut(system)
        {
            world.destroy_phantom(arm.phantom);
        }
    }

    /// `MedMine::added` and `deleted`: a ship (the root of the intruder)
    /// entering counts and sets the direction from the mine to the
    /// intruder itself; one leaving uncounts. `false` when the phantom is
    /// no mine's.
    pub(crate) fn mine_phantom_event(
        &mut self,
        system: u32,
        ev: &neuron_core::physics::PhantomEvent,
    ) -> bool {
        let Some(&mine) = self.list(system, TickList::Asteroids).iter().find(|m| {
            matches!(
                self.objects.get(**m).map(|o| &o.data),
                Some(KindData::Asteroid(a)) if a.armed.as_ref().is_some_and(|x| x.phantom == ev.phantom)
            )
        }) else {
            return false;
        };
        let intruder = ObjHandle::from_key(ev.user_key);
        let root = match self.objects.get(intruder).map(|o| &o.data) {
            Some(KindData::EquipmentObject {
                owner: Some(owner), ..
            }) => *owner,
            _ => intruder,
        };
        if !self
            .objects
            .get(root)
            .is_some_and(|o| o.is(crate::object::class::SHIP))
        {
            return true;
        }
        let intruder_pos = self
            .objects
            .get(intruder)
            .map_or(Vector::ZERO, |o| o.engine.position);
        let Some(o) = self.objects.get_mut(mine) else {
            return true;
        };
        let mine_pos = o.engine.position;
        let KindData::Asteroid(a) = &mut o.data else {
            return true;
        };
        let Some(arm) = a.armed.as_mut() else {
            return true;
        };
        if ev.entered {
            let d = Vector::new(
                intruder_pos.x - mine_pos.x,
                intruder_pos.y - mine_pos.y,
                intruder_pos.z - mine_pos.z,
            );
            // `sqrtf` of the float sum, `1.0f / length`, float products.
            let sum = ((e(d.x) * e(d.x) + e(d.y) * e(d.y)) + e(d.z) * e(d.z)) as f32;
            let length = sum.sqrt();
            let inverse = 1.0 / length;
            arm.direction = Vector::new(inverse * d.x, inverse * d.y, inverse * d.z);
            arm.intruders += 1;
        } else {
            arm.intruders -= 1;
        }
        true
    }

    /// `MedMine::update(dt)`: the recharge timer runs up to the recharge
    /// time; with an intruder and the timer full, the mine blows
    /// (`StarSystem::explode` with cause 7 when the explosion has a radius
    /// and an effect, then the mine-explosion message either way) and the
    /// timer starts again.
    pub(crate) fn update_field_mine(&mut self, h: ObjHandle, dt: f32, hooks: &mut dyn ServerHooks) {
        let Some(o) = self.objects.get_mut(h) else {
            return;
        };
        let (id, system, position) = (o.simple.id, o.physical.system, o.engine.position);
        let KindData::Asteroid(a) = &mut o.data else {
            return;
        };
        let Some(arm) = a.armed.as_mut() else {
            return;
        };
        if arm.recharge_timer < arm.recharge_time {
            arm.recharge_timer += dt;
        }
        if arm.intruders == 0 || !(arm.recharge_timer >= arm.recharge_time) {
            return;
        }
        let explosion = arm.explosion;
        let offset = Vector::new(
            arm.explosion_offset * arm.direction.x,
            arm.explosion_offset * arm.direction.y,
            arm.explosion_offset * arm.direction.z,
        );
        arm.recharge_timer = 0.0;
        if let Some(x) = self.data.archetypes.explosions.get(&explosion).cloned() {
            let epsilon = 0.0001f64;
            if e(x.radius) > epsilon
                && (e(x.impulse) > epsilon
                    || e(x.hull_damage) > epsilon
                    || e(x.energy_damage) > epsilon)
            {
                let blast = damage::Blast {
                    position,
                    radius: x.radius,
                    hull_damage: x.hull_damage,
                    energy_damage: x.energy_damage,
                };
                self.explode_caused_hooked(system, &blast, CAUSE_MINE, id, None, x.impulse, hooks);
            }
        }
        self.events
            .push(WorldEvent::MineExplosion { mine: id, offset });
    }
}
