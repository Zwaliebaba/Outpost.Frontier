//! The scenes the viewer shows: Gate G4's fixed set over the original data,
//! the synthetic one, and those named on the command line.

/// One scene.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Scene {
    /// The cube of [`crate::synthetic`]; needs no original data.
    Synthetic,
    /// A ship archetype of `SHIPS\shiparch.ini` by nickname.
    Ship(String),
    /// A solar archetype (station, dock, planet) of `SOLAR\solararch.ini`.
    Solar(String),
    /// A model file and the material libraries it needs.
    Model {
        path: String,
        libraries: Vec<String>,
    },
    /// A deformable model (`.dfm`), optionally animated from an `.anm`
    /// file (the named animation, else the first that binds).
    Character {
        body: String,
        animations: Option<String>,
        animation: Option<String>,
    },
    /// A particle effect of an `.ale` file (the named one, else the first).
    Effect { file: String, name: Option<String> },
    /// A scene script (`.thn`), played in a loop.
    Script(String),
    /// A sound of the sound tables by nickname.
    Sound(String),
    /// A voice line of a UTF voice archive (the named leaf, else the first).
    Voice {
        archive: String,
        line: Option<String>,
    },
    /// A page descriptor (`.page`) drawn over the scene.
    Page(String),
}

/// The names of the fixed scenes, in order.
pub const FIXED: [&str; 9] = [
    "synthetic",
    "ship",
    "station",
    "character",
    "effect",
    "script",
    "sound",
    "voice",
    "menu",
];

impl Scene {
    /// A fixed scene by name.
    #[must_use]
    pub fn fixed(name: &str) -> Option<Scene> {
        Some(match name {
            "synthetic" => Scene::Synthetic,
            "ship" => Scene::Ship("ge_fighter".to_owned()),
            "station" => Scene::Solar("largestation1".to_owned()),
            "character" => Scene::Character {
                body: "CHARACTERS\\BODIES\\br_bartender_body.dfm".to_owned(),
                animations: Some("CHARACTERS\\ANIMATIONS\\bodygenericmale.anm".to_owned()),
                animation: None,
            },
            "effect" => Scene::Effect {
                file: "FX\\ENGINES\\gf_br_csv_engine01.ale".to_owned(),
                name: None,
            },
            "script" => Scene::Script("MISSIONS\\M03\\m03_survivor1.thn".to_owned()),
            "sound" => Scene::Sound("hud_comm_window".to_owned()),
            "voice" => Scene::Voice {
                archive: "AUDIO\\announcer.utf".to_owned(),
                line: None,
            },
            "menu" => Scene::Page("INTERFACE\\INTRO\\page_charselectsingle.page".to_owned()),
            _ => return None,
        })
    }

    /// A short name for reports and file names.
    #[must_use]
    pub fn label(&self) -> String {
        match self {
            Scene::Synthetic => "synthetic".to_owned(),
            Scene::Ship(n) => format!("ship-{n}"),
            Scene::Solar(n) => format!("solar-{n}"),
            Scene::Model { path, .. } => format!("model-{}", base_name(path)),
            Scene::Character { body, .. } => format!("character-{}", base_name(body)),
            Scene::Effect { file, name } => {
                format!("effect-{}", name.clone().unwrap_or_else(|| base_name(file)))
            }
            Scene::Script(p) => format!("script-{}", base_name(p)),
            Scene::Sound(n) => format!("sound-{n}"),
            Scene::Voice { archive, .. } => format!("voice-{}", base_name(archive)),
            Scene::Page(p) => format!("page-{}", base_name(p)),
        }
    }

    /// Whether the scene needs the original data.
    #[must_use]
    pub fn needs_data(&self) -> bool {
        *self != Scene::Synthetic
    }
}

/// A path's last component without its extension, for file names.
fn base_name(path: &str) -> String {
    let name = path.rsplit(['\\', '/']).next().unwrap_or(path);
    let stem = name.split('.').next().unwrap_or(name);
    stem.chars()
        .map(|c| if c.is_ascii_alphanumeric() { c } else { '_' })
        .collect()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn every_fixed_scene_has_a_distinct_label() {
        let labels: Vec<String> = FIXED
            .iter()
            .map(|n| Scene::fixed(n).expect("a fixed scene").label())
            .collect();
        let mut unique = labels.clone();
        unique.sort();
        unique.dedup();
        assert_eq!(unique.len(), labels.len(), "{labels:?}");
        assert_eq!(
            Scene::fixed("character").map(|s| s.label()),
            Some("character-br_bartender_body".to_owned())
        );
        assert!(!Scene::Synthetic.needs_data());
    }
}
