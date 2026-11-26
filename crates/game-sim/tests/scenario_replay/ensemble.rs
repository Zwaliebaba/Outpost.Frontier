//! Gate G2's seed ensemble (record 0003 point 9, revision of 2026-11-19;
//! owner, 2026-11-19). The oracle is deterministic, so its run-to-run
//! spread is 0 and says nothing about the outcome of a fight after its
//! first contact. Instead each scenario of `tests/fixtures/g2_ensemble.txt`
//! runs once per listed seed on both sides: the seed replaces the one of
//! the `srand` the harness calls again once the system has loaded
//! (`Oracle.exe sim ... --seed S`, the Rust world's seeding point in
//! [`run`]); the load-time draws keep the scenario's seed.
//!
//! Every seed's run is judged as the single run is: bit for bit up to its
//! first contact or munition impact ([`judge_contact`] for `ram_ship`,
//! with the owner's 5 % bound on the first contact's damage, approved
//! 2026-11-19; [`judge_fight`] for the fights; for the others every
//! object exactly up to the first impact, the impact in the same tick, and
//! the strict outcome). After the first contact the outcome statistics of
//! the seeds are compared, Rust's distribution against the oracle's, per
//! object: the survival count, the kill tick (the first tick at hull 0),
//! the munition hits taken and the collision events up to it, and the
//! final hull and shield (see [`Bounds`]).
//!
//! `OUTPOST_ENSEMBLE_DIR` names the ensemble's folder: the oracle's runs
//! in `<dir>/seed_<S>/<scenario>/` (data.yml, "Run the seed ensemble").
//! Unset, the test returns with a notice. A seed folder without an oracle
//! run gives the Rust side only; the fights then take the pilots'
//! personalities and attitudes from `OUTPOST_ORACLE_DIR`'s runs if set.
//! The summary goes to `<dir>/rust-ensemble.txt`.

use std::sync::Mutex;
use std::sync::atomic::{AtomicUsize, Ordering};

use super::*;

/// The owner's bound on the first contact's damage, relative (approved
/// 2026-11-19): `ram_ship` today is Rust 31.77 against the oracle's 30.88
/// (+2.9 %; impact speed 63.55 against 61.76 m/s, from the contact solver,
/// D87).
const RAM_DAMAGE_BOUND: f64 = 0.05;

/// Whether the owner has approved the outcome bounds below (approved
/// 2026-11-19); a metric outside them fails the test.
const ENVELOPES_APPROVED: bool = true;

/// The outcome bounds, approved by the owner on 2026-11-19 (record 0003
/// point 10): for each metric Rust's values over the seeds against the oracle's. They
/// apply to the fights; `ram_ship` after its first contact is judged by
/// its outcome (alive) only, and its other statistics are reported.
struct Bounds {
    /// The significance level of the two-sample tests (Fisher's exact test
    /// of the survival counts, the exact two-sample Kolmogorov–Smirnov test
    /// of the other metrics): a metric fails when its p-value is below it.
    alpha: f64,
    /// The median of a metric: |rust − oracle| at most
    /// `max(floor, iqr × the oracle's interquartile range)` (the owner,
    /// 2026-11-19). With 60 seeds the standard error of the difference of
    /// two medians is about 0.17 × IQR (normal approximation), so 0.44 is
    /// about 2.5 standard errors, the level of `alpha`. (The 20-seed
    /// proposal used 0.75, the same level at 20 seeds.)
    iqr: f64,
}

const BOUNDS: Bounds = Bounds {
    alpha: 0.01,
    iqr: 0.44,
};

/// The floors of the median bound, per metric: one second of kill time,
/// two munition hits, one contact (its two collision events), 50 hull and
/// 50 shield points.
const MEDIAN_FLOORS: [(&str, f64); 5] = [
    ("kill_tick", 30.0),
    ("hits", 2.0),
    ("collisions", 2.0),
    ("hull", 50.0),
    ("shield", 50.0),
];

/// The scenarios and seeds of the ensemble.
struct Ensemble {
    scenarios: Vec<String>,
    seeds: Vec<u32>,
}

