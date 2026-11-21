//! The judges of a run against the oracle's (record 0003 points 8 and 9):
//! a scenario without contact exactly, a contact scenario and a fight bit
//! for bit up to the first contact or munition impact, the guided
//! munitions' launches and ends, and the gross bound on the trajectories.
//! Each writes its findings to the summary and pushes what fails the
//! comparison to `gross`.

use std::collections::BTreeMap;
use std::fmt::Write as _;
use std::path::Path;

use neuron_core::math::Vector;
use neuron_core::physics::replay::{CoreRecord, compare};

use super::driver::{Contact, GuidedState, Run, State};
use super::file::Scenario;
use super::oracle::{AiInputs, oracle_guided, oracle_guided_cores};
use super::records::{Json, find_files, read_lines};

/// The gross-divergence bound on position, metres.
pub const GROSS_POSITION: f64 = 500.0;

/// The distance between two points, in `f64`.
#[must_use]
pub fn dist(a: Vector, b: Vector) -> f64 {
    let d = [
        f64::from(a.x) - f64::from(b.x),
        f64::from(a.y) - f64::from(b.y),
        f64::from(a.z) - f64::from(b.z),
    ];
    (d[0] * d[0] + d[1] * d[1] + d[2] * d[2]).sqrt()
}

/// A collision event as the reports print it.
#[must_use]
pub fn fmt_contact(c: &Contact) -> String {
    let speed = c.speed.map_or_else(|| "-".to_owned(), |v| v.to_string());
    format!("against {} speed {speed} damage {}", c.b, c.damage)
}

/// Whether two states are the same, bit for bit where it counts: alive,
/// position, velocity, orientation, hit points and the shield's hit points.
#[must_use]
pub fn same_state(a: &State, b: &State) -> bool {
    let bits = |v: Vector| [v.x.to_bits(), v.y.to_bits(), v.z.to_bits()];
    a.alive == b.alive
        && (!a.alive
            || (bits(a.pos) == bits(b.pos)
                && bits(a.vel) == bits(b.vel)
                && a.orient.map(f32::to_bits) == b.orient.map(f32::to_bits)
                && a.hp.to_bits() == b.hp.to_bits()
                && match (a.shield, b.shield) {
                    (Some(x), Some(y)) => x.hp.to_bits() == y.hp.to_bits(),
                    (_, None) => true,
                    (None, Some(_)) => false,
                }))
}

/// A contact scenario (`ram_ship`, Gate G2's collide check; owner,
/// 2026-11-18, record 0003 point 9): the run is judged to its first
/// contact. Every object must equal the oracle exactly before the tick of
/// the oracle's first collision event, the Rust world's first collision
/// event must come in the same tick, and at the last tick every object
/// must be alive or gone as in the oracle. The first contact's damage and
/// the trajectories after it are reported, not judged (their envelopes
/// wait for the owner's G2 numbers).
pub fn judge_contact(
    summary: &mut String,
    gross: &mut Vec<String>,
    s: &Scenario,
    ours: &Run,
    theirs: &Run,
) {
    let Some(first) = theirs.contacts.iter().map(|c| c.tick).min() else {
        gross.push(format!("{}: the oracle records no contact", s.name));
        return;
    };
    let rust_first = ours.contacts.iter().map(|c| c.tick).min();
    let mut pre = None;
    for (name, a) in &ours.states {
        let Some(b) = theirs.states.get(name) else {
            continue;
        };
        let n = a.len().min(b.len()).min(first as usize);
        if let Some(t) = (0..n).find(|&t| !same_state(&a[t], &b[t])) {
            pre = Some(pre.map_or(t, |p: usize| p.min(t)));
            gross.push(format!(
                "{}: {name} differs from the oracle at tick {t}, before the first contact (tick {first})",
                s.name
            ));
        }
        if let (Some(x), Some(y)) = (a.last(), b.last())
            && x.alive != y.alive
        {
            gross.push(format!(
                "{}: {name} outcome: alive {} vs the oracle's {}",
                s.name, x.alive, y.alive
            ));
        }
    }
    if rust_first != Some(first) {
        gross.push(format!(
            "{}: first contact at tick {} vs the oracle's {first}",
            s.name,
            rust_first.map_or_else(|| "none".to_owned(), |t| t.to_string())
        ));
    }
    let _ = writeln!(
        summary,
        "{}: contact scenario: first contact rust tick {}, oracle tick {first}; before it {}",
        s.name,
        rust_first.map_or_else(|| "none".to_owned(), |t| t.to_string()),
        pre.map_or_else(
            || "every object equals the oracle exactly".to_owned(),
            |t| format!("the first difference is at tick {t}")
        )
    );
    for c in theirs.contacts.iter().filter(|c| c.tick == first) {
        let rust = ours
            .contacts
            .iter()
            .find(|r| r.tick == first && r.a == c.a)
            .map_or_else(|| "none".to_owned(), fmt_contact);
        let _ = writeln!(
            summary,
            "{}: first-contact damage to {}: rust {rust} | oracle {}",
            s.name,
            c.b,
            fmt_contact(c)
        );
    }
}

