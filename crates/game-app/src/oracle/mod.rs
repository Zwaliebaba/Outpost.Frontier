//! The port's `+oracle` mode (decision O53 (2), record 0009 point 6): the
//! C++ client's script language and JSON Lines files
//! ([reference-build.md](../../../design/reference-build.md) section 8),
//! written at the same hooks so that CI compares the two clients' files.

pub mod iclient;
pub mod json;
pub mod keymap;
pub mod layout;
pub mod script;
pub mod tree;

use std::io::Write as _;
use std::path::PathBuf;

use neuron_client::ui::tree::Ui;

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
/// interface and clicks on it; no screenshot, command or key yet (they come
/// with the screens); the pending state it takes.
pub struct ClientHost<'a> {
    /// The engine's interface (the `Win3D` registry), when it has one.
    pub ui: Option<&'a mut Ui>,
    /// The state the script asked for (`state`, or 1 at its end).
    pub pending_state: &'a mut Option<u32>,
}

impl script::OracleHost for ClientHost<'_> {
    fn take_shot(&mut self, _: &str) -> bool {
        false
    }
    fn tree(&mut self, dump: u32) -> tree::TreeDump {
        self.ui
            .as_deref()
            .map(|ui| tree::dump_nodes(ui, dump))
            .unwrap_or_default()
    }
    fn command(&mut self, _: &str) -> Result<String, String> {
        Err("not ported yet".into())
    }
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
