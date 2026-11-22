//! The system paths of TU41 (`Content/SystemConnections.cpp`): three tables
//! of the shortest system path from one system to another, over every
//! connection (type 0), the jump gates (type 1) and the jump holes (type 2);
//! their loader, their builder, their `[SystemConnections]` writer and their
//! readers (work package P3.4c, decision record 0008 point 10).
//!
//! GameSupport's `LoadGame` rebuilds them (`InitSystemPathTables`,
//! `GameSupport.cpp:657-668`), so they live in
//! [`ContentState`](super::ContentState); that call site is P3.4d's, with
//! the system records and gate locks the builder reads, which it takes here
//! as arguments.
//!
//! `std::map` is a [`BTreeMap`] and `std::list` a [`Vec`] in list order.

use std::collections::BTreeMap;
use std::io::{self, Write};

use game_sim::data::DataSource;
use neuron_core::hash::c_str;
use neuron_core::ini::IniReader;
use neuron_core::math::Vector;

use super::tables::open;
use crate::api;
use crate::server::Cx;

/// The files, relative to the data root, in the loader's order.
pub mod files {
    /// Every connection (type 0).
    pub const ALL: &[u8] = b"Universe\\systems_shortest_path.ini";
    /// The jump gates (type 1).
    pub const GATE: &[u8] = b"Universe\\shortest_legal_path.ini";
    /// The jump holes (type 2).
    pub const HOLE: &[u8] = b"Universe\\shortest_illegal_path.ini";
    /// The writer's system names.
    pub const UNIVERSE: &[u8] = b"Universe\\universe.ini";
}

/// `FSystemPathsFrom`: the path to each system, by destination.
pub type PathsFrom = BTreeMap<u32, Vec<u32>>;

/// `FSystemPathTable`: [`PathsFrom`] by source system.
pub type PathTable = BTreeMap<u32, PathsFrom>;

/// `GSystemPathsAll`, `GSystemPathsGate` and `GSystemPathsHole`, with
/// `GSystemPathTablesBuilt`.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct SystemPaths {
    /// Type 0, every connection.
    pub all: PathTable,
    /// Type 1, the jump gates.
    pub gate: PathTable,
    /// Type 2, the jump holes.
    pub hole: PathTable,
    /// Set by the first [`SystemPaths::build`] and never cleared: a later
    /// build does nothing.
    pub built: bool,
}

impl SystemPaths {
    /// The type switch every reader opens with; `None` for a type other
    /// than 0, 1 and 2.
    #[must_use]
    pub fn table(&self, kind: i32) -> Option<&PathTable> {
        match kind {
            0 => Some(&self.all),
            1 => Some(&self.gate),
            2 => Some(&self.hole),
            _ => None,
        }
    }

    fn table_mut(&mut self, kind: i32) -> Option<&mut PathTable> {
        match kind {
            0 => Some(&mut self.all),
            1 => Some(&mut self.gate),
            2 => Some(&mut self.hole),
            _ => None,
        }
    }

    /// `GetSystemPathLength`: the size of the `from` to `to` path, -1 when
    /// there is none.
    #[must_use]
    pub fn path_length(&self, from: u32, to: u32, kind: i32) -> i32 {
        self.table(kind)
            .and_then(|t| t.get(&from))
            .and_then(|p| p.get(&to))
            .map_or(-1, |p| p.len() as i32)
    }

    /// `GetSystemPath`: a copy of the `from` to `to` path, empty when there
    /// is none.
    #[must_use]
    pub fn path(&self, from: u32, to: u32, kind: i32) -> Vec<u32> {
        self.table(kind)
            .and_then(|t| t.get(&from))
            .and_then(|p| p.get(&to))
            .cloned()
            .unwrap_or_default()
    }

    /// `LoadSystemPathTables`: the three tables emptied and read back from
    /// their files under `source`; a file that does not open reads as
    /// empty, as the C++'s untested opens do.
    pub fn load(&mut self, cx: &mut Cx<'_>, source: &dyn DataSource) {
        self.all.clear();
        self.gate.clear();
        self.hole.clear();
        for (path, kind) in [(files::ALL, 0), (files::GATE, 1), (files::HOLE, 2)] {
            let mut reader = open(source, path);
            while reader.read_header() {
                if reader.is_header(b"SystemConnections") {
                    let table = self.table_mut(kind).expect("a path type");
                    load_section(cx, &mut reader, table);
                }
            }
        }
    }

    /// `InitSystemPathTables`' test and load: when
    /// `Universe\systems_shortest_path.ini` exists under `source` (the C++
    /// opens it with `fopen` on the data path), the tables are read back and
    /// the result is true. False when it does not: the C++ then builds the
    /// tables and writes them ([`SystemPaths::build`], [`SystemPaths::write`]),
    /// which needs the system records and gate locks of P3.4d, its caller.
    pub fn init(&mut self, cx: &mut Cx<'_>, source: &dyn DataSource) -> bool {
        if source.read(files::ALL).is_none() {
            return false;
        }
        self.load(cx, source);
        true
    }

