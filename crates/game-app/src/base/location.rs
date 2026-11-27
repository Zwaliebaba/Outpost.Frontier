//! The location controller of `menu_universe_base.cpp` (P5.9): so far the
//! audio a room's entry sets (`ApplyLocationAudio`, P5.5).

use super::room_data::RoomData;
use crate::audio::SoundCalls;

/// `ApplyLocationAudio` on the location's room: its music and ambience,
/// each cleared when it does not loop unless `keep_audio` is set (the
/// controller's `+0x58`, which this clears), with the room's loop flags
/// and the default fade; then the master fade snapped to -96 dB and raised
/// to 0 over `music_fade_time`.
pub fn apply_location_audio(sound: &mut dyn SoundCalls, room: &RoomData, keep_audio: &mut bool) {
    let mut music = room.music;
    let mut ambient = room.ambient;
    if !*keep_audio {
        if !room.music_loop {
            music = 0;
        }
        if !room.ambient_loop {
            ambient = 0;
        }
    }
    sound.set_background_music(music, u32::from(room.music_loop), -1.0);
    sound.set_background_ambient(ambient, u32::from(room.ambient_loop), -1.0);
    *keep_audio = false;
    let fade = sound.music_fade_time();
    sound.snap_audio_fade(-96.0);
    sound.set_audio_fade(0.0, fade);
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::audio::{Logged, Silent};
    use crate::oracle::audio::AudioLog;

    fn calls(room: &RoomData, keep: bool) -> (Vec<String>, bool) {
        let mut log = AudioLog::new();
        let mut keep = keep;
        apply_location_audio(
            &mut Logged {
                inner: &mut Silent {
                    music_fade_time: 2.0,
                },
                log: &mut log,
                frame: 3,
            },
            room,
            &mut keep,
        );
        (log.lines, keep)
    }

    #[test]
    fn a_room_sets_its_music_and_ambience_and_raises_the_fade() {
        let room = RoomData {
            music: 11,
            ambient: 22,
            ambient_loop: false,
            ..RoomData::default()
        };
        let (lines, keep) = calls(&room, false);
        assert!(!keep);
        assert_eq!(
            lines,
            [
                r#"{"frame":3,"rec":"snd","seq":1,"m":"SetClientBackgroundMusic","a":[11,1,"bf800000"],"r":false}"#,
                // Not looping: cleared.
                r#"{"frame":3,"rec":"snd","seq":2,"m":"SetClientBackgroundAmbient","a":[0,0,"bf800000"],"r":false}"#,
                r#"{"frame":3,"rec":"snd","seq":3,"m":"SnapClientAudioFade","a":["c2c00000"],"r":null}"#,
                r#"{"frame":3,"rec":"snd","seq":4,"m":"SetClientAudioFade","a":["00000000","40000000"],"r":null}"#,
            ]
        );
        // Kept: the id stays, and the flag is cleared.
        let (lines, keep) = calls(&room, true);
        assert!(!keep);
        assert!(lines[1].contains(r#""a":[22,0,"#), "{}", lines[1]);
    }
}
