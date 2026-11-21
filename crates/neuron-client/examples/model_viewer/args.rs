//! The command line.

use std::path::PathBuf;

use crate::scenes::{FIXED, Scene};

/// How to run.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Args {
    /// The data root (the game's `DATA` directory, `outdir/data`).
    pub data: Option<PathBuf>,
    /// Where `NeuronComponents.ini` and `OutpostFrontier.ini` are; the data
    /// root's parent when not given.
    pub ini: Option<PathBuf>,
    /// The text catalogs (`cargo xtask catalog`'s output).
    pub catalogs: PathBuf,
    pub width: u32,
    pub height: u32,
    /// Run the scenes without a window and write screenshots.
    pub headless: bool,
    /// The headless output directory.
    pub out: Option<PathBuf>,
    /// Frames each headless scene runs before its screenshot.
    pub frames: u32,
    /// `warp` or `hardware`; the first hardware adapter, else WARP, when
    /// not given (headless runs default to WARP).
    pub adapter: Option<String>,
    /// Open the audio device in a headless run (a window run always does).
    pub audio: bool,
    /// The scenes, in order.
    pub scenes: Vec<Scene>,
    pub help: bool,
}

pub const USAGE: &str = "\
model_viewer: the Gate G4 viewer over neuron-client's engine

  cargo run -p neuron-client --example model_viewer -- [options]

  --data DIR          the data root (outdir\\data); without it only the
                      synthetic scene runs
  --ini DIR           where NeuronComponents.ini and OutpostFrontier.ini are
                      (default: the data root's parent)
  --catalogs DIR      the text catalogs `cargo xtask catalog` builds
                      (default target\\catalogs)
  --size WxH          the back buffer (default 1024x768)
  --headless          no window: run each scene, write screenshots
  --out DIR           the headless output directory (default viewer-out)
  --frames N          frames per headless scene (default 30, at 1/30 s)
  --adapter warp|hardware
  --audio             open the audio device in a headless run
  --scenes a,b,...    fixed scenes: synthetic ship station character effect
                      script sound voice menu, or all
  --ship NICK         a ship of SHIPS\\shiparch.ini
  --solar NICK        a station or other solar of SOLAR\\solararch.ini
  --model PATH[,LIB]  a model file and its material libraries
  --character DFM[,ANM[,NAME]]
  --effect ALE[,NAME]
  --script THN
  --sound NICK        a [Sound] of the sound tables
  --voice VOICE[,MSG] a line of a voice profile (default: its first)
  --page PATH         a page descriptor drawn over the scene

Window keys: Tab or Right / Left next and previous scene, 1-9 a scene,
A D W S orbit, Q E or the wheel zoom, left drag orbits, R reframe,
Space spin, N next animation, P play the sound again, Esc quit.";

/// Where `cargo xtask catalog` writes the catalogs, from the repository
/// root.
pub const DEFAULT_CATALOGS: &str = "target/catalogs";

fn split(value: &str) -> Vec<String> {
    value.split(',').map(str::to_owned).collect()
}

/// Parses the arguments after the program name.
///
/// # Errors
///
/// An unknown option, a missing value or a bad number.
pub fn parse(args: impl IntoIterator<Item = String>) -> Result<Args, String> {
    let mut out = Args {
        data: None,
        ini: None,
        catalogs: PathBuf::from(DEFAULT_CATALOGS),
        width: 1024,
        height: 768,
        headless: false,
        out: None,
        frames: 30,
        adapter: None,
        audio: false,
        scenes: Vec::new(),
        help: false,
    };
    let mut fixed: Option<Vec<String>> = None;
    let mut it = args.into_iter();
    while let Some(arg) = it.next() {
        let mut value = || it.next().ok_or_else(|| format!("{arg} needs a value"));
        match arg.as_str() {
            "--help" | "-h" => out.help = true,
            "--headless" => out.headless = true,
            "--audio" => out.audio = true,
            "--data" => out.data = Some(PathBuf::from(value()?)),
            "--ini" => out.ini = Some(PathBuf::from(value()?)),
            "--catalogs" => out.catalogs = PathBuf::from(value()?),
            "--out" => out.out = Some(PathBuf::from(value()?)),
            "--adapter" => {
                let v = value()?;
                if v != "warp" && v != "hardware" {
                    return Err(format!("--adapter {v}: warp or hardware"));
                }
                out.adapter = Some(v);
            }
            "--frames" => {
                out.frames = value()?
                    .parse()
                    .map_err(|_| "--frames needs a number".to_owned())?;
            }
            "--size" => {
                let v = value()?;
                let (w, h) = v
                    .split_once(['x', 'X'])
                    .ok_or_else(|| format!("--size {v}: WIDTHxHEIGHT"))?;
                out.width = w.parse().map_err(|_| format!("--size {v}"))?;
                out.height = h.parse().map_err(|_| format!("--size {v}"))?;
                if out.width == 0 || out.height == 0 {
                    return Err(format!("--size {v}"));
                }
            }
            "--scenes" => fixed = Some(split(&value()?)),
            "--ship" => out.scenes.push(Scene::Ship(value()?)),
            "--solar" | "--station" => out.scenes.push(Scene::Solar(value()?)),
            "--model" => {
                let mut parts = split(&value()?).into_iter();
                let path = parts.next().unwrap_or_default();
                out.scenes.push(Scene::Model {
                    path,
                    libraries: parts.collect(),
                });
            }
            "--character" => {
                let mut parts = split(&value()?).into_iter();
                out.scenes.push(Scene::Character {
                    body: parts.next().unwrap_or_default(),
                    animations: parts.next(),
                    animation: parts.next(),
                });
            }
            "--effect" => {
                let mut parts = split(&value()?).into_iter();
                out.scenes.push(Scene::Effect {
                    file: parts.next().unwrap_or_default(),
                    name: parts.next(),
                });
            }
            "--script" => out.scenes.push(Scene::Script(value()?)),
            "--sound" => out.scenes.push(Scene::Sound(value()?)),
            "--voice" => {
                let mut parts = split(&value()?).into_iter();
                out.scenes.push(Scene::Voice {
                    profile: parts.next().unwrap_or_default(),
                    msg: parts.next(),
                });
            }
            "--page" => out.scenes.push(Scene::Page(value()?)),
            other => return Err(format!("unknown option {other} (see --help)")),
        }
    }
    let names: Vec<String> = match fixed {
        Some(names) if names.iter().any(|n| n == "all") => {
            FIXED.iter().map(|&n| n.to_owned()).collect()
        }
        Some(names) => names,
        None if out.scenes.is_empty() && out.data.is_some() => {
            FIXED.iter().map(|&n| n.to_owned()).collect()
        }
        None if out.scenes.is_empty() => vec!["synthetic".to_owned()],
        None => Vec::new(),
    };
    for name in names {
        let scene = Scene::fixed(&name).ok_or_else(|| format!("no fixed scene {name}"))?;
        out.scenes.push(scene);
    }
    Ok(out)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn args(line: &str) -> Result<Args, String> {
        parse(line.split_whitespace().map(str::to_owned))
    }

    #[test]
    fn the_scenes_come_from_the_options() {
        let a = args("").unwrap();
        assert_eq!(a.scenes, vec![Scene::Synthetic]);
        let a = args("--data d").unwrap();
        assert_eq!(a.scenes.len(), FIXED.len());
        let a = args("--data d --ship li_elite --scenes menu,ship").unwrap();
        assert_eq!(a.scenes[0], Scene::Ship("li_elite".to_owned()));
        assert_eq!(a.scenes[1], Scene::fixed("menu").unwrap());
        let a = args("--character a.dfm,b.anm --headless --size 640x480 --frames 3").unwrap();
        assert!(a.headless);
        assert_eq!((a.width, a.height, a.frames), (640, 480, 3));
        assert_eq!(
            a.scenes,
            vec![Scene::Character {
                body: "a.dfm".to_owned(),
                animations: Some("b.anm".to_owned()),
                animation: None
            }]
        );
        assert!(args("--size 0x4").is_err());
        assert!(args("--scenes nothing").is_err());
        assert!(args("--bogus").is_err());
        assert!(args("--adapter gpu").is_err());
    }
}