    /// `BuildSystemPathTables` without its write: once per process, the
    /// tables emptied and computed from `systems`, the keys of the system
    /// records in key order (P3.4d's table), each a shortest-path search per
    /// connection type. `is_locked(object)` is the gate-lock test the C++
    /// asks for player 0 (`IsContentGateLocked`, P3.4d). True when the
    /// tables were built now, so that the caller writes them.
    pub fn build(
        &mut self,
        cx: &mut Cx<'_>,
        systems: &[u32],
        is_locked: &dyn Fn(u32) -> bool,
    ) -> bool {
        if self.built {
            return false;
        }
        self.hole.clear();
        self.gate.clear();
        self.all.clear();
        self.built = true;
        for kind in 0..3 {
            for &source in systems {
                let table = self.table_mut(kind).expect("a path type");
                let mut table = std::mem::take(table);
                build_paths_from(cx, source, kind, &mut table, systems, is_locked);
                *self.table_mut(kind).expect("a path type") = table;
            }
        }
        true
    }

    /// `WriteSystemPathTables`' three files: each table as its
    /// `[SystemConnections]` sections, systems by the nickname of `names`
    /// ([`system_names`]). The C++ opens the files in text mode (`"wt"`),
    /// so its `\n` reach the disk as `\r\n`; these writers get the `\n`, and
    /// the caller's file does the translation, as P3.3's save writer does.
    ///
    /// A writer that is `None` is a file that did not open. The C++ writes
    /// the gate file's path systems through the first file's stream, which
    /// reuses the closed stream's slot, so they reach the gate file when the
    /// first file opened and are dropped when it did not. A system without a
    /// name is written as an empty name, where the C++ reads the map's head
    /// node (D792).
    ///
    /// # Errors
    ///
    /// The writers' errors.
    pub fn write(
        &self,
        names: &BTreeMap<u32, Vec<u8>>,
        all: Option<&mut dyn Write>,
        gate: Option<&mut dyn Write>,
        hole: Option<&mut dyn Write>,
    ) -> io::Result<()> {
        let all_opened = all.is_some();
        if let Some(out) = all {
            write_file(out, true, &self.all, names)?;
        }
        if let Some(out) = gate {
            write_file(out, all_opened, &self.gate, names)?;
        }
        if let Some(out) = hole {
            write_file(out, true, &self.hole, names)?;
        }
        Ok(())
    }
}

/// `LoadSystemPathSection` (`:301-328`): every value line, `Path` or not,
/// inserts `{to, the systems read so far}` into the section's map (a unique
/// insert) and empties the list; the map goes into `table` under the last
/// `from` (a unique insert), unless its low 30 bits are 0.
///
/// `GetSystemID` leaves its out value on a miss, so an unknown name repeats
/// the previous id. The C++ never initialises the path systems' id, so an
/// unknown first path system leaves stack bits; the port starts from 0
/// (D793).
pub fn load_section(cx: &mut Cx<'_>, reader: &mut IniReader, table: &mut PathTable) {
    let mut to = 0u32;
    let mut from = 0u32;
    let mut path_system = 0u32;
    let mut path = Vec::new();
    let mut paths = PathsFrom::new();
    while reader.read_value() {
        if reader.is_value(b"Path") {
            system_id(cx, &mut from, &reader.value_string_at(0));
            system_id(cx, &mut to, &reader.value_string_at(1));
            let mut index = 2;
            while !reader.is_value_empty(index) {
                system_id(cx, &mut path_system, &reader.value_string_at(index));
                path.push(path_system);
                index += 1;
            }
        }
        paths.entry(to).or_insert_with(|| path.clone());
        path.clear();
    }
    if from & 0x3fff_ffff != 0 {
        table.entry(from).or_insert(paths);
    }
}

/// `pub::GetSystemID` into a slot: written only on success.
fn system_id(cx: &mut Cx<'_>, slot: &mut u32, name: &[u8]) {
    if let Ok(id) = api::misc::system_id(cx, c_str(name)) {
        *slot = id;
    }
}

/// `CollectOpenConnectionSystems`: the destination systems of `system`'s
/// connections of `kind`, in file order, whose object `is_locked` does not
/// lock; nothing when the system is unknown.
pub fn collect_open_connection_systems(
    cx: &mut Cx<'_>,
    system: u32,
    kind: i32,
    is_locked: &dyn Fn(u32) -> bool,
) -> Vec<u32> {
    let Ok(connections) = api::system::enumerate_connections(cx, system, kind as u32) else {
        return Vec::new();
    };
    connections
        .iter()
        .filter(|c| !is_locked(c.object_id))
        .map(|c| c.goto_system)
        .collect()
}

/// `CopyShortestPath`: the first of the shortest of `paths` (a later one of
/// equal length does not replace it), `None` when there is none.
#[must_use]
pub fn copy_shortest_path(paths: &[Vec<u32>]) -> Option<&Vec<u32>> {
    let mut best: Option<&Vec<u32>> = None;
    for path in paths {
        if best.is_some_and(|b| b.len() <= path.len()) {
            continue;
        }
        best = Some(path);
    }
    best
}

