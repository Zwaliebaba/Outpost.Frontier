//! The viewer in a window (Windows): the engine on the first hardware
//! adapter (or WARP) presenting into the window, the audio device open,
//! keys through the window's input router and the mouse's moves and
//! buttons for the orbit.

use std::time::Instant;

use neuron_client::engine::windows::create_engine;
use neuron_client::engine::{EngineConfig, FrameInput};
use neuron_client::input::keyboard::{KeyEvent, KeyEventKind, vk};
use neuron_client::input::mouse::{MouseEvent, WHEEL_BUTTON};
use neuron_client::platform::d3d12::AdapterChoice;
use neuron_client::platform::window::{InputRouter, Pump, Window};
use neuron_client::window::{DisplayMode, WindowEvent};

use crate::scenes::Scene;
use crate::viewer::Viewer;

/// What a key or the wheel asked for.
#[derive(Clone, Copy, Debug, PartialEq)]
enum Command {
    Next,
    Previous,
    Select(usize),
    Turn(f32, f32),
    Zoom(f32),
    Reframe,
    Spin,
    NextAnimation,
    Sound,
    Quit,
}

/// The viewer's input router: key downs become commands (and are
/// consumed), the wheel zooms, the left button drags the orbit.
#[derive(Default)]
struct Router {
    commands: Vec<Command>,
    dragging: Option<bool>,
}

impl InputRouter for Router {
    fn mouse_event(&mut self, event: MouseEvent) {
        if let MouseEvent::Button { index, down, .. } = event {
            if index == WHEEL_BUTTON {
                self.commands
                    .push(Command::Zoom(if down { -1.0 } else { 1.0 }));
            } else if index == 0 {
                self.dragging = Some(down);
            }
        }
    }

    fn key_event(&mut self, event: &KeyEvent) -> bool {
        if event.kind != KeyEventKind::Key || !event.down {
            return false;
        }
        let command = match event.key {
            vk::TAB | vk::RIGHT => Command::Next,
            vk::LEFT => Command::Previous,
            vk::ESCAPE => Command::Quit,
            vk::SPACE => Command::Spin,
            k @ 0x31..=0x39 => Command::Select((k - 0x31) as usize),
            0x41 => Command::Turn(-0.1, 0.0),
            0x44 => Command::Turn(0.1, 0.0),
            0x57 => Command::Turn(0.0, 0.1),
            0x53 => Command::Turn(0.0, -0.1),
            0x51 => Command::Zoom(1.0),
            0x45 => Command::Zoom(-1.0),
            0x52 => Command::Reframe,
            0x4E => Command::NextAnimation,
            0x50 => Command::Sound,
            _ => return false,
        };
        self.commands.push(command);
        true
    }
}

fn show(viewer: &mut Viewer<impl neuron_client::render::ff::device::Backend>, scene: &Scene) {
    let first = viewer.log.len();
    if let Err(e) = viewer.show(scene) {
        viewer.log.push(format!("{}: {e}", scene.label()));
    }
    for line in &viewer.log[first..] {
        println!("{line}");
    }
}

/// Runs the viewer in a window until it closes.
///
/// # Errors
///
/// The window or the engine could not be made, or the renderer failed.
pub fn run(
    mut config: EngineConfig,
    adapter: Option<AdapterChoice>,
    scenes: &[Scene],
    catalogs: &std::path::Path,
) -> Result<(), String> {
    let mode = DisplayMode {
        width: config.mode.width,
        height: config.mode.height,
        fullscreen: false,
    };
    config.mode = mode;
    config.audio_device = true;
    let mut window =
        Window::new("Outpost:Frontier model viewer", mode).map_err(|e| e.to_string())?;
    let engine = create_engine(config, adapter, Some(window.hwnd())).map_err(|e| e.to_string())?;
    for notice in &engine.notices {
        println!("engine: {notice}");
    }
    let mut viewer = Viewer::new(engine);
    println!("{}", viewer.load_catalogs(catalogs));
    let mut index = 0;
    show(&mut viewer, &scenes[index]);
    let start = Instant::now();
    let mut last = start;
    let mut last_status = start;
    let mut cursor: Option<(i32, i32)> = None;
    let mut dragging = false;
    loop {
        let mut router = Router::default();
        let events = match window.pump_routed(&mut router) {
            Pump::Quit(..) => return Ok(()),
            Pump::Events(events) => events,
        };
        for event in &events {
            match *event {
                WindowEvent::CloseRequested => return Ok(()),
                WindowEvent::MouseMoved { x, y } => {
                    if let (true, Some((px, py))) = (dragging, cursor) {
                        #[allow(clippy::cast_precision_loss)]
                        viewer
                            .orbit
                            .turn((x - px) as f32 * 0.01, (y - py) as f32 * 0.01);
                    }
                    cursor = Some((x, y));
                }
                _ => {}
            }
        }
        if let Some(down) = router.dragging {
            dragging = down;
        }
        for command in router.commands {
            let mut target = None;
            match command {
                Command::Quit => return Ok(()),
                Command::Next => target = Some((index + 1) % scenes.len()),
                Command::Previous => target = Some((index + scenes.len() - 1) % scenes.len()),
                Command::Select(i) if i < scenes.len() => target = Some(i),
                Command::Select(_) => {}
                Command::Turn(yaw, pitch) => viewer.orbit.turn(yaw, pitch),
                Command::Zoom(steps) => viewer.orbit.zoom(steps),
                Command::Reframe => target = Some(index),
                Command::Spin => viewer.spin = !viewer.spin,
                Command::NextAnimation => {
                    let from = viewer.animation_after();
                    if let Err(e) = viewer.next_animation(from) {
                        println!("{e}");
                    } else if let Some(line) = viewer.log.last() {
                        println!("{line}");
                    }
                }
                Command::Sound => {
                    if !viewer.replay_sound() {
                        println!("no sound to play in this scene");
                    }
                }
            }
            if let Some(i) = target {
                index = i;
                show(&mut viewer, &scenes[index]);
            }
        }
        let now = Instant::now();
        let dt = now.duration_since(last).as_secs_f64().min(0.25);
        last = now;
        let frame_ms = u32::try_from(now.duration_since(start).as_millis()).unwrap_or(u32::MAX);
        window.mouse_frame(frame_ms);
        viewer.frame(FrameInput {
            delta_seconds: dt,
            frame_ms,
            mouse: None,
            ui_input: Vec::new(),
        });
        if let Some(e) = viewer.engine.device().backend.errors.first() {
            return Err(format!("renderer: {e}"));
        }
        if now.duration_since(last_status).as_secs() >= 2 {
            last_status = now;
            println!("[{}] {}", scenes[index].label(), viewer.status());
        }
    }
}