/// A run's first contact or munition impact: its first collision event,
/// munition hit or spent guided munition (the oracle's `guided` records
/// when `dir` is given, else the run's own).
#[must_use]
pub fn first_event(dir: &Path, scenario: &str, run: &Run) -> Option<u32> {
    let guided = oracle_guided(dir, scenario);
    first_event_of(run, &guided)
}

/// A run's first contact or munition impact, its spent guided munitions
/// read from `guided` (the run's own, or the oracle's).
#[must_use]
pub fn first_event_of(run: &Run, guided: &BTreeMap<u32, Vec<GuidedState>>) -> Option<u32> {
    let contact = run.contacts.iter().map(|c| c.tick).min();
    let hit = run.hits.iter().map(|h| h.tick).min();
    let spent = guided
        .values()
        .filter_map(|v| {
            v.iter()
                .find(|s| !s.alive || s.vel == Vector::ZERO)
                .map(|s| s.tick)
        })
        .min();
    [contact, hit, spent].into_iter().flatten().min()
}

/// The escort scenarios of P2.5, none with a contact or a munition impact
/// (formation flight, a follow directive, a guide directive, a cruiser's
/// goto and large-ship manoeuvre), judged exactly as recorded.
pub const EXACT_SCENARIOS: [&str; 4] = ["formation", "follow", "guide", "large_ship"];

/// A scenario without contact judged exactly: every object equals the
/// oracle bit for bit at every recorded tick (position, velocity,
/// orientation, hit points, shield), alive or gone as in the oracle.
pub fn judge_exact(
    summary: &mut String,
    gross: &mut Vec<String>,
    s: &Scenario,
    ours: &Run,
    theirs: &Run,
) {
    let mut exact = true;
    for (name, a) in &ours.states {
        let Some(b) = theirs.states.get(name) else {
            continue;
        };
        if a.len() != b.len() {
            exact = false;
            gross.push(format!(
                "{}: {name} has {} ticks against the oracle's {}",
                s.name,
                a.len(),
                b.len()
            ));
        }
        let n = a.len().min(b.len());
        if let Some(t) = (0..n).find(|&t| !same_state(&a[t], &b[t])) {
            exact = false;
            gross.push(format!(
                "{}: {name} differs from the oracle at tick {t} (no contact: judged exactly)",
                s.name
            ));
        }
    }
    if exact {
        let _ = writeln!(
            summary,
            "{}: exact: every object equals the oracle bit for bit at every tick",
            s.name
        );
    }
}