/// One search of `BuildSystemPathTables` for one source system and
/// connection type (`BuildSystemPathsFrom`, `:67-150`). The type-0 pass
/// tests that the source has a system record (it always has: the sources
/// are the records' keys); the others test the path's last system and drop
/// the path on a miss.
fn build_paths_from(
    cx: &mut Cx<'_>,
    source: u32,
    kind: i32,
    table: &mut PathTable,
    systems: &[u32],
    is_locked: &dyn Fn(u32) -> bool,
) {
    let mut queue: Vec<Vec<u32>> = collect_open_connection_systems(cx, source, kind, is_locked)
        .into_iter()
        .map(|next| vec![source, next])
        .collect();
    while !queue.is_empty() {
        let path = copy_shortest_path(&queue).cloned().unwrap_or_default();
        let last = *path.last().expect("a queued path has two systems");
        let checked = if kind == 0 { source } else { last };
        if systems.binary_search(&checked).is_err() {
            if kind != 0 {
                queue.retain(|p| *p != path);
            }
            continue;
        }
        table
            .entry(source)
            .or_default()
            .entry(source)
            .or_insert_with(|| vec![source]);
        queue.retain(|p| *p != path);
        let known = table
            .get(&source)
            .and_then(|p| p.get(&last))
            .map_or(-1, |p| p.len() as i32);
        if known >= 0 && known <= path.len() as i32 {
            continue;
        }
        table.entry(source).or_default().insert(last, path.clone());
        for next in collect_open_connection_systems(cx, last, kind, is_locked) {
            let mut longer = path.clone();
            longer.push(next);
            queue.push(longer);
        }
    }
}

/// The body of one file of `WriteSystemPathTables` (`WriteSystemPathFile`,
/// `:157-178`).
fn write_file(
    out: &mut dyn Write,
    with_path_systems: bool,
    table: &PathTable,
    names: &BTreeMap<u32, Vec<u8>>,
) -> io::Result<()> {
    let name = |id: &u32| names.get(id).map_or(&[][..], Vec::as_slice);
    for (from, paths) in table {
        out.write_all(b"[SystemConnections]\n\n")?;
        for (to, path) in paths {
            out.write_all(b"Path = ")?;
            out.write_all(name(from))?;
            out.write_all(b", ")?;
            out.write_all(name(to))?;
            if with_path_systems {
                for system in path {
                    out.write_all(b", ")?;
                    out.write_all(name(system))?;
                }
            }
            out.write_all(b"\n")?;
        }
        out.write_all(b"\n")?;
    }
    Ok(())
}

/// The names `WriteSystemPathTables` reads from `Universe\universe.ini`:
/// each `[System]` `nickname`, at most 15 bytes (a `TString<0x10>`), under
/// its `GetSystemID` id (a unique insert). `GetSystemID` leaves the id on a
/// miss, so an unknown nickname is filed under the previous one's id; the
/// C++ starts from an uninitialised id, the port from 0.
pub fn system_names(cx: &mut Cx<'_>, source: &dyn DataSource) -> BTreeMap<u32, Vec<u8>> {
    let mut names = BTreeMap::new();
    let mut reader = open(source, files::UNIVERSE);
    let mut id = 0u32;
    while reader.read_header() {
        if !reader.is_header(b"System") {
            continue;
        }
        while reader.read_value() {
            if !reader.is_value(b"nickname") {
                continue;
            }
            let mut nickname = c_str(&reader.value_string()).to_vec();
            nickname.truncate(15);
            system_id(cx, &mut id, &nickname);
            names.entry(id).or_insert(nickname);
        }
    }
    names
}

/// `GetNextHopConnectionObject`: the connection object in `from` that
/// leads to the second system of the `from` to `to` path; 0 when there is
/// none, when either id's low 30 bits are 0, or when no connection of the
/// type goes there.
pub fn next_hop_connection_object(
    cx: &mut Cx<'_>,
    paths: &SystemPaths,
    from: u32,
    to: u32,
    kind: i32,
) -> u32 {
    let path = paths.path(from, to, kind);
    let next = path.get(1).copied().unwrap_or(0);
    if from & 0x3fff_ffff == 0 || next & 0x3fff_ffff == 0 {
        return 0;
    }
    let Ok(connections) = api::system::enumerate_connections(cx, from, kind as u32) else {
        return 0;
    };
    connections
        .iter()
        .find(|c| c.goto_system == next)
        .map_or(0, |c| c.object_id)
}

/// `GetConnectionPosition`: the position of connection object `object` in
/// `system`, `None` (-2) when it is not one.
pub fn connection_position(cx: &mut Cx<'_>, system: u32, object: u32) -> Option<Vector> {
    api::system::enumerate_connections(cx, system, 0)
        .ok()?
        .iter()
        .find(|c| c.object_id == object)
        .map(|c| c.pos)
}

/// `GetConnectionGotoSystem`: the destination of connection object
/// `object` in `system`, `None` (-2) when it is not one.
pub fn connection_goto_system(cx: &mut Cx<'_>, system: u32, object: u32) -> Option<u32> {
    api::system::enumerate_connections(cx, system, 0)
        .ok()?
        .iter()
        .find(|c| c.object_id == object)
        .map(|c| c.goto_system)
}