fn read_ensemble() -> Ensemble {
    let path = Path::new(env!("CARGO_MANIFEST_DIR")).join("tests/fixtures/g2_ensemble.txt");
    let text = fs::read_to_string(&path).unwrap_or_else(|e| panic!("{}: {e}", path.display()));
    let mut out = Ensemble {
        scenarios: Vec::new(),
        seeds: Vec::new(),
    };
    for line in text.lines() {
        let line = line.split('#').next().unwrap_or("").trim();
        let mut t = line.split_whitespace();
        match (t.next(), t.next()) {
            (Some("scenario"), Some(name)) => out.scenarios.push(name.to_owned()),
            (Some("seed"), Some(seed)) => out
                .seeds
                .push(seed.parse().unwrap_or_else(|e| panic!("seed {seed}: {e}"))),
            _ => {}
        }
    }
    out
}

/// One object's outcome in one run.
#[derive(Clone, Debug, Default)]
struct ObjOutcome {
    /// Never killed: its hull stays above 0 to the last tick.
    survived: bool,
    /// The kill (owner, 2026-11-19): the first tick its hull is 0 or less
    /// (or it is gone), not the tick it is removed. The server's death fuse
    /// keeps a ship at hull 0 in space for 75 more ticks in the C++
    /// (the bare world without the server destroys it at once), where it
    /// can still be hit.
    kill_tick: Option<u32>,
    /// Munition hits it took up to its kill (all of them without one).
    hits: usize,
    /// Collision events naming it up to its kill.
    collisions: usize,
    /// Its hull at the last tick (0 when gone).
    hull: f32,
    /// Its first shield's hit points at the last tick (0 when gone),
    /// `None` for an object without a shield.
    shield: Option<f32>,
}

/// What one run gives the statistics.
#[derive(Clone, Debug, Default)]
struct Outcome {
    first_event: Option<u32>,
    objects: BTreeMap<String, ObjOutcome>,
}

fn outcome(run: &Run, first_event: Option<u32>) -> Outcome {
    let mut objects = BTreeMap::new();
    for (name, states) in &run.states {
        let kill_tick = states
            .iter()
            .position(|st| !st.alive || st.hp <= 0.0)
            .and_then(|t| u32::try_from(t).ok());
        let last = states.last().copied().unwrap_or_default();
        let has_shield = states.iter().any(|st| st.shield.is_some());
        let until = kill_tick.unwrap_or(u32::MAX);
        objects.insert(
            name.clone(),
            ObjOutcome {
                survived: kill_tick.is_none(),
                kill_tick,
                hits: run
                    .hits
                    .iter()
                    .filter(|h| &h.target == name && h.tick <= until)
                    .count(),
                collisions: run
                    .contacts
                    .iter()
                    .filter(|c| (&c.a == name || &c.b == name) && c.tick <= until)
                    .count(),
                hull: if last.alive { last.hp } else { 0.0 },
                shield: has_shield.then(|| {
                    if last.alive {
                        last.shield.map_or(0.0, |s| s.hp)
                    } else {
                        0.0
                    }
                }),
            },
        );
    }
    Outcome {
        first_event,
        objects,
    }
}

fn fmt_outcome(o: &ObjOutcome) -> String {
    format!(
        "{} kill {} hits {} coll {} hull {} shield {}",
        if o.survived { "alive" } else { "dead" },
        o.kill_tick
            .map_or_else(|| "-".to_owned(), |t| t.to_string()),
        o.hits,
        o.collisions,
        o.hull,
        o.shield.map_or_else(|| "-".to_owned(), |s| s.to_string())
    )
}

/// One seed's result.
#[derive(Default)]
struct SeedResult {
    seed: u32,
    lines: String,
    gross: Vec<String>,
    rust: Option<Outcome>,
    oracle: Option<Outcome>,
    /// The largest relative error of the first contact's damage.
    ram_error: Option<f64>,
    /// Each side's run as one hash per tick ([`trace`]), to see where the
    /// seed changes it.
    rust_trace: Vec<u64>,
    oracle_trace: Vec<u64>,
}