/// A fight (owner, 2026-11-18; record 0003 point 9, as `ram_ship`): every
/// object must equal the oracle exactly before the oracle's first contact
/// or munition impact, and the Rust world's first contact, hit or spent
/// missile must come in the same tick (owner, 2026-11-19), or not at all
/// when the oracle has none. After it the outcome (alive states), the
/// collisions and the hits are reported, not judged: they fall under the
/// Gate G2 envelopes, the differences coming from the accepted contact
/// solver (D87, decision O19) and the fuse's timing (D177).
pub fn judge_fight(
    summary: &mut String,
    gross: &mut Vec<String>,
    dir: &Path,
    s: &Scenario,
    ours: &Run,
    theirs: &Run,
) {
    let rust_first = first_event_of(ours, &ours.guided);
    let Some(first) = first_event(dir, &s.name, theirs) else {
        // No contact or impact: the whole run is judged, outcome included.
        if let Some(t) = rust_first {
            gross.push(format!(
                "{}: the Rust world's first contact or impact comes at tick {t}; the oracle has none",
                s.name
            ));
        }
        for (name, a) in &ours.states {
            if let (Some(x), Some(y)) = (a.last(), theirs.states.get(name).and_then(|b| b.last()))
                && x.alive != y.alive
            {
                gross.push(format!(
                    "{}: {name} outcome: alive {} vs the oracle's {} (no contact)",
                    s.name, x.alive, y.alive
                ));
            }
        }
        let _ = writeln!(summary, "{}: fight without contact or impact", s.name);
        return;
    };
    let mut pre = None;
    for (name, a) in &ours.states {
        let Some(b) = theirs.states.get(name) else {
            continue;
        };
        let n = a.len().min(b.len()).min(first as usize);
        if let Some(t) = (0..n).find(|&t| !same_state(&a[t], &b[t])) {
            pre = Some(pre.map_or(t, |p: usize| p.min(t)));
            gross.push(format!(
                "{}: {name} differs from the oracle at tick {t}, before the first contact or impact (tick {first})",
                s.name
            ));
        }
    }
    // The first contact or impact comes in the oracle's tick, neither
    // before nor after it (owner, 2026-11-19, as `ram_ship`'s contact).
    if rust_first != Some(first) {
        gross.push(format!(
            "{}: the Rust world's first contact or impact comes at tick {}, the oracle's at tick {first}",
            s.name,
            rust_first.map_or_else(|| "none".to_owned(), |t| t.to_string())
        ));
    }
    let _ = writeln!(
        summary,
        "{}: fight: first contact or impact rust tick {}, oracle tick {first}; before it {}",
        s.name,
        rust_first.map_or_else(|| "none".to_owned(), |t| t.to_string()),
        pre.map_or_else(
            || "every object equals the oracle exactly".to_owned(),
            |t| format!("the first difference is at tick {t}")
        )
    );
    for (name, a) in &ours.states {
        if let (Some(x), Some(y)) = (a.last(), theirs.states.get(name).and_then(|b| b.last())) {
            let _ = writeln!(
                summary,
                "{}: after the first contact or impact (reported): {name} alive {} hull {} | oracle alive {} hull {}",
                s.name, x.alive, x.hp, y.alive, y.hp
            );
        }
    }
    let _ = writeln!(
        summary,
        "{}: after the first contact or impact (reported): collision events rust {} oracle {}; munition hits rust {} oracle {}",
        s.name,
        ours.contacts.len(),
        theirs.contacts.len(),
        ours.hits.len(),
        theirs.hits.len()
    );
}

