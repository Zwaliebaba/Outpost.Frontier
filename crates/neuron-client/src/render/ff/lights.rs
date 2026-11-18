//! Lights and the material, as DX8RENDERER's light manager keeps them
//! (`FDx8RendererLightManager` and the light methods in
//! `CppSource/DX8RENDERER/DX8RENDERER.cpp`).
//!
//! The C++ keys a light by the caller's pointer and gives it a Direct3D
//! light index from a counter that only grows. `SetLight` sends data at
//! once; `AddLight` keeps the caller's pointer and the data is read at the
//! next flush, which every draw runs. Enable requests are recorded and
//! committed at the flush too. Here a light is keyed by a caller-chosen
//! number, and `add_light` and `update_light` take the data, which the C++
//! would read from the caller at the flush (divergence D205).

use std::collections::BTreeMap;

/// The enabled lights a draw honours (record 0004, owner answer 3).
pub const MAX_DRAW_LIGHTS: usize = 16;

/// `D3DLIGHT8` as its 26 raw words: type, diffuse, specular and ambient
/// colours (four floats each), position, direction, range, falloff, three
/// attenuation factors, theta and phi.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub struct LightData(pub [u32; 26]);

/// `D3DLIGHTTYPE`.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum LightType {
    Point,
    Spot,
    Directional,
    Other(u32),
}

impl LightData {
    fn f(&self, i: usize) -> f32 {
        f32::from_bits(self.0[i])
    }

    fn colour(&self, at: usize) -> [f32; 4] {
        [self.f(at), self.f(at + 1), self.f(at + 2), self.f(at + 3)]
    }

    #[must_use]
    pub fn light_type(&self) -> LightType {
        match self.0[0] {
            1 => LightType::Point,
            2 => LightType::Spot,
            3 => LightType::Directional,
            other => LightType::Other(other),
        }
    }

    /// Red, green, blue, alpha.
    #[must_use]
    pub fn diffuse(&self) -> [f32; 4] {
        self.colour(1)
    }
    #[must_use]
    pub fn specular(&self) -> [f32; 4] {
        self.colour(5)
    }
    #[must_use]
    pub fn ambient(&self) -> [f32; 4] {
        self.colour(9)
    }
    #[must_use]
    pub fn position(&self) -> [f32; 3] {
        [self.f(13), self.f(14), self.f(15)]
    }
    #[must_use]
    pub fn direction(&self) -> [f32; 3] {
        [self.f(16), self.f(17), self.f(18)]
    }
    #[must_use]
    pub fn range(&self) -> f32 {
        self.f(19)
    }
    #[must_use]
    pub fn falloff(&self) -> f32 {
        self.f(20)
    }
    /// Constant, linear and quadratic attenuation.
    #[must_use]
    pub fn attenuation(&self) -> [f32; 3] {
        [self.f(21), self.f(22), self.f(23)]
    }
    /// The inner cone angle of a spot light, in radians.
    #[must_use]
    pub fn theta(&self) -> f32 {
        self.f(24)
    }
    /// The outer cone angle of a spot light, in radians.
    #[must_use]
    pub fn phi(&self) -> f32 {
        self.f(25)
    }
}

/// `D3DMATERIAL8` as its 17 raw words: diffuse, ambient, specular and
/// emissive colours (four floats each), then the specular power. A new
/// device's material is all zero.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, Default)]
pub struct Material(pub [u32; 17]);

impl Material {
    fn colour(&self, at: usize) -> [f32; 4] {
        std::array::from_fn(|i| f32::from_bits(self.0[at + i]))
    }
    #[must_use]
    pub fn diffuse(&self) -> [f32; 4] {
        self.colour(0)
    }
    #[must_use]
    pub fn ambient(&self) -> [f32; 4] {
        self.colour(4)
    }
    #[must_use]
    pub fn specular(&self) -> [f32; 4] {
        self.colour(8)
    }
    #[must_use]
    pub fn emissive(&self) -> [f32; 4] {
        self.colour(12)
    }
    #[must_use]
    pub fn power(&self) -> f32 {
        f32::from_bits(self.0[16])
    }
}