/// One hash per tick of every scenario object's state (alive, position,
/// velocity, orientation, hull and shield bits).
fn trace(run: &Run) -> Vec<u64> {
    use std::hash::{DefaultHasher, Hash, Hasher};
    let ticks = run.states.values().map(Vec::len).max().unwrap_or(0);
    (0..ticks)
        .map(|t| {
            let mut h = DefaultHasher::new();
            for (name, states) in &run.states {
                name.hash(&mut h);
                if let Some(st) = states.get(t) {
                    st.alive.hash(&mut h);
                    for v in [st.pos, st.vel] {
                        [v.x, v.y, v.z].map(f32::to_bits).hash(&mut h);
                    }
                    st.orient.map(f32::to_bits).hash(&mut h);
                    st.hp.to_bits().hash(&mut h);
                    st.shield.map(|s| s.hp.to_bits()).hash(&mut h);
                }
            }
            h.finish()
        })
        .collect()
}

/// The first tick at which two traces differ, `None` when they are equal.
fn trace_difference(a: &[u64], b: &[u64]) -> Option<usize> {
    (0..a.len().max(b.len())).find(|&t| a.get(t) != b.get(t))
}

/// A scenario without contact and without a fight (`fly_to_point`,
/// `fire_at_target`): every object equals the oracle exactly up to the
/// first munition impact (the whole run without one), the Rust world's
/// first impact comes in the same tick, and the strict outcome holds
/// (position within the gross bound, alive or gone as in the oracle,
/// collision events on both sides or neither).
fn judge_strict(
    summary: &mut String,
    gross: &mut Vec<String>,
    dir: &Path,
    s: &Scenario,
    ours: &Run,
    theirs: &Run,
) {
    let first = first_event(dir, &s.name, theirs);
    let rust_first = first_event_of(ours, &ours.guided);
    let cut = first.map_or(usize::MAX, |t| t as usize);
    let mut pre = None;
    for (name, a) in &ours.states {
        let Some(b) = theirs.states.get(name) else {
            continue;
        };
        let n = a.len().min(b.len()).min(cut);
        if let Some(t) = (0..n).find(|&t| !same_state(&a[t], &b[t])) {
            pre = Some(pre.map_or(t, |p: usize| p.min(t)));
            gross.push(format!(
                "{}: {name} differs from the oracle at tick {t}, before the first impact ({})",
                s.name,
                first.map_or_else(|| "none".to_owned(), |t| t.to_string())
            ));
        }
        let n = a.len().min(b.len());
        if n == 0 {
            continue;
        }
        let max_pos = (0..n)
            .filter(|&t| a[t].alive && b[t].alive)
            .map(|t| dist(a[t].pos, b[t].pos))
            .fold(0.0, f64::max);
        let bound = GROSS_POSITION.max(0.5 * dist(b[0].pos, b[n - 1].pos));
        if max_pos > bound || a[n - 1].alive != b[n - 1].alive {
            gross.push(format!(
                "{}: {name} (position {max_pos:.1} m > {bound:.1} m or alive {} vs {})",
                s.name,
                a[n - 1].alive,
                b[n - 1].alive
            ));
        }
    }
    if rust_first != first {
        gross.push(format!(
            "{}: first impact at tick {} vs the oracle's {}",
            s.name,
            rust_first.map_or_else(|| "none".to_owned(), |t| t.to_string()),
            first.map_or_else(|| "none".to_owned(), |t| t.to_string())
        ));
    }
    if ours.contacts.is_empty() != theirs.contacts.is_empty() {
        gross.push(format!(
            "{}: collision events rust {} vs oracle {}",
            s.name,
            ours.contacts.len(),
            theirs.contacts.len()
        ));
    }
    let _ = writeln!(
        summary,
        "{}: first impact rust tick {}, oracle tick {}; before it {}",
        s.name,
        rust_first.map_or_else(|| "none".to_owned(), |t| t.to_string()),
        first.map_or_else(|| "none".to_owned(), |t| t.to_string()),
        pre.map_or_else(
            || "every object equals the oracle exactly".to_owned(),
            |t| format!("the first difference is at tick {t}")
        )
    );
}