/// The guided munitions of both sides, numbered by id: the launch tick,
/// the last tick alive and the speed then, the first bit difference of the
/// trajectory and of the core. Different launch or end ticks, or a missile
/// on one side only, are gross divergence.
pub fn report_guided(
    summary: &mut String,
    gross: &mut Vec<String>,
    dir: &Path,
    scenario: &str,
    ours: &Run,
    cutoff: u32,
) {
    let theirs = oracle_guided(dir, scenario);
    if theirs.is_empty() && ours.guided.is_empty() {
        return;
    }
    let their_cores = oracle_guided_cores(dir, scenario);
    let rust: Vec<(&u32, &Vec<GuidedState>)> = ours.guided.iter().collect();
    let oracle: Vec<(&u32, &Vec<GuidedState>)> = theirs.iter().collect();
    // The launch tick, the tick the munition is spent (the first tick it
    // is gone, or reads a velocity of exactly zero: the reference's object
    // destroyed in the collision pass stays in the object database until
    // the next update, its body gone), and the last flying state's speed
    // and target.
    let span = |v: &[GuidedState]| {
        let first = v.first().map_or(0, |s| s.tick);
        let spent = v
            .iter()
            .find(|s| !s.alive || s.vel == Vector::ZERO)
            .map_or(u32::MAX, |s| s.tick);
        let last = v.iter().rev().find(|s| s.alive && s.tick < spent);
        (
            first,
            spent,
            last.map_or(0.0, |s| dist(s.vel, Vector::ZERO)),
            last.map_or_else(String::new, |s| s.target.clone()),
        )
    };
    let _ = writeln!(
        summary,
        "{scenario}: guided munitions: rust {}, oracle {}",
        rust.len(),
        oracle.len()
    );
    for note in &ours.launch_notes {
        let _ = writeln!(summary, "    rust launch {note}");
    }
    let hex3 = |v: Vector| {
        format!(
            "[{:08x} {:08x} {:08x}]",
            v.x.to_bits(),
            v.y.to_bits(),
            v.z.to_bits()
        )
    };
    for (tick, t) in &ours.guided_trace {
        let _ = writeln!(
            summary,
            "    rust seek tick {tick} #{}: target pos {} vel {} own pos {} vel {} sees {} turn {} align {}",
            t.id,
            hex3(t.target_pos),
            hex3(t.target_vel),
            hex3(t.pos),
            hex3(t.vel),
            t.sees,
            t.turn.map_or_else(|| "-".to_owned(), hex3),
            t.align.map_or_else(|| "-".to_owned(), hex3)
        );
    }
    // The oracle's first PSIs of each missile: what its update added.
    if let Some(file) = find_files(&dir.join(scenario), "psi.jsonl")
        .into_iter()
        .min()
    {
        let mut seen: BTreeMap<u32, usize> = BTreeMap::new();
        for r in read_lines(&file) {
            if r.get("obj").and_then(Json::as_str) != Some("guided") {
                continue;
            }
            let id = r.get("id").and_then(Json::as_u32).unwrap_or(0);
            let n = seen.entry(id).or_default();
            *n += 1;
            if *n > 3 {
                continue;
            }
            let h = |c: Option<&Json>, k: &str| {
                c.and_then(|c| c.get(k))
                    .and_then(Json::as_floats)
                    .map(|v| {
                        v.iter()
                            .map(|x| format!("{:08x}", x.to_bits()))
                            .collect::<Vec<_>>()
                            .join(" ")
                    })
                    .unwrap_or_default()
            };
            let i = r.get("in");
            let _ = writeln!(
                summary,
                "    oracle psi tick {} #{id}: in speed [{}] speed_change [{}] rot_speed [{}] rot_speed_change [{}] pos [{}] q [{}] ulr {:?}; pushes {}",
                r.get("tick").and_then(Json::as_u32).unwrap_or(0),
                h(i, "speed"),
                h(i, "speed_change"),
                h(i, "rot_speed"),
                h(i, "rot_speed_change"),
                h(i, "pos"),
                h(i, "q"),
                i.and_then(|c| c.get("upper_limit_radius"))
                    .and_then(Json::as_f32),
                r.get("pushes")
                    .and_then(Json::as_array)
                    .map_or(0, |a| a.len())
            );
        }
    }
    // The oracle's shooters' frames in the launch ticks (`tick.jsonl`).
    if let Some(file) = find_files(&dir.join(scenario), "tick.jsonl")
        .into_iter()
        .next()
    {
        let launches: Vec<u32> = oracle
            .iter()
            .filter_map(|(_, v)| v.first().map(|s| s.tick))
            .collect();
        for r in read_lines(&file) {
            let tick = r.get("tick").and_then(Json::as_u32).unwrap_or(0);
            if r.get("rec").and_then(Json::as_str) != Some("object") || !launches.contains(&tick) {
                continue;
            }
            let hex = |k: &str| {
                r.get(k)
                    .and_then(Json::as_floats)
                    .map(|v| {
                        v.iter()
                            .map(|x| format!("{:08x}", x.to_bits()))
                            .collect::<Vec<_>>()
                            .join(" ")
                    })
                    .unwrap_or_default()
            };
            let _ = writeln!(
                summary,
                "    oracle tick {tick} {}: orient [{}] pos [{}]",
                r.get("obj").and_then(Json::as_str).unwrap_or("?"),
                hex("orient"),
                hex("pos")
            );
        }
    }
    for (id, v) in &oracle {
        if let Some(first) = v.first() {
            let _ = writeln!(
                summary,
                "    oracle guided #{id} first record tick {}: orient [{}] vel [{:08x} {:08x} {:08x}]",
                first.tick,
                first
                    .orient
                    .iter()
                    .map(|x| format!("{:08x}", x.to_bits()))
                    .collect::<Vec<_>>()
                    .join(" "),
                first.vel.x.to_bits(),
                first.vel.y.to_bits(),
                first.vel.z.to_bits()
            );
        }
    }
    for (id, v) in &rust {
        if let Some(first) = v.first() {
            let _ = writeln!(
                summary,
                "    rust guided #{id} first record tick {}: orient [{}] vel [{:08x} {:08x} {:08x}]",
                first.tick,
                first
                    .orient
                    .iter()
                    .map(|x| format!("{:08x}", x.to_bits()))
                    .collect::<Vec<_>>()
                    .join(" "),
                first.vel.x.to_bits(),
                first.vel.y.to_bits(),
                first.vel.z.to_bits()
            );
        }
    }
    // Only what happens before a fight's first contact or impact is judged
    // (`cutoff`, owner 2026-11-18): the munitions launched before it.
    let launched_before = |v: &[(&u32, &Vec<GuidedState>)]| {
        v.iter()
            .filter(|(_, s)| s.first().is_some_and(|f| f.tick < cutoff))
            .count()
    };
    if launched_before(&rust) != launched_before(&oracle) && !theirs.is_empty() {
        gross.push(format!(
            "{scenario}: guided munitions launched before tick {cutoff}: rust {} vs oracle {}",
            launched_before(&rust),
            launched_before(&oracle)
        ));
    }
    for k in 0..rust.len().max(oracle.len()) {
        let a = rust.get(k);
        let b = oracle.get(k);
        let fmt = |x: Option<&(&u32, &Vec<GuidedState>)>| {
            x.map_or_else(
                || "none".to_owned(),
                |(id, v)| {
                    let (first, last, speed, target) = span(v);
                    format!(
                        "#{id} launched at tick {first}, spent at tick {last}, last speed {speed:.3} m/s, target {target}"
                    )
                },
            )
        };
        let _ = writeln!(
            summary,
            "{scenario}: guided{}: rust {} | oracle {}",
            k + 1,
            fmt(a),
            fmt(b)
        );
        let (Some((rid, av)), Some((oid, bv))) = (a, b) else {
            continue;
        };
        let (sa, sb) = (span(av), span(bv));
        let before = sa.0.min(sb.0) < cutoff && sa.0 != sb.0 || sa.1.min(sb.1) < cutoff;
        if (sa.0, sa.1) != (sb.0, sb.1) && before {
            gross.push(format!(
                "{scenario}: guided{} launched at tick {} and spent at tick {}; the oracle's at {} and {}",
                k + 1,
                sa.0,
                sa.1,
                sb.0,
                sb.1
            ));
        }
        // The trajectory, bit for bit, tick by tick.
        let bits = |v: Vector| [v.x.to_bits(), v.y.to_bits(), v.z.to_bits()];
        let differs = |x: &GuidedState, y: &GuidedState| {
            x.alive != y.alive
                || (x.alive
                    && (bits(x.pos) != bits(y.pos)
                        || bits(x.vel) != bits(y.vel)
                        || bits(x.angvel) != bits(y.angvel)
                        || x.orient.map(f32::to_bits) != y.orient.map(f32::to_bits)
                        || x.lifetime.to_bits() != y.lifetime.to_bits()
                        || x.seeker_elapsed.to_bits() != y.seeker_elapsed.to_bits()))
        };
        let pairs: Vec<(&GuidedState, &GuidedState)> = av
            .iter()
            .filter_map(|x| bv.iter().find(|y| y.tick == x.tick).map(|y| (x, y)))
            .collect();
        let line = |st: &GuidedState| {
            let hex = |v: Vector| {
                format!(
                    "[{:08x} {:08x} {:08x}]",
                    v.x.to_bits(),
                    v.y.to_bits(),
                    v.z.to_bits()
                )
            };
            format!(
                "alive {} pos ({:.4}, {:.4}, {:.4}) {} vel {} speed {:.4} angvel {} fwd ({:.4}, {:.4}, {:.4}) lifetime {} elapsed {} target {}",
                st.alive,
                st.pos.x,
                st.pos.y,
                st.pos.z,
                hex(st.pos),
                hex(st.vel),
                dist(st.vel, Vector::ZERO),
                hex(st.angvel),
                -st.orient[2],
                -st.orient[5],
                -st.orient[8],
                st.lifetime,
                st.seeker_elapsed,
                st.target
            )
        };
        match pairs.iter().position(|(x, y)| differs(x, y)) {
            None => {
                let _ = writeln!(
                    summary,
                    "{scenario}: guided{}: every common tick ({}) equals the oracle bit for bit",
                    k + 1,
                    pairs.len()
                );
            }
            Some(i) => {
                let _ = writeln!(
                    summary,
                    "{scenario}: guided{}: first bit difference at tick {} ({} of {} common ticks differ):",
                    k + 1,
                    pairs[i].0.tick,
                    pairs.iter().filter(|(x, y)| differs(x, y)).count(),
                    pairs.len()
                );
                for (x, y) in pairs.iter().skip(i.saturating_sub(1)).take(4) {
                    let _ = writeln!(summary, "    tick {} rust:   {}", x.tick, line(x));
                    let _ = writeln!(summary, "    tick {} oracle: {}", y.tick, line(y));
                }
            }
        }
        // The core after each PSI against the oracle's `out`.
        let (Some(rc), Some(oc)) = (ours.guided_cores.get(rid), their_cores.get(oid)) else {
            continue;
        };
        let mut first = None;
        for (tick, expected) in oc {
            let Some((_, actual)) = rc.iter().find(|(t, _)| t == tick) else {
                continue;
            };
            let actual = CoreRecord {
                core: actual.core.clone(),
                unit_state: expected.unit_state,
            };
            let diffs = compare(expected, &actual, 0.0);
            if !diffs.is_empty() {
                first = Some((*tick, diffs));
                break;
            }
        }
        match first {
            None => {
                let _ = writeln!(
                    summary,
                    "{scenario}: guided{}: every core of the oracle's {} PSIs equals the Rust core bit for bit",
                    k + 1,
                    oc.len()
                );
            }
            Some((tick, diffs)) => {
                let _ = writeln!(
                    summary,
                    "{scenario}: guided{}: core first differs after tick {tick}: {}",
                    k + 1,
                    diffs.join("; ")
                );
            }
        }
    }
}