/// `FDx8RendererLightRecord`.
#[derive(Clone, Debug)]
struct Record {
    enabled: bool,
    want_enabled: bool,
    data_dirty: bool,
    generation: i64,
    index: u32,
    /// The data `add_light` or `update_light` gave; `None` for a light
    /// created by `set_light` or `set_light_enable` (a null data pointer).
    data: Option<LightData>,
}

/// A light as the device holds it, by Direct3D index.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Default)]
pub struct DeviceLight {
    pub data: Option<LightData>,
    pub enabled: bool,
}

/// Why a flush stopped: the device refused `SetLight` with no data
/// (`D3DERR_INVALIDCALL`).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct FlushError {
    pub index: u32,
}

/// The light manager and the device's light table.
#[derive(Clone, Debug, Default)]
pub struct Lights {
    records: BTreeMap<u64, Record>,
    /// `G_LightIndexNext`: indices are never given back.
    next_index: u32,
    dirty: Vec<Option<u64>>,
    generation: i64,
    enabled_count: i64,
    device: BTreeMap<u32, DeviceLight>,
    material: Material,
}

impl Lights {
    #[must_use]
    pub fn new() -> Lights {
        Lights::default()
    }

    /// `FindOrCreate`: the record's index; a new record takes the next
    /// index and is dirty when it has data. An existing record keeps its
    /// data.
    fn find_or_create(&mut self, key: u64, data: Option<LightData>) -> u32 {
        if let Some(r) = self.records.get(&key) {
            return r.index;
        }
        let index = self.next_index;
        self.next_index += 1;
        self.records.insert(
            key,
            Record {
                enabled: false,
                want_enabled: false,
                data_dirty: data.is_some(),
                generation: -1,
                index,
                data,
            },
        );
        index
    }

    /// `LightQueueDirty`: once per generation, and only when the enable
    /// state changes or the light is on with data to send.
    fn queue(&mut self, key: u64) {
        let generation = self.generation;
        let Some(r) = self.records.get_mut(&key) else {
            return;
        };
        if r.generation == generation {
            return;
        }
        if r.enabled == r.want_enabled && (!r.want_enabled || !r.data_dirty) {
            return;
        }
        r.generation = generation;
        self.dirty.push(Some(key));
    }

    /// `SetLight`: the data goes to the device at once, at the index of the
    /// key's record (created without data when new).
    pub fn set_light(&mut self, key: u64, data: LightData) -> u32 {
        let index = self.find_or_create(key, None);
        self.device.entry(index).or_default().data = Some(data);
        index
    }

    /// `GetLight`: the device's light at `index`. The C++ passes the
    /// caller's handle through as the index.
    #[must_use]
    pub fn device_light(&self, index: u32) -> Option<&DeviceLight> {
        self.device.get(&index)
    }

    /// `SetLightEnable`: records the request; a disable of an unknown light
    /// does nothing, an enable creates it without data.
    pub fn set_light_enable(&mut self, key: u64, enable: bool) {
        if !self.records.contains_key(&key) {
            if !enable {
                return;
            }
            self.find_or_create(key, None);
        }
        if let Some(r) = self.records.get_mut(&key) {
            r.want_enabled = enable;
        }
        self.queue(key);
    }

    /// `GetLightEnable`: the requested state; false for an unknown light.
    #[must_use]
    pub fn light_enable(&self, key: u64) -> bool {
        self.records.get(&key).is_some_and(|r| r.want_enabled)
    }

    /// `AddLight`.
    pub fn add_light(&mut self, key: u64, data: LightData) {
        self.find_or_create(key, Some(data));
    }

    /// `UpdateLight`: the record's data is sent again at the next flush.
    /// Returns false for an unknown light (the C++ logs "light not found").
    pub fn update_light(&mut self, key: u64, data: LightData) -> bool {
        let Some(r) = self.records.get_mut(&key) else {
            return false;
        };
        r.data = Some(data);
        r.data_dirty = true;
        self.queue(key);
        true
    }

