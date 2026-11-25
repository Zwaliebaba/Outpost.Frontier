//! The reputation handles of Content's space objects
//! (`FContentPlayerStateBase`, `Content/ContentPlayerState.h:58-71`; work
//! package P3.6c): reference-counted objects that hold one vibe
//! (`Reputation`, `+0x08`). The C++ keeps them by pointer, shared between
//! a parameter block, the NPC ship made from it and the spawner; the port
//! keeps them in Content's arena by [`RepHandle`] with their count, as
//! `FContentRefObject` counts (`AddRef`, `Release`, the last release runs
//! the destructor).
//!
//! Two kinds are made: a named NPC's (`FNpcNameReputation`, TU42, whose
//! destructor gives the name back to the faction's pool and frees the
//! vibe; the name builder of [`super::npc_names`]) and a plain one around a
//! vibe the caller owns (the scenarios' NPC ships, whose vibe the harness
//! made).

use std::collections::BTreeMap;

use crate::server::Cx;

/// A reputation handle in Content's arena.
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub struct RepHandle(pub u32);

/// What the handle's destructor does.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum RepKind {
    /// `FNpcNameReputation`: the vibe was allocated from the name; the
    /// destructor returns the name's pool entries to the faction's tables
    /// and frees the vibe ([`super::npc_names::release_name_reputation`]).
    Named,
    /// A handle around a vibe its maker keeps: nothing is freed.
    Plain,
}

/// One handle (`FContentPlayerStateBase`).
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct RepEntry {
    /// The reference count (`FContentRefObject +0x04`).
    pub refs: u32,
    /// `Reputation` (`+0x08`).
    pub vibe: i32,
    /// The handle's class.
    pub kind: RepKind,
}

/// The arena (`ContentState::rep_handles`).
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct RepHandles {
    next: u32,
    entries: BTreeMap<RepHandle, RepEntry>,
}

impl RepHandles {
    /// A new handle with one reference (the creator's).
    pub fn create(&mut self, vibe: i32, kind: RepKind) -> RepHandle {
        self.next += 1;
        let handle = RepHandle(self.next);
        self.entries.insert(
            handle,
            RepEntry {
                refs: 1,
                vibe,
                kind,
            },
        );
        handle
    }

    /// The handle's entry.
    #[must_use]
    pub fn get(&self, handle: RepHandle) -> Option<&RepEntry> {
        self.entries.get(&handle)
    }

    /// `AddRef`.
    pub fn add_ref(&mut self, handle: RepHandle) {
        if let Some(e) = self.entries.get_mut(&handle) {
            e.refs += 1;
        }
    }

    /// `GetReputation()`: the vibe, -1 for a handle that is gone.
    #[must_use]
    pub fn vibe(&self, handle: Option<RepHandle>) -> i32 {
        handle
            .and_then(|h| self.entries.get(&h))
            .map_or(-1, |e| e.vibe)
    }
}

/// `Release`: the count drops, and the last release runs the handle's
/// destructor (a named NPC's frees its vibe and returns its name).
pub fn release(cx: &mut Cx<'_>, handle: RepHandle) {
    let handles = &mut cx.logic.content.state.rep_handles;
    let Some(entry) = handles.entries.get_mut(&handle) else {
        return;
    };
    entry.refs = entry.refs.saturating_sub(1);
    if entry.refs != 0 {
        return;
    }
    let Some(entry) = handles.entries.remove(&handle) else {
        return;
    };
    if entry.kind == RepKind::Named {
        super::npc_names::release_name_reputation(cx, entry.vibe);
    }
}

/// `FContentPlayerStateBase::GetAffiliation`: the vibe's affiliation (the
/// call's status is ignored: -1 when it fails).
pub fn affiliation(cx: &mut Cx<'_>, handle: Option<RepHandle>) -> u32 {
    let vibe = cx.logic.content.state.rep_handles.vibe(handle);
    crate::api::reputation::affiliation(cx, vibe).unwrap_or(u32::MAX)
}