/// The first contact's damage against the owner's 5 % bound: each oracle
/// collision event of its first contact tick against the Rust one of the
/// same tick and reporting object. Returns the largest relative error.
fn judge_ram_damage(
    gross: &mut Vec<String>,
    s: &Scenario,
    ours: &Run,
    theirs: &Run,
) -> Option<f64> {
    let first = theirs.contacts.iter().map(|c| c.tick).min()?;
    let mut worst: Option<f64> = None;
    for c in theirs.contacts.iter().filter(|c| c.tick == first) {
        let Some(r) = ours.contacts.iter().find(|r| r.tick == first && r.a == c.a) else {
            gross.push(format!(
                "{}: no Rust collision event of {} in the first contact's tick {first}",
                s.name, c.a
            ));
            continue;
        };
        let rel = (f64::from(r.damage) - f64::from(c.damage)).abs() / f64::from(c.damage).abs();
        worst = Some(worst.map_or(rel, |w| w.max(rel)));
        if rel.is_nan() || rel > RAM_DAMAGE_BOUND {
            gross.push(format!(
                "{}: first-contact damage to {}: rust {} vs oracle {} ({:+.2} %, bound {} %)",
                s.name,
                c.b,
                r.damage,
                c.damage,
                100.0 * (f64::from(r.damage) - f64::from(c.damage)) / f64::from(c.damage),
                100.0 * RAM_DAMAGE_BOUND
            ));
        }
    }
    worst
}

/// One seed of one scenario: the Rust run, the oracle's when it is there,
/// the judgement and the outcomes.
fn run_seed(base: &Scenario, seed: u32, dir: &Path, fallback_ai: Option<&Path>) -> SeedResult {
    let mut s = base.clone();
    s.reseed = Some(seed);
    let mut out = SeedResult {
        seed,
        ..SeedResult::default()
    };
    let seed_dir = dir.join(format!("seed_{seed}"));
    let mut ai = oracle_ai_inputs(&seed_dir, &s.name);
    if ai.personalities.is_empty()
        && ai.attitudes.is_empty()
        && let Some(main) = fallback_ai
    {
        ai = oracle_ai_inputs(main, &s.name);
    }
    let fight = !ai.attitudes.is_empty();
    let Some(ours) = run(&s, &ai) else {
        out.gross.push(format!(
            "{} seed {seed}: the Rust world did not run",
            s.name
        ));
        return out;
    };
    let theirs = oracle_run(&seed_dir, &s.name);
    let oracle_first = theirs
        .as_ref()
        .and_then(|t| first_event(&seed_dir, &s.name, t));
    out.rust = Some(outcome(&ours, first_event_of(&ours, &ours.guided)));
    out.rust_trace = trace(&ours);
    let Some(theirs) = theirs else {
        return out;
    };
    let mut gross = Vec::new();
    let mut lines = String::new();
    if s.contact {
        judge_contact(&mut lines, &mut gross, &s, &ours, &theirs);
        out.ram_error = judge_ram_damage(&mut gross, &s, &ours, &theirs);
    } else if fight {
        judge_fight(&mut lines, &mut gross, &seed_dir, &s, &ours, &theirs);
    } else {
        judge_strict(&mut lines, &mut gross, &seed_dir, &s, &ours, &theirs);
    }
    // A difference before the first contact: where, for the record.
    if !gross.is_empty() {
        for (name, a) in &ours.states {
            if let Some(b) = theirs.states.get(name) {
                first_divergences(&mut lines, &s.name, name, a, b);
            }
        }
        // The first hits on each side and the Rust missiles before them.
        let rust_first = first_event_of(&ours, &ours.guided);
        let until = oracle_first.max(rust_first).unwrap_or(0) + 5;
        for (side, hits) in [("rust", &ours.hits), ("oracle", &theirs.hits)] {
            for h in hits.iter().filter(|h| h.tick <= until).take(6) {
                let _ = writeln!(
                    lines,
                    "{}: {side} hit at tick {} on {}: {}",
                    s.name,
                    h.tick,
                    h.target,
                    fmt_hit(h)
                );
            }
        }
        for (tick, shooter, gun) in ours.missiles.iter().filter(|m| m.0 <= until) {
            let _ = writeln!(
                lines,
                "{}: rust missile launched at tick {tick} by {shooter} (gun {gun})",
                s.name
            );
        }
    }
    out.oracle = Some(outcome(&theirs, oracle_first));
    out.oracle_trace = trace(&theirs);
    out.gross = gross
        .into_iter()
        .map(|g| format!("seed {seed}: {g}"))
        .collect();
    for l in lines.lines() {
        let _ = writeln!(out.lines, "  seed {seed}: {l}");
    }
    out
}