    /// `Remove`: the record goes; a queued entry of it is cleared.
    fn remove(&mut self, key: u64) {
        if !self.records.contains_key(&key) {
            return;
        }
        if let Some(slot) = self.dirty.iter_mut().find(|k| **k == Some(key)) {
            if self.records[&key].enabled {
                self.enabled_count -= 1;
            }
            *slot = None;
        }
        self.records.remove(&key);
    }

    /// `RemoveLight` and `DestroyLight`: disable, flush, forget.
    pub fn remove_light(&mut self, key: u64) -> Result<(), FlushError> {
        self.set_light_enable(key, false);
        let flushed = self.flush();
        self.remove(key);
        flushed
    }

    /// `Flush`, run by every draw: sends dirty data of lights that are
    /// wanted on, then the enable changes. Stops at the first refusal; the
    /// queue is emptied either way.
    pub fn flush(&mut self) -> Result<(), FlushError> {
        let queue = std::mem::take(&mut self.dirty);
        let mut result = Ok(());
        for key in queue.into_iter().flatten() {
            let Some(r) = self.records.get_mut(&key) else {
                continue;
            };
            let was_enabled = r.enabled;
            if r.data_dirty && r.want_enabled {
                r.data_dirty = false;
                match r.data {
                    Some(data) => self.device.entry(r.index).or_default().data = Some(data),
                    None => {
                        result = Err(FlushError { index: r.index });
                        break;
                    }
                }
            }
            if r.want_enabled != r.enabled {
                self.device.entry(r.index).or_default().enabled = r.want_enabled;
                r.enabled = r.want_enabled;
            }
            if was_enabled != r.enabled {
                self.enabled_count += if r.enabled { 1 } else { -1 };
            }
        }
        self.generation += 1;
        result
    }

    /// `Reset`, after the device is created again: every light with data is
    /// dirty, every wanted light is off on the new device and queued.
    pub fn reset(&mut self) {
        self.dirty.clear();
        self.generation = 0;
        self.enabled_count = 0;
        self.device.clear();
        let keys: Vec<u64> = self.records.keys().copied().collect();
        for key in keys {
            let r = self.records.get_mut(&key).expect("listed key");
            if r.data.is_some() {
                r.data_dirty = true;
            }
            if r.want_enabled {
                r.enabled = false;
                self.queue(key);
            }
        }
    }

    /// The lights a draw uses: the device's enabled lights with data, by
    /// index, at most [`MAX_DRAW_LIGHTS`] (the lowest indices).
    #[must_use]
    pub fn draw_lights(&self) -> Vec<(u32, LightData)> {
        self.device
            .iter()
            .filter(|(_, l)| l.enabled)
            .filter_map(|(i, l)| l.data.map(|d| (*i, d)))
            .take(MAX_DRAW_LIGHTS)
            .collect()
    }

    /// The number of enabled lights beyond [`MAX_DRAW_LIGHTS`] a draw would
    /// drop.
    #[must_use]
    pub fn dropped_lights(&self) -> usize {
        self.device
            .values()
            .filter(|l| l.enabled && l.data.is_some())
            .count()
            .saturating_sub(MAX_DRAW_LIGHTS)
    }

    /// `EnabledCount`.
    #[must_use]
    pub fn enabled_count(&self) -> i64 {
        self.enabled_count
    }

    pub fn set_material(&mut self, material: Material) {
        self.material = material;
    }

