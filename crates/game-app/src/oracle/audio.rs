//! `audio.jsonl` (P5.5): a `snd` record for each call the game makes into
//! the client's sounds, written as the call returns (`OracleOnSoundCall`,
//! `oracle_mode.cpp`): its number from 1, the method's C++ name, its
//! unsigned then its float arguments, and its result (null for none); and
//! a `locaudio` record ahead of a room's audio (`OracleOnLocationAudio`).

use super::json::OracleJson;

/// The file's lines and the calls' count.
#[derive(Clone, Debug, Default)]
pub struct AudioLog {
    seq: u32,
    /// The lines written.
    pub lines: Vec<String>,
}

impl AudioLog {
    /// No lines.
    #[must_use]
    pub fn new() -> AudioLog {
        AudioLog::default()
    }

    /// No lines; the next call is number `seq + 1`.
    #[must_use]
    pub fn after(seq: u32) -> AudioLog {
        AudioLog {
            seq,
            lines: Vec::new(),
        }
    }

    /// A `snd` record at `frame`.
    pub fn call(
        &mut self,
        frame: u32,
        method: &str,
        uints: &[u32],
        floats: &[f32],
        result: Option<bool>,
    ) {
        self.seq += 1;
        let mut j = OracleJson::new();
        j.uint("frame", frame)
            .str("rec", "snd")
            .uint("seq", self.seq)
            .str("m", method)
            .begin_array("a");
        for &u in uints {
            j.item_uint(u);
        }
        for &f in floats {
            j.item_float(f);
        }
        j.end_array();
        match result {
            Some(r) => j.bool("r", r),
            None => j.null("r"),
        };
        self.lines.push(j.finish());
    }

    /// A `locaudio` record at `frame`.
    pub fn location(&mut self, frame: u32, location: u32, keep_audio: bool) {
        let mut j = OracleJson::new();
        j.uint("frame", frame)
            .str("rec", "locaudio")
            .uint("loc", location)
            .bool("keep", keep_audio);
        self.lines.push(j.finish());
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn the_records_are_the_cpp_hooks() {
        let mut log = AudioLog::new();
        log.location(7, 0x1234, false);
        log.call(7, "SetClientBackgroundMusic", &[5, 1], &[-1.0], Some(false));
        log.call(7, "PushClientSoundDucking", &[], &[], None);
        assert_eq!(
            log.lines,
            [
                r#"{"frame":7,"rec":"locaudio","loc":4660,"keep":false}"#,
                r#"{"frame":7,"rec":"snd","seq":1,"m":"SetClientBackgroundMusic","a":[5,1,"bf800000"],"r":false}"#,
                r#"{"frame":7,"rec":"snd","seq":2,"m":"PushClientSoundDucking","a":[],"r":null}"#,
            ]
        );
    }
}
