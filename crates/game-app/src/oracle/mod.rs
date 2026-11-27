//! The port's `+oracle` mode (decision O53 (2), record 0009 point 6): the
//! C++ client's script language and JSON Lines files
//! ([reference-build.md](../../../design/reference-build.md) section 8),
//! written at the same hooks so that CI compares the two clients' files.

pub mod compare;
pub mod iclient;
pub mod json;
pub mod keymap;
pub mod layout;
pub mod room;
pub mod script;
pub mod tree;

use std::cell::RefCell;
use std::io::Write as _;
use std::path::PathBuf;

use neuron_client::ui::services::HudRecord;
use neuron_client::ui::tree::Ui;

use crate::hud_events::{self, HudEvents};
use crate::keymap as key_map;
use crate::startup_config::DisplayMode;

/// `kOracleWidth`, `kOracleHeight`, `kOracleColorBits`.
pub const DISPLAY: (u32, u32, i32) = (0x400, 0x300, 0x20);

/// The files of one run (`kStreamFiles`), in stream order.
pub const FILES: [&str; 5] = [
    "session.jsonl",
    "tree.jsonl",
    "keymap.jsonl",
    "layout.jsonl",
    "iclient.jsonl",
];

/// One oracle run of the port's client: the script, the output folder and
/// the files' lines (`session.jsonl` and `tree.jsonl` are the script's own;
/// `iclient.jsonl` is the receiver's log).
#[derive(Debug)]
pub struct OracleRun {
    /// The script, `session.jsonl` and `tree.jsonl`.
    pub script: script::OracleScript,
    /// The output folder.
    pub out: PathBuf,
    /// `keymap.jsonl`.
    pub keymap: Vec<String>,
    /// `layout.jsonl`.
    pub layout: Vec<String>,
}

impl OracleRun {
    /// `Start(script, out)`: the script read and the output folder made (its
    /// parent must exist); `Err` with the C++'s message otherwise (it then
    /// ends with exit code 3).
    ///
    /// # Errors
    ///
    /// The message `Fail` prints.
    pub fn open(script_path: &str, out: &str) -> Result<Self, String> {
        let text = std::fs::read(script_path).map_err(|_| {
            format!("OutpostFrontier +oracle: cannot read the script: {script_path}")
        })?;
        match std::fs::create_dir(out) {
            Ok(()) => {}
            Err(e) if e.kind() == std::io::ErrorKind::AlreadyExists => {}
            Err(_) => {
                return Err(format!(
                    "OutpostFrontier +oracle: cannot create the output folder: {out}"
                ));
            }
        }
        let text = String::from_utf8_lossy(&text);
        Ok(Self {
            script: script::OracleScript::start(&text, script_path, out),
            out: PathBuf::from(out.trim_end_matches(['\\', '/'])),
            keymap: Vec::new(),
            layout: Vec::new(),
        })
    }

    /// `OracleForceDisplayMode`: windowed 1024 by 768 at 32 bits, and the
    /// `display` record.
    pub fn force_display(&mut self, mode: &mut DisplayMode) {
        mode.fullscreen = false;
        mode.width = DISPLAY.0;
        mode.height = DISPLAY.1;
        mode.color_bpp = DISPLAY.2;
        self.script.note_display(
            mode.width.cast_signed(),
            mode.height.cast_signed(),
            mode.color_bpp,
            i32::from(mode.fullscreen),
        );
    }

    /// The five files written, each line ended by `\n`.
    ///
    /// # Errors
    ///
    /// When a file cannot be written.
    pub fn write(&self, iclient: &[String]) -> std::io::Result<()> {
        let streams: [&[String]; 5] = [
            self.script.session(),
            self.script.tree(),
            &self.keymap,
            &self.layout,
            iclient,
        ];
        for (name, lines) in FILES.iter().zip(streams) {
            let mut file = std::fs::File::create(self.out.join(name))?;
            for line in lines {
                file.write_all(line.as_bytes())?;
                file.write_all(b"\n")?;
            }
        }
        Ok(())
    }
}

