//! The client's space effects (`Client\space\Effect.cpp`, work package
//! P5.6): the frame's effect vertex batch ([`batch`]) and the beams'
//! geometry ([`beam`]).
//!
//! This module holds no game data or rules. Its callers in game-app hand
//! it plain parameters (a beam's shape from its archetype, the options and
//! camera state that the reference reads from globals) and the scene's
//! transforms.

pub mod batch;
pub mod beam;
