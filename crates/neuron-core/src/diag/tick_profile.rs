//! The port-only tick profiler (decision O7): inclusive wall-clock times of
//! the client tick's sections, shown in an overlay
//! (`client_tick_profile.cpp`, `common_tick_profile.cpp`).
//!
//! The C++ compiles the profiler only with
//! `OUTPOST_FRONTIER_ENABLE_TICK_PROFILE`; otherwise every scope is empty.
//! Here the Cargo feature `tick-profile` plays that role: without it the
//! scopes and frames record nothing and enter no span. The accumulator
//! arithmetic ([`TickProfile::add_ticks`], [`TickProfile::finish`]) is
//! always available.
//!
//! The C++ keeps the profile in globals and hands Common's physics a
//! per-thread pointer. Here the profile is a value, and the physics
//! counters are a [`PhysicsTickProfile`] passed to the physics code by the
//! scope that owns them. With the feature on, every section scope also
//! enters a `tracing` span named `tick` with the section's name.

use std::cell::Cell;

use crate::time::Clock;

/// Whether the `tick-profile` feature is on.
pub const ENABLED: bool = cfg!(feature = "tick-profile");

/// The client sections (`EClientTickSection`). Children are part of their
/// parents' times.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum ClientTickSection {
    /// The whole previous tick.
    Tick,
    /// The UI tick.
    Ui,
    /// The in-process server tick.
    Server,
    /// The game tick.
    Game,
    /// Object updates.
    Objects,
    /// Physics and hits.
    Physics,
    /// The physics step.
    PhysicsStep,
    /// The physics engine's simulate call.
    PhysicsSimulate,
    /// Its prepare stage.
    PhysicsPsiPrepare,
    /// Its simulation units.
    PhysicsSimUnits,
    /// Committing hulls.
    PhysicsCommitHulls,
    /// Contacts and events.
    PhysicsContacts,
    /// Syncing objects from the physics state.
    PhysicsSync,
    /// Moving beams.
    MoveBeams,
    /// Finding beam hits.
    BeamHits,
    /// Dispatching hits.
    HitDispatch,
    /// World and effects.
    WorldFx,
    /// Sound.
    Sound,
    /// Rendering.
    Render,
    /// Presenting the frame.
    Present,
}

impl ClientTickSection {
    /// Every section in the C++ order.
    pub const ALL: [Self; 20] = [
        Self::Tick,
        Self::Ui,
        Self::Server,
        Self::Game,
        Self::Objects,
        Self::Physics,
        Self::PhysicsStep,
        Self::PhysicsSimulate,
        Self::PhysicsPsiPrepare,
        Self::PhysicsSimUnits,
        Self::PhysicsCommitHulls,
        Self::PhysicsContacts,
        Self::PhysicsSync,
        Self::MoveBeams,
        Self::BeamHits,
        Self::HitDispatch,
        Self::WorldFx,
        Self::Sound,
        Self::Render,
        Self::Present,
    ];

    /// The number of sections.
    pub const COUNT: usize = Self::ALL.len();

    /// The index in [`Self::ALL`].
    #[must_use]
    pub fn index(self) -> usize {
        self as usize
    }

    /// The overlay label; the arrows show the nesting.
    #[must_use]
    pub const fn label(self) -> &'static str {
        match self {
            Self::Tick => "Tick (prev):",
            Self::Ui => "->UI tick:",
            Self::Server => "->Server tick:",
            Self::Game => "->Game tick:",
            Self::Objects => "->->Objects:",
            Self::Physics => "->->Physics / hits:",
            Self::PhysicsStep => "->->->Physics step:",
            Self::PhysicsSimulate => "->->->->IVP simulate:",
            Self::PhysicsPsiPrepare => "->->->->->PSI prepare:",
            Self::PhysicsSimUnits => "->->->->->Sim units:",
            Self::PhysicsCommitHulls => "->->->->->Commit / hulls:",
            Self::PhysicsContacts => "->->->->->Contacts / events:",
            Self::PhysicsSync => "->->->->Sync objects:",
            Self::MoveBeams => "->->->Move beams:",
            Self::BeamHits => "->->->Find beam hits:",
            Self::HitDispatch => "->->->Dispatch hits:",
            Self::WorldFx => "->->World / FX:",
            Self::Sound => "->->Sound:",
            Self::Render => "->->Render:",
            Self::Present => "->->Present:",
        }
    }

    /// The span field value: the variant name.
    #[must_use]
    pub const fn name(self) -> &'static str {
        match self {
            Self::Tick => "Tick",
            Self::Ui => "Ui",
            Self::Server => "Server",
            Self::Game => "Game",
            Self::Objects => "Objects",
            Self::Physics => "Physics",
            Self::PhysicsStep => "PhysicsStep",
            Self::PhysicsSimulate => "PhysicsSimulate",
            Self::PhysicsPsiPrepare => "PhysicsPsiPrepare",
            Self::PhysicsSimUnits => "PhysicsSimUnits",
            Self::PhysicsCommitHulls => "PhysicsCommitHulls",
            Self::PhysicsContacts => "PhysicsContacts",
            Self::PhysicsSync => "PhysicsSync",
            Self::MoveBeams => "MoveBeams",
            Self::BeamHits => "BeamHits",
            Self::HitDispatch => "HitDispatch",
            Self::WorldFx => "WorldFx",
            Self::Sound => "Sound",
            Self::Render => "Render",
            Self::Present => "Present",
        }
    }
}