/// The `q` quantile of sorted values (linear between order statistics).
fn quantile(sorted: &[f64], q: f64) -> f64 {
    if sorted.is_empty() {
        return f64::NAN;
    }
    let h = (sorted.len() - 1) as f64 * q;
    let (lo, hi) = (h.floor() as usize, h.ceil() as usize);
    sorted[lo] + (h - lo as f64) * (sorted[hi] - sorted[lo])
}

/// The two-sample Kolmogorov–Smirnov statistic: the largest difference
/// between the two empirical distribution functions, ties included.
fn ks_statistic(a: &[f64], b: &[f64]) -> f64 {
    let cdf = |v: &[f64], x: f64| v.iter().filter(|&&y| y <= x).count() as f64 / v.len() as f64;
    a.iter()
        .chain(b)
        .map(|&x| (cdf(a, x) - cdf(b, x)).abs())
        .fold(0.0, f64::max)
}

/// The exact p-value of the two-sample Kolmogorov–Smirnov test for sample
/// sizes `n` and `m` and statistic `d`: the share of the C(n+m, n) orders
/// of the pooled values whose statistic reaches `d` (the lattice-path
/// count). Without ties it is exact; ties make it conservative.
fn ks_p_value(n: usize, m: usize, d: f64) -> f64 {
    if n == 0 || m == 0 || d <= 0.0 {
        return 1.0;
    }
    let (nf, mf) = (n as f64, m as f64);
    // Paths that stay strictly inside the band |i/n − j/m| < d.
    let inside = |i: usize, j: usize| (i as f64 / nf - j as f64 / mf).abs() < d - 1e-9;
    let mut row = vec![0.0f64; m + 1];
    for i in 0..=n {
        for j in 0..=m {
            row[j] = if !inside(i, j) {
                0.0
            } else if i == 0 && j == 0 {
                1.0
            } else {
                let up = if i > 0 { row[j] } else { 0.0 };
                let left = if j > 0 { row[j - 1] } else { 0.0 };
                up + left
            };
        }
    }
    let mut total = 1.0f64;
    for k in 0..n {
        total = total * (m + n - k) as f64 / (k + 1) as f64;
    }
    (1.0 - row[m] / total).clamp(0.0, 1.0)
}

/// Fisher's exact test, two-sided, of `a` of `n` against `b` of `m`.
fn fisher_p_value(a: usize, n: usize, b: usize, m: usize) -> f64 {
    let ln_choose = |n: usize, k: usize| -> f64 {
        (1..=k)
            .map(|i| ((n - k + i) as f64).ln() - (i as f64).ln())
            .sum()
    };
    let total = a + b;
    let p = |x: usize| -> f64 {
        if x > n || total - x > m {
            return 0.0;
        }
        (ln_choose(n, x) + ln_choose(m, total - x) - ln_choose(n + m, total)).exp()
    };
    let observed = p(a);
    (0..=total.min(n))
        .map(p)
        .filter(|&q| q <= observed * (1.0 + 1e-7))
        .sum::<f64>()
        .min(1.0)
}

/// A sample's summary: n, median, quartiles, range.
fn fmt_sample(v: &[f64]) -> String {
    if v.is_empty() {
        return "n 0".to_owned();
    }
    format!(
        "n {} median {:.4} q25 {:.4} q75 {:.4} min {:.4} max {:.4}",
        v.len(),
        quantile(v, 0.5),
        quantile(v, 0.25),
        quantile(v, 0.75),
        v[0],
        v[v.len() - 1]
    )
}

/// One metric of one object, Rust's values against the oracle's: the
/// summary line and whether it is outside the approved bounds.
fn compare_metric(metric: &str, mut rust: Vec<f64>, mut oracle: Vec<f64>) -> (String, bool) {
    rust.sort_by(f64::total_cmp);
    oracle.sort_by(f64::total_cmp);
    let floor = MEDIAN_FLOORS
        .iter()
        .find(|f| f.0 == metric)
        .map_or(0.0, |f| f.1);
    if rust.is_empty() || oracle.is_empty() {
        let outside = rust.len() != oracle.len() && metric != "kill_tick";
        return (
            format!(
                "{metric}: rust {} | oracle {}",
                fmt_sample(&rust),
                fmt_sample(&oracle)
            ),
            outside,
        );
    }
    let iqr = quantile(&oracle, 0.75) - quantile(&oracle, 0.25);
    let margin = floor.max(BOUNDS.iqr * iqr);
    let dm = quantile(&rust, 0.5) - quantile(&oracle, 0.5);
    let d = ks_statistic(&rust, &oracle);
    let p = ks_p_value(rust.len(), oracle.len(), d);
    let outside = dm.abs() > margin || p < BOUNDS.alpha;
    (
        format!(
            "{metric}: rust {} | oracle {} | median diff {dm:+.4} (margin {margin:.4}), KS D {d:.3} p {p:.4}{}",
            fmt_sample(&rust),
            fmt_sample(&oracle),
            if outside { "  OUTSIDE" } else { "" }
        ),
        outside,
    )
}

