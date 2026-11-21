//! `pub::Audio` (`Server/server_pub.cpp:7681-7716`): one `IClient` call
//! each, no checks, always 0.
//!
//! | Export | Line | Port |
//! | --- | --- | --- |
//! | `SetMusic` | 7688 | [`set_music`] |
//! | `CancelMusic` | 7701 | [`cancel_music`] |
//! | `PlaySoundEffect` | 7710 | [`play_sound_effect`] |

use game_sim::protocol::ToClient;
use game_sim::protocol::to_client::{CancelMusic, PlaySoundEffect, ServerMessage, SetMusic};

use crate::server::Cx;

fn send(cx: &mut Cx<'_>, client: u32, message: impl Into<ServerMessage>) {
    cx.logic
        .client
        .send(client, ToClient::Server(message.into()));
}

/// `pub::Audio::Tryptich`: the music to play.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct Tryptich {
    /// `Field0`: the space music.
    pub space: u32,
    /// `Field4`: the danger music.
    pub danger: u32,
    /// `Field8`: the battle music.
    pub battle: u32,
    /// `FieldC`: the override music.
    pub override_music: u32,
    /// `Field10`, a byte (sent as 0 or 1).
    pub flag: u8,
    /// `Field14`: the fade, a float's bits.
    pub fade: f32,
}

/// `SetMusic` (`SendServerSetMusic`, slot +0x1e8, ch2 0x57): the four
/// tracks, the flag as 0 or 1 and the fade. Returns 0.
pub fn set_music(cx: &mut Cx<'_>, player: u32, track: &Tryptich) -> i32 {
    let message = SetMusic {
        field_20: track.space,
        field_24: track.danger,
        field_28: track.battle,
        field_2c: track.override_music,
        field_30: u8::from(track.flag != 0),
        field_34: track.fade,
    };
    send(cx, player, message);
    0
}

/// `CancelMusic` (`SendServerCancelMusic`, slot +0x1ec, ch2 0x58). Returns
/// 0.
pub fn cancel_music(cx: &mut Cx<'_>, player: u32) -> i32 {
    send(cx, player, CancelMusic {});
    0
}

/// `PlaySoundEffect` (`SendServerPlaySoundEffect`, slot +0x1f0, ch2 0x59).
/// Returns 0.
pub fn play_sound_effect(cx: &mut Cx<'_>, player: u32, sound: u32) -> i32 {
    send(cx, player, PlaySoundEffect { value: sound });
    0
}
