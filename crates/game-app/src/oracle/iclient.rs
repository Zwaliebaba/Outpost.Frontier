//! `iclient.jsonl` (`oracle_iclient_proxy.cpp`, `oracle_iclient_args.cpp`):
//! one line per call into the client's event interface, written before the
//! call is handled, and for a slot with an answer a second line after it
//! with the same `seq` and `r`.

use super::json::OracleJson;

/// The IClient log of one oracle run.
#[derive(Clone, Debug, Default)]
pub struct IClientLog {
    frame: u32,
    seq: u32,
    lines: Vec<String>,
}

impl IClientLog {
    /// An empty log at frame 0.
    #[must_use]
    pub fn new() -> Self {
        Self::default()
    }

    /// `OracleFrameNumber()`: the frame the next lines carry (the number of
    /// `TickClientFrame` calls so far; 0 during start-up).
    pub fn set_frame(&mut self, frame: u32) {
        self.frame = frame;
    }

    /// `OracleIClientBegin` and `OracleIClientEnd`: the call's line, its
    /// arguments written by `args` inside `"a"`; the answer is its `seq`
    /// (`OracleNextIClientSeq`, from 1).
    pub fn call(&mut self, slot: i32, method: &str, args: impl FnOnce(&mut OracleJson)) -> u32 {
        self.seq += 1;
        let mut j = OracleJson::new();
        j.uint("frame", self.frame)
            .uint("seq", self.seq)
            .int("slot", slot)
            .str("m", method)
            .begin_object("a");
        args(&mut j);
        j.end_object();
        self.lines.push(j.finish());
        self.seq
    }

    /// `OracleIClientReturnBegin` with `Bool("r", value)` and
    /// `OracleIClientReturnEnd`: a boolean answer.
    pub fn answer_bool(&mut self, slot: i32, method: &str, seq: u32, value: bool) {
        let mut j = OracleJson::new();
        j.uint("frame", self.frame)
            .uint("seq", seq)
            .int("slot", slot)
            .str("m", method)
            .bool("r", value);
        self.lines.push(j.finish());
    }

    /// The lines so far.
    #[must_use]
    pub fn lines(&self) -> &[String] {
        &self.lines
    }

    /// The lines so far, taken.
    pub fn take_lines(&mut self) -> Vec<String> {
        std::mem::take(&mut self.lines)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn calls_and_answers_read_as_the_cpp_log() {
        let mut log = IClientLog::new();
        let seq = log.call(10, "Startup", |a| {
            a.uint("MaxPlayers", 1).uint("Port", 14041);
        });
        log.answer_bool(10, "Startup", seq, true);
        log.set_frame(398);
        log.call(15, "Shutdown", |_| {});
        assert_eq!(
            log.lines(),
            [
                r#"{"frame":0,"seq":1,"slot":10,"m":"Startup","a":{"MaxPlayers":1,"Port":14041}}"#,
                r#"{"frame":0,"seq":1,"slot":10,"m":"Startup","r":true}"#,
                r#"{"frame":398,"seq":2,"slot":15,"m":"Shutdown","a":{}}"#,
            ]
        );
    }
}