/// The physics sections (`EPhysicsTickSection`), summed over all sub-steps
/// of one client physics block.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum PhysicsTickSection {
    /// The simulate call.
    Simulate,
    /// Its prepare stage.
    PsiPrepare,
    /// Its simulation units.
    SimUnits,
    /// Committing hulls.
    CommitHulls,
    /// Contacts and events.
    Contacts,
    /// Syncing objects.
    SyncObjects,
}

impl PhysicsTickSection {
    /// Every section in the C++ order.
    pub const ALL: [Self; 6] = [
        Self::Simulate,
        Self::PsiPrepare,
        Self::SimUnits,
        Self::CommitHulls,
        Self::Contacts,
        Self::SyncObjects,
    ];

    /// The number of sections.
    pub const COUNT: usize = Self::ALL.len();

    /// The client section this one is added to.
    #[must_use]
    pub const fn client_section(self) -> ClientTickSection {
        match self {
            Self::Simulate => ClientTickSection::PhysicsSimulate,
            Self::PsiPrepare => ClientTickSection::PhysicsPsiPrepare,
            Self::SimUnits => ClientTickSection::PhysicsSimUnits,
            Self::CommitHulls => ClientTickSection::PhysicsCommitHulls,
            Self::Contacts => ClientTickSection::PhysicsContacts,
            Self::SyncObjects => ClientTickSection::PhysicsSync,
        }
    }
}

fn read<C: Clock + ?Sized>(clock: &C) -> i64 {
    clock.counter().unwrap_or(0)
}

fn span(section: &'static str) -> Option<tracing::span::EnteredSpan> {
    ENABLED.then(|| tracing::trace_span!("tick", section).entered())
}

/// The client tick profile: section counts of the running frame and the
/// milliseconds of the last finished one.
#[derive(Debug)]
pub struct TickProfile<C: Clock> {
    clock: C,
    frequency: Cell<i64>,
    ticks: [Cell<i64>; ClientTickSection::COUNT],
    completed_ms: [Cell<f64>; ClientTickSection::COUNT],
    active: Cell<bool>,
    rendering: Cell<bool>,
    render_start: Cell<i64>,
}

impl<C: Clock> TickProfile<C> {
    /// An idle profile reading `clock`.
    pub fn new(clock: C) -> Self {
        Self {
            clock,
            frequency: Cell::new(0),
            ticks: Default::default(),
            completed_ms: Default::default(),
            active: Cell::new(false),
            rendering: Cell::new(false),
            render_start: Cell::new(0),
        }
    }

    /// Whether a frame is running.
    pub fn is_active(&self) -> bool {
        self.active.get()
    }

    /// The counts of `section` in the running frame.
    pub fn ticks(&self, section: ClientTickSection) -> i64 {
        self.ticks[section.index()].get()
    }

    /// The milliseconds of `section` in the last finished frame.
    pub fn completed_ms(&self, section: ClientTickSection) -> f64 {
        self.completed_ms[section.index()].get()
    }

    /// Adds `ticks` counts to `section`.
    pub fn add_ticks(&self, section: ClientTickSection, ticks: i64) {
        let cell = &self.ticks[section.index()];
        cell.set(cell.get().wrapping_add(ticks));
    }