/// The ensemble statistics of one scenario. `outcome_only`: a contact
/// scenario (`ram_ship`), whose statistics after the first contact other
/// than survival are reported, not bounded (Gate G2: after its first
/// contact only the outcome is compared).
fn report_statistics(
    summary: &mut String,
    outside: &mut Vec<String>,
    name: &str,
    results: &[SeedResult],
    outcome_only: bool,
) {
    let rust: Vec<&Outcome> = results.iter().filter_map(|r| r.rust.as_ref()).collect();
    let oracle: Vec<&Outcome> = results.iter().filter_map(|r| r.oracle.as_ref()).collect();
    let firsts = |v: &[&Outcome]| -> Vec<f64> {
        v.iter()
            .filter_map(|o| o.first_event)
            .map(f64::from)
            .collect()
    };
    let (mut fr, mut fo) = (firsts(&rust), firsts(&oracle));
    fr.sort_by(f64::total_cmp);
    fo.sort_by(f64::total_cmp);
    let _ = writeln!(
        summary,
        "{name}: first contact or impact tick: rust {} | oracle {}",
        fmt_sample(&fr),
        fmt_sample(&fo)
    );
    // The seeds whose first contact or impact comes in the oracle's tick.
    let fmt_tick = |t: Option<u32>| t.map_or_else(|| "-".to_owned(), |t| t.to_string());
    let paired: Vec<(u32, Option<u32>, Option<u32>)> = results
        .iter()
        .filter_map(|r| {
            Some((
                r.seed,
                r.rust.as_ref()?.first_event,
                r.oracle.as_ref()?.first_event,
            ))
        })
        .collect();
    if !paired.is_empty() {
        let other: Vec<String> = paired
            .iter()
            .filter(|p| p.1 != p.2)
            .map(|p| {
                format!(
                    "seed {} rust {} oracle {}",
                    p.0,
                    fmt_tick(p.1),
                    fmt_tick(p.2)
                )
            })
            .collect();
        let _ = writeln!(
            summary,
            "{name}: first contact or impact in the oracle's tick in {} of {} seeds{}",
            paired.len() - other.len(),
            paired.len(),
            if other.is_empty() {
                String::new()
            } else {
                format!(" (not: {})", other.join(", "))
            }
        );
    }
    let objects: Vec<String> = rust
        .iter()
        .chain(&oracle)
        .flat_map(|o| o.objects.keys().cloned())
        .collect::<std::collections::BTreeSet<_>>()
        .into_iter()
        .collect();
    for obj in objects {
        let of = |v: &[&Outcome]| -> Vec<ObjOutcome> {
            v.iter()
                .filter_map(|o| o.objects.get(&obj).cloned())
                .collect()
        };
        let (r, o) = (of(&rust), of(&oracle));
        let (sr, so) = (
            r.iter().filter(|x| x.survived).count(),
            o.iter().filter(|x| x.survived).count(),
        );
        if !o.is_empty() {
            let p = fisher_p_value(sr, r.len(), so, o.len());
            let off = p < BOUNDS.alpha;
            let _ = writeln!(
                summary,
                "{name}: {obj}: survived: rust {sr} of {} | oracle {so} of {} | diff {:+}, Fisher p {p:.4} (bound {}){}",
                r.len(),
                o.len(),
                sr as i64 - so as i64,
                BOUNDS.alpha,
                if off { "  OUTSIDE" } else { "" }
            );
            if off {
                outside.push(format!("{name}: {obj}: survived"));
            }
        } else {
            let _ = writeln!(summary, "{name}: {obj}: survived: rust {sr} of {}", r.len());
        }
        type Pick = fn(&ObjOutcome) -> Option<f64>;
        let metrics: [(&str, Pick); 5] = [
            ("kill_tick", |x| x.kill_tick.map(f64::from)),
            ("hits", |x| Some(x.hits as f64)),
            ("collisions", |x| Some(x.collisions as f64)),
            ("hull", |x| Some(f64::from(x.hull))),
            ("shield", |x| x.shield.map(f64::from)),
        ];
        for (metric, pick) in metrics {
            let rv: Vec<f64> = r.iter().filter_map(pick).collect();
            let ov: Vec<f64> = o.iter().filter_map(pick).collect();
            if rv.is_empty() && ov.is_empty() {
                continue;
            }
            if o.is_empty() {
                let mut rv = rv;
                rv.sort_by(f64::total_cmp);
                let _ = writeln!(summary, "{name}: {obj}: {metric}: rust {}", fmt_sample(&rv));
                continue;
            }
            let (line, off) = compare_metric(metric, rv, ov);
            let off = off && !outcome_only;
            let _ = writeln!(
                summary,
                "{name}: {obj}: {line}{}",
                if outcome_only { " (reported)" } else { "" }
            );
            if off {
                outside.push(format!("{name}: {obj}: {metric}"));
            }
        }
    }
}