    #[must_use]
    pub fn material(&self) -> Material {
        self.material
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn light(kind: u32, r: f32) -> LightData {
        let mut w = [0u32; 26];
        w[0] = kind;
        w[1] = r.to_bits();
        w[21] = 1.0f32.to_bits();
        LightData(w)
    }

    #[test]
    fn added_lights_reach_the_device_at_the_flush() {
        let mut l = Lights::new();
        l.add_light(100, light(3, 0.5));
        l.set_light_enable(100, true);
        assert!(l.light_enable(100));
        assert!(l.draw_lights().is_empty());
        l.flush().unwrap();
        assert_eq!(l.draw_lights(), vec![(0, light(3, 0.5))]);
        assert_eq!(l.enabled_count(), 1);
        assert_eq!(l.draw_lights()[0].1.light_type(), LightType::Directional);
        // An update is sent at the next flush.
        assert!(l.update_light(100, light(3, 0.25)));
        assert_eq!(l.draw_lights()[0].1.diffuse()[0], 0.5);
        l.flush().unwrap();
        assert_eq!(l.draw_lights()[0].1.diffuse()[0], 0.25);
        assert!(!l.update_light(7, light(1, 1.0)));
    }

    #[test]
    fn indices_only_grow() {
        let mut l = Lights::new();
        l.add_light(1, light(1, 1.0));
        l.add_light(2, light(1, 1.0));
        l.remove_light(1).unwrap();
        l.add_light(3, light(1, 1.0));
        l.set_light_enable(3, true);
        l.flush().unwrap();
        assert_eq!(l.draw_lights()[0].0, 2);
    }

    #[test]
    fn removing_disables_on_the_device() {
        let mut l = Lights::new();
        l.add_light(5, light(1, 1.0));
        l.set_light_enable(5, true);
        l.flush().unwrap();
        l.remove_light(5).unwrap();
        assert!(l.draw_lights().is_empty());
        assert_eq!(l.enabled_count(), 0);
        assert!(!l.light_enable(5));
        // Disabling an unknown light creates nothing.
        l.set_light_enable(9, false);
        assert!(!l.records.contains_key(&9));
    }

    #[test]
    fn a_light_without_data_stops_the_flush() {
        let mut l = Lights::new();
        l.set_light_enable(1, true);
        // Enabled with no data: nothing to send, the enable goes through.
        l.flush().unwrap();
        assert!(l.draw_lights().is_empty());
        // update_light marks it dirty; set_light gives the device data directly.
        let index = l.set_light(1, light(2, 1.0));
        assert_eq!(index, 0);
        assert_eq!(l.draw_lights().len(), 1);
        // A dirty record without data: the device refuses SetLight.
        let r = l.records.get_mut(&1).unwrap();
        r.data_dirty = true;
        r.generation = -1;
        l.queue(1);
        assert_eq!(l.flush(), Err(FlushError { index: 0 }));
    }

    #[test]
    fn reset_restarts_the_generation_as_the_reference_does() {
        let mut l = Lights::new();
        l.add_light(1, light(1, 1.0));
        l.set_light_enable(1, true); // queued in generation 0
        l.flush().unwrap();
        l.reset();
        // The record was last queued in generation 0, which reset starts
        // again, so it is not queued: the new device sees it only after
        // another change.
        l.flush().unwrap();
        assert!(l.draw_lights().is_empty());
        assert!(l.update_light(1, light(1, 1.0)));
        l.flush().unwrap();
        assert_eq!(l.draw_lights().len(), 1);
    }

    #[test]
    fn draws_take_at_most_sixteen_lights() {
        let mut l = Lights::new();
        for key in 0..20u64 {
            l.add_light(key, light(1, key as f32));
            l.set_light_enable(key, true);
        }
        l.flush().unwrap();
        let lights = l.draw_lights();
        assert_eq!(lights.len(), MAX_DRAW_LIGHTS);
        assert_eq!(lights.last().unwrap().0, 15);
        assert_eq!(l.dropped_lights(), 4);
    }

    #[test]
    fn the_material_starts_at_zero() {
        let mut l = Lights::new();
        assert_eq!(l.material(), Material::default());
        let mut m = Material::default();
        m.0[16] = 8.0f32.to_bits();
        l.set_material(m);
        assert_eq!(l.material().power(), 8.0);
    }
}