    /// Starts a frame: the counts are cleared, the frequency is read once.
    /// The frame ends when the guard drops. A frame inside a frame records
    /// nothing of its own (`FClientTickProfileFrame`).
    pub fn frame(&self) -> TickFrame<'_, C> {
        let owns = ENABLED && !self.active.get();
        let mut start = 0;
        if owns {
            self.begin();
            start = read(&self.clock);
        }
        TickFrame {
            profile: self,
            start,
            owns,
        }
    }

    fn begin(&self) {
        if self.frequency.get() == 0 {
            self.frequency.set(self.clock.frequency().unwrap_or(0));
        }
        for cell in &self.ticks {
            cell.set(0);
        }
        self.rendering.set(false);
        self.active.set(true);
    }

    /// Ends a frame whose whole duration was `tick_counts`: the counts
    /// become milliseconds, `counts * 1000 / frequency`.
    pub fn finish(&self, tick_counts: i64) {
        self.ticks[ClientTickSection::Tick.index()].set(tick_counts);
        self.active.set(false);
        self.rendering.set(false);
        let frequency = self.frequency.get() as f64;
        for (ticks, ms) in self.ticks.iter().zip(&self.completed_ms) {
            ms.set(ticks.get() as f64 * 1000.0 / frequency);
        }
    }

    /// Sets the frequency the milliseconds are computed with, as the first
    /// frame does from the clock.
    pub fn set_frequency(&self, frequency: i64) {
        self.frequency.set(frequency);
    }

    /// Times `section` until the guard drops or is stopped
    /// (`FClientTickScope`); records only inside a frame.
    pub fn scope(&self, section: ClientTickSection) -> TickScope<'_, C> {
        let running = self.active.get();
        let start = if running { read(&self.clock) } else { 0 };
        TickScope {
            profile: self,
            section,
            start,
            running,
            _span: if running { span(section.name()) } else { None },
        }
    }

    /// `BeginClientProfileRender`.
    pub fn begin_render(&self) {
        if self.active.get() && !self.rendering.get() {
            self.render_start.set(read(&self.clock));
            self.rendering.set(true);
        }
    }

    /// `EndClientProfileRender`.
    pub fn end_render(&self) {
        if self.rendering.get() {
            let elapsed = read(&self.clock).wrapping_sub(self.render_start.get());
            self.add_ticks(ClientTickSection::Render, elapsed);
            self.rendering.set(false);
        }
    }

    /// Arms physics counters for the client physics block
    /// (`FClientPhysicsProfileScope`). The physics code times its sections
    /// with [`PhysicsScope::counters`]; when the scope ends, they are added
    /// to the matching client sections.
    pub fn physics_scope(&self) -> PhysicsScope<'_, C> {
        PhysicsScope {
            profile: self,
            counters: PhysicsTickProfile::new(),
            running: self.active.get(),
        }
    }
}

/// A running frame; see [`TickProfile::frame`].
#[derive(Debug)]
pub struct TickFrame<'a, C: Clock> {
    profile: &'a TickProfile<C>,
    start: i64,
    owns: bool,
}

impl<C: Clock> Drop for TickFrame<'_, C> {
    fn drop(&mut self) {
        if self.owns {
            let elapsed = read(&self.profile.clock).wrapping_sub(self.start);
            self.profile.finish(elapsed);
        }
    }
}

/// A timed section; see [`TickProfile::scope`].
#[derive(Debug)]
pub struct TickScope<'a, C: Clock> {
    profile: &'a TickProfile<C>,
    section: ClientTickSection,
    start: i64,
    running: bool,
    _span: Option<tracing::span::EnteredSpan>,
}

impl<C: Clock> TickScope<'_, C> {
    /// Ends the section early; dropping the guard then does nothing.
    pub fn stop(&mut self) {
        if !self.running {
            return;
        }
        let elapsed = read(&self.profile.clock).wrapping_sub(self.start);
        self.profile.add_ticks(self.section, elapsed);
        self.running = false;
        self._span = None;
    }
}

impl<C: Clock> Drop for TickScope<'_, C> {
    fn drop(&mut self) {
        self.stop();
    }
}

/// Physics section counts (`FPhysicsTickProfile`).
#[derive(Debug, Default)]
pub struct PhysicsTickProfile {
    ticks: [Cell<i64>; PhysicsTickSection::COUNT],
}

impl PhysicsTickProfile {
    /// All counts zero.
    #[must_use]
    pub fn new() -> Self {
        Self::default()
    }

    /// The counts of `section`.
    pub fn ticks(&self, section: PhysicsTickSection) -> i64 {
        self.ticks[section as usize].get()
    }

    /// Adds `ticks` counts to `section`.
    pub fn add_ticks(&self, section: PhysicsTickSection, ticks: i64) {
        let cell = &self.ticks[section as usize];
        cell.set(cell.get().wrapping_add(ticks));
    }
}

