//! Bay, gate and docking animations as the simulation sees them: the
//! state machines of `CShip::open_bay`, `close_bay` and `update`'s bay
//! part (`Common/common_cship.cpp`), `CSolar::update_system_gate`
//! (`common_csolar.cpp`) and `CEqObj::update_docking_animations`
//! (`common_ceqobj.cpp`), over a [`Playback`] that stands for the
//! animation playback they drive (`EvalPlayback`, `GetStoredDuration`,
//! `GetScriptDuration`, `FilterObjects`). No channel is evaluated: the
//! playback's time runs at speed 1 between 0 and the script's duration
//! (divergence register).

fn e(v: f32) -> f64 {
    f64::from(v)
}

fn r(v: f64) -> f32 {
    v as f32
}

/// Which way a playback runs.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Hash)]
pub enum PlayDir {
    /// Stopped (finished, or never played).
    #[default]
    Stopped,
    /// Towards the end.
    Forward,
    /// Towards the start.
    Reverse,
}

/// An animation playback (`PlayScript`'s id): the longest channel's time.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct Playback {
    /// `GetScriptDuration`: the named animation's longest channel.
    pub duration: f32,
    /// `GetStoredDuration`: the time reached.
    pub time: f32,
    /// The direction it runs in.
    pub dir: PlayDir,
}

impl Playback {
    /// A playback of an animation `duration` seconds long, at its start.
    #[must_use]
    pub fn new(duration: f32) -> Self {
        Self {
            duration,
            time: 0.0,
            dir: PlayDir::Stopped,
        }
    }

    /// `EvalPlayback(id, reverse, start, 1.0, ..)`: `start` -2 keeps the
    /// time, -1 starts at the end, any other value is the start time.
    pub fn eval(&mut self, reverse: bool, start: f32) {
        if start == -1.0 {
            self.time = self.duration;
        } else if start != -2.0 {
            self.time = start;
        }
        self.dir = if reverse {
            PlayDir::Reverse
        } else {
            PlayDir::Forward
        };
    }

    /// `MixerWalk`: the time moves by `dt` and stops at either end.
    pub fn advance(&mut self, dt: f32) {
        match self.dir {
            PlayDir::Stopped => {}
            PlayDir::Forward => {
                let t = r(e(self.time) + e(dt));
                if t >= self.duration {
                    self.time = self.duration;
                    self.dir = PlayDir::Stopped;
                } else {
                    self.time = t;
                }
            }
            PlayDir::Reverse => {
                let t = r(e(self.time) - e(dt));
                if t <= 0.0 {
                    self.time = 0.0;
                    self.dir = PlayDir::Stopped;
                } else {
                    self.time = t;
                }
            }
        }
    }

    /// `FilterObjects`: the playback has run to its end.
    #[must_use]
    pub fn finished(&self) -> bool {
        self.dir == PlayDir::Stopped
    }
}

/// `CShip`'s bay states (`get_bay_state`).
pub use super::ship::BayState;

/// What a bay request did.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct BayRequest {
    /// The time the doors need (the returned value).
    pub time: f64,
    /// The doors started to move: `notify(BayStateChanged, open)` and the
    /// shields' activation (off on opening, on on closing).
    pub changed: Option<bool>,
    /// The playback started (`AnimDB::Add`).
    pub play: bool,
}

/// `CShip::open_bay`: the first request opens a closed or closing bay.
pub fn open_bay(
    counter: &mut i32,
    state: &mut BayState,
    anim: Option<&mut Playback>,
) -> BayRequest {
    let old = *counter;
    *counter = old + 1;
    let none = BayRequest {
        time: 0.0,
        changed: None,
        play: false,
    };
    if old != 0 {
        return match anim {
            Some(a) => BayRequest {
                time: e(a.duration) - e(a.time),
                ..none
            },
            None => none,
        };
    }
    if *state != BayState::Closing && *state != BayState::Closed {
        return none;
    }
    let Some(a) = anim else {
        *state = BayState::Open;
        return none;
    };
    *state = BayState::Opening;
    a.eval(false, -2.0);
    BayRequest {
        time: e(r(e(a.duration) - e(a.time))),
        changed: Some(true),
        play: true,
    }
}