#[test]
fn g2_seed_ensemble() {
    let Some(dir) = std::env::var_os("OUTPOST_ENSEMBLE_DIR").map(PathBuf::from) else {
        println!("OUTPOST_ENSEMBLE_DIR is not set: the G2 seed ensemble was not run");
        return;
    };
    if support::world::game_data().is_none() {
        println!("outdir/ is missing: the G2 seed ensemble was not run");
        return;
    }
    let fallback_ai = std::env::var_os("OUTPOST_ORACLE_DIR").map(PathBuf::from);
    let ensemble = read_ensemble();
    let workers = std::thread::available_parallelism().map_or(2, std::num::NonZero::get);
    let mut summary = format!(
        "G2 seed ensemble: {} scenarios, {} seeds, {workers} workers\n",
        ensemble.scenarios.len(),
        ensemble.seeds.len()
    );
    let mut gross = Vec::new();
    let mut outside = Vec::new();
    for name in &ensemble.scenarios {
        let Some(base) = parse(&scenarios_dir().join(format!("{name}.txt"))) else {
            gross.push(format!("{name}: does not parse"));
            continue;
        };
        if let Some(why) = needs_ai(&base) {
            gross.push(format!("{name}: cannot run ({why})"));
            continue;
        }
        let started = std::time::Instant::now();
        let next = AtomicUsize::new(0);
        let results: Mutex<Vec<SeedResult>> = Mutex::new(Vec::new());
        std::thread::scope(|scope| {
            for _ in 0..workers.min(ensemble.seeds.len()) {
                scope.spawn(|| {
                    loop {
                        let i = next.fetch_add(1, Ordering::Relaxed);
                        let Some(&seed) = ensemble.seeds.get(i) else {
                            break;
                        };
                        let r = run_seed(&base, seed, &dir, fallback_ai.as_deref());
                        results.lock().unwrap_or_else(|e| e.into_inner()).push(r);
                    }
                });
            }
        });
        let mut results = results.into_inner().unwrap_or_else(|e| e.into_inner());
        results.sort_by_key(|r| {
            ensemble
                .seeds
                .iter()
                .position(|&s| s == r.seed)
                .unwrap_or(usize::MAX)
        });
        let oracle_runs = results.iter().filter(|r| r.oracle.is_some()).count();
        let _ = writeln!(
            summary,
            "\n== {name}: {} seeds, {oracle_runs} with an oracle run, {:.1} s",
            results.len(),
            started.elapsed().as_secs_f64()
        );
        let base_trace = |side: fn(&SeedResult) -> &Vec<u64>| {
            results.first().map(side).cloned().unwrap_or_default()
        };
        let (rust_base, oracle_base) = (
            base_trace(|r| &r.rust_trace),
            base_trace(|r| &r.oracle_trace),
        );
        for r in &results {
            let first = |o: &Option<Outcome>| {
                o.as_ref()
                    .and_then(|o| o.first_event)
                    .map_or_else(|| "-".to_owned(), |t| t.to_string())
            };
            // Where this seed's run leaves the first seed's, on each side.
            let leaves = |a: &[u64], base: &[u64]| {
                if a.is_empty() {
                    "-".to_owned()
                } else {
                    trace_difference(a, base).map_or_else(|| "never".to_owned(), |t| t.to_string())
                }
            };
            let _ = writeln!(
                summary,
                "{name} seed {}: first contact or impact rust {} oracle {}; leaves the first seed's run at tick rust {} oracle {}{}",
                r.seed,
                first(&r.rust),
                first(&r.oracle),
                leaves(&r.rust_trace, &rust_base),
                leaves(&r.oracle_trace, &oracle_base),
                if r.gross.is_empty() { "" } else { "  GROSS" }
            );
            if let Some(o) = &r.rust {
                for (obj, x) in &o.objects {
                    let theirs = r
                        .oracle
                        .as_ref()
                        .and_then(|t| t.objects.get(obj))
                        .map_or_else(|| "-".to_owned(), fmt_outcome);
                    let _ = writeln!(
                        summary,
                        "    {obj}: rust {} | oracle {theirs}",
                        fmt_outcome(x)
                    );
                }
            }
            summary.push_str(&r.lines);
            gross.extend(r.gross.iter().map(|g| format!("{name} {g}")));
        }
        let errors: Vec<f64> = results.iter().filter_map(|r| r.ram_error).collect();
        if !errors.is_empty() {
            let worst = errors.iter().copied().fold(0.0, f64::max);
            let _ = writeln!(
                summary,
                "{name}: first-contact damage: largest relative error over {} seeds {:.3} % (bound {} %)",
                errors.len(),
                100.0 * worst,
                100.0 * RAM_DAMAGE_BOUND
            );
        }
        report_statistics(&mut summary, &mut outside, name, &results, base.contact);
    }
    let _ = writeln!(
        summary,
        "\nmetrics outside the bounds ({}): {}",
        if ENVELOPES_APPROVED {
            "approved: failing"
        } else {
            "awaiting the owner: reported"
        },
        if outside.is_empty() {
            "none".to_owned()
        } else {
            outside.join("; ")
        }
    );
    print!("{summary}");
    let _ = fs::create_dir_all(&dir);
    let _ = fs::write(dir.join("rust-ensemble.txt"), &summary);
    assert!(gross.is_empty(), "gross divergence: {gross:?}");
    assert!(
        !ENVELOPES_APPROVED || outside.is_empty(),
        "outside the G2 envelopes: {outside:?}"
    );
}