/// One object's measured differences from the oracle over the run (the
/// largest position error and its tick, the largest velocity and
/// orientation errors, the ticks alive on one side only, the final hit
/// points), written to the summary. Outside a contact scenario and a
/// `fight` (the ships' attitudes come from the oracle), a position error
/// beyond [`GROSS_POSITION`] (or half the distance the object travelled
/// in the oracle, if more) or a different alive state at the last tick is
/// gross.
pub fn report_object_errors(
    summary: &mut String,
    gross: &mut Vec<String>,
    s: &Scenario,
    name: &str,
    ours_states: &[State],
    their_states: &[State],
    fight: bool,
) {
    let n = ours_states.len().min(their_states.len());
    let (mut max_pos, mut max_vel, mut max_orient, mut alive_mismatch) =
        (0.0f64, 0.0f64, 0.0f32, 0usize);
    let mut max_tick = 0;
    for t in 0..n {
        let (a, b) = (&ours_states[t], &their_states[t]);
        if a.alive != b.alive {
            alive_mismatch += 1;
            continue;
        }
        if !a.alive {
            continue;
        }
        let p = dist(a.pos, b.pos);
        if p > max_pos {
            max_pos = p;
            max_tick = t;
        }
        max_vel = max_vel.max(dist(a.vel, b.vel));
        for i in 0..9 {
            max_orient = max_orient.max((a.orient[i] - b.orient[i]).abs());
        }
    }
    let travelled = dist(their_states[0].pos, their_states[n - 1].pos);
    let bound = GROSS_POSITION.max(0.5 * travelled);
    let (fa, fb) = (&ours_states[n - 1], &their_states[n - 1]);
    let _ = writeln!(
        summary,
        "{}: {name}: max position error {max_pos:.4} m (tick {max_tick}), velocity {max_vel:.4} m/s, orientation {max_orient:.6}, alive mismatches {alive_mismatch}, final hp {} vs {}",
        s.name, fa.hp, fb.hp
    );
    // A fight (the ships' attitudes come from the oracle) is judged
    // exactly to its first contact or impact and its outcome is
    // reported after it (`judge_fight`; owner, 2026-11-18). The
    // other scenarios without contact keep their strict outcome.
    if !s.contact && !fight && (max_pos > bound || fa.alive != fb.alive) {
        gross.push(format!(
            "{}: {name} (position {max_pos:.1} m > {bound:.1} m or alive {} vs {})",
            s.name, fa.alive, fb.alive
        ));
    }
}