/// `CShip::close_bay`: the last request closes an open or opening bay.
pub fn close_bay(
    counter: &mut i32,
    state: &mut BayState,
    anim: Option<&mut Playback>,
) -> BayRequest {
    *counter -= 1;
    let none = BayRequest {
        time: 0.0,
        changed: None,
        play: false,
    };
    if *counter != 0 {
        return BayRequest {
            time: anim.map_or(-1.0, |a| e(a.time)),
            ..none
        };
    }
    if *state != BayState::Opening && *state != BayState::Open {
        return none;
    }
    let Some(a) = anim else {
        *state = BayState::Closed;
        return none;
    };
    *state = BayState::Closing;
    a.eval(true, -2.0);
    BayRequest {
        time: e(a.time),
        changed: Some(false),
        play: true,
    }
}

/// `CShip::update`'s bay part: an opening bay is open once its playback
/// reached the script's duration, a closing one closed at 0.
pub fn update_bay(state: &mut BayState, anim: Option<&Playback>) {
    let (time, duration) = anim.map_or((-1.0, -1.0), |a| (a.time, a.duration));
    match *state {
        BayState::Opening if time >= duration => *state = BayState::Open,
        BayState::Closing if time <= 0.0 => *state = BayState::Closed,
        _ => {}
    }
}

/// What one `update_system_gate` did.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct GateStep {
    /// `notify(1, &code)`.
    pub notify: Option<i32>,
    /// The playback was started (`EvalPlayback` and `AnimDB::Add`), and
    /// whether in reverse.
    pub play: Option<bool>,
}

/// `CSolar::update_system_gate`. `opening` is `GateAnimDirection`;
/// `finished` answers `FilterObjects` on the gate's playback.
pub fn update_system_gate(
    state: &mut i32,
    timer: &mut f32,
    opening: bool,
    has_anim: bool,
    finished: bool,
    dt: f32,
) -> GateStep {
    let s = *state;
    match s {
        0 => *timer = r(e(dt) + e(*timer)),
        2 => *timer = r(e(*timer) - e(dt)),
        _ => *timer = 0.0,
    }
    let mut out = GateStep::default();
    if opening {
        match s {
            3 | 4 => {
                out.notify = Some(0);
                *state = 0;
                *timer = 0.0;
            }
            0 if !(*timer > 10.0) => {
                if has_anim {
                    out.play = Some(false);
                }
            }
            0 | 2 => {
                *state = 1;
                out.notify = Some(1);
            }
            _ => {}
        }
        return out;
    }
    match s {
        0 | 1 => {
            *state = 2;
            *timer = 5.0;
        }
        2 => {
            if !(*timer <= 0.0) {
                return out;
            }
            if has_anim {
                out.play = Some(true);
            }
            out.notify = Some(2);
            *state = 3;
        }
        3 if finished => {
            *state = 4;
            out.notify = Some(3);
        }
        _ => {}
    }
    out
}

/// What `update_docking_animations` does to one docking point.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct DockStep {
    /// `notify(DockAnimEnded, &flag)`.
    pub notify: Option<bool>,
    /// The playback was told to run (reverse, start), and whether it was
    /// registered (`AnimDB::Add`).
    pub play: Option<(bool, f32, bool)>,
}