/// Times a physics section into `profile` until dropped or stopped
/// (`FPhysicsTickScope`). With `profile` `None`, as when no client scope
/// armed the counters, it records nothing.
#[derive(Debug)]
pub struct PhysicsSectionScope<'a, C: Clock + ?Sized> {
    profile: Option<&'a PhysicsTickProfile>,
    clock: &'a C,
    section: PhysicsTickSection,
    start: i64,
}

impl<'a, C: Clock + ?Sized> PhysicsSectionScope<'a, C> {
    /// Starts timing `section`.
    pub fn new(
        profile: Option<&'a PhysicsTickProfile>,
        clock: &'a C,
        section: PhysicsTickSection,
    ) -> Self {
        let profile = profile.filter(|_| ENABLED);
        let start = if profile.is_some() { read(clock) } else { 0 };
        Self {
            profile,
            clock,
            section,
            start,
        }
    }

    /// Ends the section early.
    pub fn stop(&mut self) {
        if let Some(profile) = self.profile.take() {
            profile.add_ticks(self.section, read(self.clock).wrapping_sub(self.start));
        }
    }
}

impl<C: Clock + ?Sized> Drop for PhysicsSectionScope<'_, C> {
    fn drop(&mut self) {
        self.stop();
    }
}

/// The client physics block's counters; see [`TickProfile::physics_scope`].
#[derive(Debug)]
pub struct PhysicsScope<'a, C: Clock> {
    profile: &'a TickProfile<C>,
    counters: PhysicsTickProfile,
    running: bool,
}

impl<C: Clock> PhysicsScope<'_, C> {
    /// The counters to hand to the physics code, `None` outside a frame.
    pub fn counters(&self) -> Option<&PhysicsTickProfile> {
        self.running.then_some(&self.counters)
    }

    /// The clock the physics code times with.
    pub fn clock(&self) -> &C {
        &self.profile.clock
    }

    /// Adds the counters to the client sections and disarms them.
    pub fn stop(&mut self) {
        if !self.running {
            return;
        }
        for section in PhysicsTickSection::ALL {
            self.profile
                .add_ticks(section.client_section(), self.counters.ticks(section));
        }
        self.running = false;
    }
}

impl<C: Clock> Drop for PhysicsScope<'_, C> {
    fn drop(&mut self) {
        self.stop();
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::time::ManualClock;

    #[test]
    fn finish_converts_counts_to_milliseconds() {
        let profile = TickProfile::new(ManualClock::new(Some(10_000_000)));
        profile.set_frequency(10_000_000);
        profile.add_ticks(ClientTickSection::Ui, 12_345);
        profile.add_ticks(ClientTickSection::Ui, 5);
        profile.finish(160_000);
        assert_eq!(
            profile.completed_ms(ClientTickSection::Tick),
            160_000.0 * 1000.0 / 10_000_000.0
        );
        assert_eq!(
            profile.completed_ms(ClientTickSection::Ui),
            12_350.0 * 1000.0 / 10_000_000.0
        );
        assert_eq!(profile.completed_ms(ClientTickSection::Present), 0.0);
        assert!(!profile.is_active());
    }

    #[test]
    fn physics_sections_map_to_client_sections() {
        assert_eq!(PhysicsTickSection::COUNT, 6);
        assert_eq!(ClientTickSection::COUNT, 20);
        for (index, section) in ClientTickSection::ALL.iter().enumerate() {
            assert_eq!(section.index(), index);
        }
        assert_eq!(
            PhysicsTickSection::SyncObjects.client_section(),
            ClientTickSection::PhysicsSync
        );
    }

    #[test]
    fn scopes_record_only_with_the_feature() {
        let clock = ManualClock::new(Some(1000));
        let profile = TickProfile::new(&clock);
        {
            let _frame = profile.frame();
            {
                let _ui = profile.scope(ClientTickSection::Ui);
                clock.advance(7, 0);
            }
            let physics = profile.physics_scope();
            {
                let _step = PhysicsSectionScope::new(
                    physics.counters(),
                    physics.clock(),
                    PhysicsTickSection::Contacts,
                );
                clock.advance(3, 0);
            }
            drop(physics);
            profile.begin_render();
            clock.advance(2, 0);
            profile.end_render();
        }
        let expect = |ms: f64| if ENABLED { ms } else { 0.0 };
        assert_eq!(profile.completed_ms(ClientTickSection::Tick), expect(12.0));
        assert_eq!(profile.completed_ms(ClientTickSection::Ui), expect(7.0));
        assert_eq!(
            profile.completed_ms(ClientTickSection::PhysicsContacts),
            expect(3.0)
        );
        assert_eq!(profile.completed_ms(ClientTickSection::Render), expect(2.0));
    }
}