/// Every judgement `scenario_replay` makes of a run against the oracle's
/// run `theirs` (from `dir`), without its diagnostic reports: each
/// object's errors ([`report_object_errors`]), the guided munitions
/// ([`report_guided`], a fight's up to its first contact or impact),
/// [`judge_exact`] for the [`EXACT_SCENARIOS`], then [`judge_contact`]
/// for a contact scenario, [`judge_fight`] for a fight (the ships'
/// attitudes in `ai`), and for the others collision events on one side
/// only. A run whose AI ship enters an unported behaviour (`Run::unported`)
/// is not judged here: `scenario_replay` skips it after
/// [`report_guided`] to the end of the run.
pub fn judge(
    summary: &mut String,
    gross: &mut Vec<String>,
    dir: &Path,
    s: &Scenario,
    ai: &AiInputs,
    ours: &Run,
    theirs: &Run,
) {
    let fight = !ai.attitudes.is_empty();
    for (name, ours_states) in &ours.states {
        let Some(their_states) = theirs.states.get(name) else {
            continue;
        };
        report_object_errors(summary, gross, s, name, ours_states, their_states, fight);
    }
    let cutoff = if fight {
        first_event(dir, &s.name, theirs).unwrap_or(u32::MAX)
    } else {
        u32::MAX
    };
    report_guided(summary, gross, dir, &s.name, ours, cutoff);
    if EXACT_SCENARIOS.contains(&s.name.as_str()) {
        judge_exact(summary, gross, s, ours, theirs);
    }
    if s.contact {
        judge_contact(summary, gross, s, ours, theirs);
    } else if fight {
        judge_fight(summary, gross, dir, s, ours, theirs);
    } else if ours.contacts.is_empty() != theirs.contacts.is_empty() {
        gross.push(format!(
            "{}: collision events rust {} vs oracle {}",
            s.name,
            ours.contacts.len(),
            theirs.contacts.len()
        ));
    }
}