/// `CEqObj::update_docking_animations` for one point: an active point
/// opens (restarting from 0 at the closed end, continuing while it does
/// not move), an inactive one closes (from the end once open).
pub fn update_dock_point(active: bool, anim: &Playback, last: &mut f32) -> DockStep {
    let d = anim.time;
    let p = anim.duration;
    let mut out = DockStep::default();
    if active {
        if d < p {
            if d == 0.0 {
                out.notify = Some(true);
                out.play = Some((false, 0.0, true));
            } else if d <= *last {
                out.play = Some((false, -2.0, false));
            }
        }
    } else if d > 0.0 {
        if d >= p {
            out.notify = Some(false);
            out.play = Some((true, -1.0, true));
        } else if d >= *last {
            out.play = Some((true, -2.0, false));
        }
    }
    *last = d;
    out
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn bay_opens_and_closes_over_the_script() {
        let mut anim = Playback::new(1.5);
        let (mut counter, mut state) = (0, BayState::Closed);
        let req = open_bay(&mut counter, &mut state, Some(&mut anim));
        assert_eq!(state, BayState::Opening);
        assert_eq!(req.changed, Some(true));
        assert!((req.time - 1.5).abs() < 1e-6);
        for _ in 0..30 {
            anim.advance(1.0 / 30.0);
            update_bay(&mut state, Some(&anim));
        }
        assert_eq!(state, BayState::Opening);
        for _ in 0..16 {
            anim.advance(1.0 / 30.0);
            update_bay(&mut state, Some(&anim));
        }
        assert_eq!(state, BayState::Open);
        // A second request only counts.
        assert_eq!(
            open_bay(&mut counter, &mut state, Some(&mut anim)).changed,
            None
        );
        assert_eq!(
            close_bay(&mut counter, &mut state, Some(&mut anim)).changed,
            None
        );
        assert_eq!(
            close_bay(&mut counter, &mut state, Some(&mut anim)).changed,
            Some(false)
        );
        assert_eq!(state, BayState::Closing);
        for _ in 0..46 {
            anim.advance(1.0 / 30.0);
            update_bay(&mut state, Some(&anim));
        }
        assert_eq!(state, BayState::Closed);
    }

    #[test]
    fn a_bay_without_animation_switches_at_once() {
        let (mut counter, mut state) = (0, BayState::Closed);
        open_bay(&mut counter, &mut state, None);
        assert_eq!(state, BayState::Open);
        close_bay(&mut counter, &mut state, None);
        assert_eq!(state, BayState::Closed);
    }

    #[test]
    fn gate_cycles() {
        let (mut state, mut timer) = (4, 0.0f32);
        let dt = 0.5;
        // Opening from closed: code 0, then the animation plays in state 0
        // until the timer passes 10 s, then state 1 with code 1.
        assert_eq!(
            update_system_gate(&mut state, &mut timer, true, true, false, dt).notify,
            Some(0)
        );
        assert_eq!(state, 0);
        let mut steps = 0;
        loop {
            steps += 1;
            let s = update_system_gate(&mut state, &mut timer, true, true, false, dt);
            if s.notify == Some(1) {
                break;
            }
            assert_eq!(s.play, Some(false));
        }
        assert_eq!(state, 1);
        assert_eq!(steps, 21);
        // Closing: 5 s in state 2, then the reverse playback and code 2,
        // then code 3 once the playback finished.
        update_system_gate(&mut state, &mut timer, false, true, false, dt);
        assert_eq!((state, timer), (2, 5.0));
        let mut closing = 0;
        loop {
            closing += 1;
            let s = update_system_gate(&mut state, &mut timer, false, true, false, dt);
            if s.notify == Some(2) {
                assert_eq!(s.play, Some(true));
                break;
            }
        }
        assert_eq!(closing, 10);
        assert_eq!(
            update_system_gate(&mut state, &mut timer, false, true, false, dt).notify,
            None
        );
        assert_eq!(
            update_system_gate(&mut state, &mut timer, false, true, true, dt).notify,
            Some(3)
        );
        assert_eq!(state, 4);
    }

    #[test]
    fn dock_point_opens_from_zero() {
        let anim = Playback::new(2.0);
        let mut last = 0.0;
        let s = update_dock_point(true, &anim, &mut last);
        assert_eq!(s.notify, Some(true));
        assert_eq!(s.play, Some((false, 0.0, true)));
        let mut open = Playback::new(2.0);
        open.time = 2.0;
        let s = update_dock_point(false, &open, &mut last);
        assert_eq!(s.notify, Some(false));
        assert_eq!(s.play, Some((true, -1.0, true)));
    }
}