/// The client's answers to the script for one frame: the tree of its
/// interface, clicks on it and the key map's commands; no screenshot or key
/// yet (D1114); the pending state it takes.
pub struct ClientHost<'a> {
    /// The engine's interface (the `Win3D` registry), when it has one.
    pub ui: Option<&'a mut Ui>,
    /// The state the script asked for (`state`, or 1 at its end).
    pub pending_state: &'a mut Option<u32>,
    /// `G_KeyMap`.
    pub keymap: &'a key_map::KeyMap,
    /// The HUD command dispatcher's registry.
    pub events: &'a RefCell<HudEvents>,
}

impl ClientHost<'_> {
    /// `DoCommand(nickname)`: the command's id from its nickname; the event
    /// record filled from its first active binding, else its first
    /// binding; `HudDispatchCommand(id, 1)` on key down if its state has
    /// `keydown` (a command the key map lacks has only that) and again on
    /// key up if it has `keyup`. The note gives the id, whether the binding
    /// was active, the key, its modifiers and each dispatch's answer (-1
    /// for none).
    fn do_command(&mut self, nickname: &str) -> Result<String, String> {
        let id = key_map::nickname_id(nickname.as_bytes());
        if id == 0 {
            return Err("unknown nickname".into());
        }
        let mask = self
            .keymap
            .find_by_id(id)
            .map_or(key_map::STATE_KEY_DOWN, |i| self.keymap.commands[i].state);
        let active = self.keymap.active_binding(id);
        let binding = active
            .or_else(|| self.keymap.binding(id, 0))
            .unwrap_or_default();
        let events = self.events;
        let mut ui = self.ui.as_deref_mut();
        let mut dispatch = |message: u32, down: bool| -> i32 {
            events.borrow_mut().record = HudRecord {
                key: binding.virtual_key.cast_unsigned(),
                modifiers: binding.modifiers,
                message,
                wparam: binding.virtual_key.cast_unsigned(),
                down,
                ..HudRecord::default()
            };
            let taken = ui
                .as_deref_mut()
                .is_some_and(|ui| hud_events::dispatch(ui, events, id, 1));
            i32::from(taken)
        };
        let down = if mask & key_map::STATE_KEY_DOWN != 0 {
            dispatch(hud_events::WM_KEYDOWN, true)
        } else {
            -1
        };
        let up = if mask & key_map::STATE_KEY_UP != 0 {
            dispatch(hud_events::WM_KEYUP, false)
        } else {
            -1
        };
        Ok(format!(
            "id={id} active={} vk={} mods={} down={down} up={up}",
            i32::from(active.is_some()),
            binding.virtual_key,
            binding.modifiers,
        ))
    }
}

impl script::OracleHost for ClientHost<'_> {
    /// Not ported yet (D1114): no screenshot is taken.
    fn take_shot(&mut self, _: &str) -> bool {
        false
    }
    fn tree(&mut self, dump: u32) -> tree::TreeDump {
        self.ui
            .as_deref()
            .map(|ui| tree::dump_nodes(ui, dump))
            .unwrap_or_default()
    }
    fn command(&mut self, nickname: &str) -> Result<String, String> {
        self.do_command(nickname)
    }
    /// Not ported yet (D1114): posting `WM_KEYDOWN` and `WM_KEYUP` to the
    /// window needs the window and the key map's routing of key messages.
    fn key(&mut self, _: &str) -> Result<String, String> {
        Err("not ported yet".into())
    }
    fn click(&mut self, spec: &str) -> Result<String, String> {
        match self.ui.as_deref_mut() {
            Some(ui) => tree::click(ui, spec),
            None => Err("not found".into()),
        }
    }
    fn set_pending_state(&mut self, id: u32) {
        *self.pending_state = Some(id);
    }
}
