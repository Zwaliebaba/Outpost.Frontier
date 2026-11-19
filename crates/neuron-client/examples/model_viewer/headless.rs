//! The headless mode: each scene shown for a fixed number of frames at
//! 1/30 s, then the frame read back and written as a BMP screenshot, and
//! a report of what each scene loaded, drew and played.

use std::fmt::Write as _;
use std::path::Path;

use neuron_client::engine::{FrameInput, FrameReport};
use neuron_client::render::ff::device::{Backend, RecordingBackend};
use neuron_client::render::ff::screenshot;

use crate::scenes::Scene;
use crate::viewer::Viewer;

/// The frame time of a headless run.
pub const STEP: f64 = 1.0 / 30.0;

/// What a backend can say about a frame beyond the image.
pub trait Probe {
    /// The draw packets submitted so far, where the backend counts them.
    fn draw_count(&self) -> Option<usize>;
    /// The errors the backend reported so far.
    fn error_count(&self) -> usize;
}

impl Probe for RecordingBackend {
    fn draw_count(&self) -> Option<usize> {
        Some(self.draws.len())
    }
    fn error_count(&self) -> usize {
        0
    }
}

#[cfg(windows)]
impl Probe for neuron_client::platform::d3d12::backend::D3d12Backend {
    fn draw_count(&self) -> Option<usize> {
        None
    }
    fn error_count(&self) -> usize {
        self.errors.len()
    }
}

/// How the frame's image looks: enough to tell a picture from a cleared
/// target.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct ImageStats {
    pub width: u32,
    pub height: u32,
    /// Pixels that are not opaque black.
    pub lit: usize,
    /// Distinct colours, counted up to 4096.
    pub colours: usize,
}

impl ImageStats {
    /// The statistics of a BGRA image.
    #[must_use]
    pub fn of(width: u32, height: u32, bgra: &[u8]) -> ImageStats {
        let mut seen = std::collections::BTreeSet::new();
        let mut lit = 0;
        for p in bgra.as_chunks::<4>().0 {
            if p[..3] != [0, 0, 0] {
                lit += 1;
            }
            if seen.len() < 4096 {
                seen.insert(*p);
            }
        }
        ImageStats {
            width,
            height,
            lit,
            colours: seen.len(),
        }
    }

    /// Whether the image holds a picture: some lit pixels in more than
    /// one colour.
    #[must_use]
    #[cfg_attr(not(all(windows, test)), allow(dead_code))]
    pub fn non_trivial(&self) -> bool {
        self.lit > 0 && self.colours > 1
    }
}

/// What one scene did.
#[derive(Debug)]
pub struct SceneRun {
    pub label: String,
    pub result: Result<(), String>,
    pub notes: Vec<String>,
    pub last: FrameReport,
    pub status: String,
    pub draws: Option<usize>,
    pub errors: usize,
    pub image: Option<(ImageStats, String)>,
}

/// Shows `scene`, runs `frames` frames and reads the last one back; with
/// `out`, writes the screenshot there.
pub fn run_scene<B: Backend + Probe + 'static>(
    viewer: &mut Viewer<B>,
    scene: &Scene,
    frames: u32,
    out: Option<&Path>,
) -> SceneRun {
    let label = scene.label();
    let first_note = viewer.log.len();
    let result = viewer.show(scene);
    let draws_before = viewer.engine.device().backend.draw_count();
    let mut last = FrameReport::default();
    if result.is_ok() {
        for _ in 0..frames.max(1) {
            last = viewer.frame(FrameInput::idle(STEP));
        }
    }
    let status = viewer.status();
    let mut device = viewer.engine.device();
    let draws = device
        .backend
        .draw_count()
        .zip(draws_before)
        .map(|(after, before)| after - before);
    let errors = device.backend.error_count();
    let image = if result.is_ok() {
        device.backend.read_scene().map(|(width, height, bgra)| {
            let stats = ImageStats::of(width, height, &bgra);
            let file = format!("{label}.bmp");
            let written = match (out, screenshot::encode(&file, width, height, &bgra)) {
                (Some(dir), Ok(bytes)) => match std::fs::write(dir.join(&file), bytes) {
                    Ok(()) => file,
                    Err(e) => format!("{file} not written: {e}"),
                },
                (None, _) => "not written".to_owned(),
                (_, Err(e)) => format!("{file} not encoded: {e:?}"),
            };
            (stats, written)
        })
    } else {
        None
    };
    drop(device);
    SceneRun {
        label,
        result,
        notes: viewer.log[first_note..].to_vec(),
        last,
        status,
        draws,
        errors,
        image,
    }
}

/// Runs every scene and writes the screenshots and `report.txt` into
/// `out`. The report's text.
///
/// # Errors
///
/// The output directory or the report cannot be written.
pub fn run<B: Backend + Probe + 'static>(
    viewer: &mut Viewer<B>,
    scenes: &[Scene],
    frames: u32,
    out: &Path,
    header: &str,
) -> std::io::Result<(String, Vec<SceneRun>)> {
    std::fs::create_dir_all(out)?;
    let mut runs = Vec::new();
    for scene in scenes {
        let run = run_scene(viewer, scene, frames, Some(out));
        println!("{}", summary(&run));
        runs.push(run);
    }
    viewer.clear();
    let report = report(header, frames, &runs, &viewer.engine.notices);
    std::fs::write(out.join("report.txt"), &report)?;
    Ok((report, runs))
}

/// One line on a scene.
#[must_use]
pub fn summary(run: &SceneRun) -> String {
    let mut line = format!("{}: ", run.label);
    match &run.result {
        Ok(()) => line.push_str("ok"),
        Err(e) => {
            let _ = write!(line, "failed: {e}");
        }
    }
    let r = run.last;
    let _ = write!(
        line,
        "; last frame: models {}, deformables {}, animations {}, effects {}, scripts {}, \
         sounds {}, UI draws {} (control meshes {}, text tiles {}, quad lists {}{})",
        r.models,
        r.deformables,
        r.animations,
        r.effects,
        r.scripts,
        r.sounds,
        r.ui_draws,
        r.ui_meshes,
        r.ui_tiles,
        r.quad_lists,
        if r.cursor { ", cursor" } else { "" }
    );
    if let Some(d) = run.draws {
        let _ = write!(line, "; {d} draw packets");
    }
    if run.errors > 0 {
        let _ = write!(line, "; {} renderer errors", run.errors);
    }
    if let Some((stats, file)) = &run.image {
        let _ = write!(
            line,
            "; image {}x{}, {} lit pixels, {} colours ({file})",
            stats.width, stats.height, stats.lit, stats.colours
        );
    }
    line
}

/// The report of a run.
#[must_use]
pub fn report(header: &str, frames: u32, runs: &[SceneRun], notices: &[String]) -> String {
    let mut text = format!(
        "{header}\n{} scenes, {frames} frames each at 1/30 s\n\n",
        runs.len()
    );
    for run in runs {
        let _ = writeln!(text, "{}", summary(run));
        for note in &run.notes {
            let _ = writeln!(text, "    {note}");
        }
        if !run.status.is_empty() {
            let _ = writeln!(text, "    {}", run.status);
        }
    }
    if !notices.is_empty() {
        let _ = writeln!(text, "\nengine notices:");
        for notice in notices {
            let _ = writeln!(text, "    {notice}");
        }
    }
    text
}
