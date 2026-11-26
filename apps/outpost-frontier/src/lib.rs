//! The client executable's wiring (`apps/outpost-frontier`): what connects
//! the client (`game-app`) to the in-process server (`game-logic`), which
//! neither crate may do itself (AGENTS.md, record 0002 point 4).
#![forbid(unsafe_code)]

pub mod local_server;
