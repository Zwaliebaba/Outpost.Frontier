//! The game options the engine's draw reads (`GetGameOption`,
//! `game_options.cpp`), with the reference's defaults: `LoadStartupConfig`
//! scatters the descriptor table `GGameOptionDescs` into the options
//! before anything reads one. The options menu and the saved options that
//! change them are the game's (P5); the engine holds the values, and the
//! game may set them (divergence D512).

/// `kGameOptCount`.
pub const OPTION_COUNT: usize = 0x46;

/// `kGameOptInterfaceGroup` ("[INTERFACE]").
pub const INTERFACE: u32 = 0x06;
/// "TEXT".
pub const TEXT: u32 = 0x08;
/// "SHIP WIRES": the tinted submits of the control meshes.
pub const SHIP_WIRES: u32 = 0x0B;
/// "DRAW SCROLL BAR CONTROLS".
pub const DRAW_SCROLL_BAR_CONTROLS: u32 = 0x16;
/// "LIKE_PETAL": the Win3D layer's projection cache off.
pub const LIKE_PETAL: u32 = 0x3E;

/// `GGameOptionDescs`: index, name and default value, in the table's row
/// order (which is not index order; 0x2F is listed twice).
pub const DESCRIPTORS: [(u32, &[u8], i32); 71] = [
    (0x00, b"[OUTLINES]", 0),
    (0x01, b"PHYS WIRES", 0),
    (0x02, b"PHYS SPHERES", 0),
    (0x03, b"SHIELDS", 0),
    (0x04, b"PHANTOMS", 0),
    (0x05, b"HOTSPOTS", 0),
    (0x06, b"[INTERFACE]", 1),
    (0x07, b"BUTTONS", 0),
    (0x08, b"TEXT", 1),
    (0x09, b"NEURONET", 2),
    (0x0a, b"HUD", 1),
    (0x0c, b"TARGET WINDOW LOCK", 0),
    (0x0d, b"AUTO OPEN TARGET WINDOW", 1),
    (0x0e, b"DRAW MOUSE", 1),
    (0x0f, b"DISPLAY SHIP IDS", 0),
    (0x10, b"DISPLAY INTERFACE TIMING", 0),
    (0x11, b"DISPLAY PLAYER STATS", 0),
    (0x12, b"SWITCHING TARGETS CLEARS MANEUVER", 1),
    (0x0b, b"SHIP WIRES", 1),
    (0x13, b"DRAW TEXT CONTROLS", 1),
    (0x14, b"DRAW BUTTON CONTROLS", 1),
    (0x15, b"DRAW STATIC CONTROLS", 1),
    (0x16, b"DRAW SCROLL BAR CONTROLS", 1),
    (0x17, b"INVERSE MOUSE", 0),
    (0x18, b"LIGHTING", 2),
    (0x19, b"[BACKGROUND]", 1),
    (0x1a, b"STARS", 1),
    (0x1b, b"[OBJECTS]", 1),
    (0x1c, b"CHARACTERS", 1),
    (0x1d, b"SHIPS", 1),
    (0x1e, b"PROJECTILES", 1),
    (0x1f, b"DEBRIS", 1),
    (0x20, b"SOLAR", 1),
    (0x21, b"ASTEROIDS", 1),
    (0x22, b"DYN_ASTEROIDS", 1),
    (0x23, b"PLANETS", 1),
    (0x24, b"SUN", 1),
    (0x25, b"[EFFECTS]", 1),
    (0x26, b"PARTICLES", 1),
    (0x27, b"NEBULA", 1),
    (0x28, b"SUN_EFFECTS", 1),
    (0x29, b"FLASHLIGHTS", 1),
    (0x2a, b"TRADE_LANES", 1),
    (0x2b, b"SHADOWS", 1),
    (0x2c, b"ASTEROID_EXT", 2),
    (0x2d, b"[AUDIO]", 1),
    (0x2f, b"VOL_VOICE", 0),
    (0x2e, b"VOL_MUSIC", 0),
    (0x2f, b"VOL_VOICE", 0),
    (0x30, b"VOL_SFX", 0),
    (0x31, b"VOL_INTERFACE", 0),
    (0x32, b"VOL_AMBIENCE", 0),
    (0x33, b"VOICE PROFILE OFFSET", 0),
    (0x34, b"PITCH_BEND_SOUNDS", 1),
    (0x35, b"[PLAYER]", 0),
    (0x36, b"AUTO_AIM", 0),
    (0x37, b"DISABLE_COLLIDE", 0),
    (0x38, b"SURFACE_HIT_EFFECTS", 1),
    (0x39, b"SPLASH_HIT_EFFECTS", 0),
    (0x3a, b"[MISC]", 0),
    (0x3b, b"AMBIENT", 0),
    (0x3c, b"DETAIL_LEVELS", 0),
    (0x3d, b"CHAR_PLACES", 0),
    (0x3e, b"LIKE_PETAL", 0),
    (0x3f, b"LIGHTS_DEBUG", 0),
    (0x40, b"PARTICLEFX_LOD", 1),
    (0x41, b"COMM_LOG", 0),
    (0x42, b"[PARTICLEFX_DEBUG]", 1),
    (0x43, b"BATCH_PARTICLES", 0),
    (0x44, b"USE_DYN_VB", 1),
    (0x45, b"COPY_TO_DYN_VB", 1),
];

/// The option values (`DisplaySettings::options`).
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct GameOptions {
    pub values: [i32; OPTION_COUNT],
}

impl Default for GameOptions {
    /// `LoadStartupConfig`'s scatter of the descriptor table.
    fn default() -> Self {
        let mut values = [0; OPTION_COUNT];
        for &(index, _, value) in &DESCRIPTORS {
            values[index as usize] = value;
        }
        GameOptions { values }
    }
}

impl GameOptions {
    /// `GetGameOption`: 0 for an index past the table.
    #[must_use]
    pub fn get(&self, index: u32) -> i32 {
        self.values.get(index as usize).copied().unwrap_or(0)
    }

    /// Sets an option (the options menu's write); an index past the table
    /// is ignored.
    pub fn set(&mut self, index: u32, value: i32) {
        if let Some(v) = self.values.get_mut(index as usize) {
            *v = value;
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn the_defaults_are_the_descriptor_table() {
        let o = GameOptions::default();
        assert_eq!(o.get(SHIP_WIRES), 1);
        assert_eq!(o.get(LIKE_PETAL), 0);
        assert_eq!(o.get(9), 2);
        assert_eq!(o.get(INTERFACE), 1);
        assert_eq!(o.get(0x46), 0);
        // Every index has a row.
        let mut seen = [false; OPTION_COUNT];
        for &(i, _, _) in &DESCRIPTORS {
            seen[i as usize] = true;
        }
        assert!(seen.iter().all(|&s| s));
    }
}