#[test]
fn ensemble_statistics() {
    // The exact KS distribution for n = m = 20: P(D ≥ 9/20) ≈ 0.0524 and
    // P(D ≥ 11/20) ≈ 0.0055 (published critical values 0.45 at 5 % and
    // 0.55 at 1 %, both two-sided, within rounding of the tables).
    let p9 = ks_p_value(20, 20, 9.0 / 20.0);
    let p11 = ks_p_value(20, 20, 11.0 / 20.0);
    assert!((0.03..0.07).contains(&p9), "{p9}");
    assert!((0.002..0.01).contains(&p11), "{p11}");
    assert!((ks_p_value(20, 20, 1.0) - 2.0 / 137_846_528_820.0).abs() < 1e-12);
    // n = m = 60 (the ensemble's size): the asymptotic 1 % critical value
    // is 1.628 × sqrt(2 / 60) ≈ 0.297, so P(D ≥ 18/60) is near 1 %.
    let p60 = ks_p_value(60, 60, 18.0 / 60.0);
    assert!((0.003..0.02).contains(&p60), "{p60}");
    assert_eq!(ks_statistic(&[1.0, 2.0], &[1.0, 2.0]), 0.0);
    assert_eq!(ks_statistic(&[1.0, 1.0], &[2.0, 2.0]), 1.0);
    // Fisher: 20 of 20 against 15 of 20 is p ≈ 0.047; equal counts p = 1.
    let f = fisher_p_value(20, 20, 15, 20);
    assert!((0.04..0.05).contains(&f), "{f}");
    assert!((fisher_p_value(10, 20, 10, 20) - 1.0).abs() < 1e-9);
    assert_eq!(quantile(&[1.0, 2.0, 3.0, 4.0], 0.5), 2.5);
}
