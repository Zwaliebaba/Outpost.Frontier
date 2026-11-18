//! The defaults the device applies: DX8RENDERER's built-in table, then the
//! engine INI section `Startup` names (`[RenderPipeline]` in the shipped
//! `NeuronComponents.ini`), read as `ReadProfileNumber` reads a number.

use neuron_core::ini::EngineSection;
use neuron_core::msvcrt;

use super::state::{PipelineProperty, RENDER_STATES, RenderStateId};

/// `strtoul(text, nullptr, 16)` of the Microsoft C runtime: white space, a
/// sign, an optional `0x`, hex digits; overflow gives `ULONG_MAX`, and a
/// minus sign negates the result.
fn strtoul_hex(text: &[u8]) -> u32 {
    let mut at = 0;
    let byte = |at: usize| text.get(at).copied().unwrap_or(0);
    while matches!(byte(at), b' ' | b'\t' | b'\n' | 0x0b | 0x0c | b'\r') {
        at += 1;
    }
    let negative = byte(at) == b'-';
    if matches!(byte(at), b'-' | b'+') {
        at += 1;
    }
    if byte(at) == b'0' && matches!(byte(at + 1), b'x' | b'X') && byte(at + 2).is_ascii_hexdigit() {
        at += 2;
    }
    let mut value: u32 = 0;
    let mut overflow = false;
    while let Some(d) = char::from(byte(at)).to_digit(16) {
        match value.checked_mul(16).and_then(|v| v.checked_add(d)) {
            Some(v) => value = v,
            None => overflow = true,
        }
        at += 1;
    }
    if overflow {
        return u32::MAX;
    }
    if negative {
        value.wrapping_neg()
    } else {
        value
    }
}

/// `ReadProfileNumber`'s reading of a value: the leading run of
/// `0123456789truefalsynoxTRUEFALSYNOX+-`, then `true`/`yes` (1),
/// `false`/`no` (0), hex when it holds an `x`, else `atoi`.
#[must_use]
pub fn profile_number(value: &[u8]) -> u32 {
    const SPAN: &[u8] = b"0123456789truefalsynoxTRUEFALSYNOX+-";
    let value = value.split(|&b| b == 0).next().unwrap_or_default();
    let span = value.iter().take_while(|b| SPAN.contains(b)).count();
    let text = if span != 0 { &value[..span] } else { value };
    let is = |word: &[u8]| text.eq_ignore_ascii_case(word);
    if is(b"true") || is(b"yes") {
        1
    } else if is(b"false") || is(b"no") {
        0
    } else if text.iter().any(|&b| b == b'x' || b == b'X') {
        strtoul_hex(text)
    } else {
        msvcrt::atoi(text).cast_unsigned()
    }
}

/// The values the device starts from.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Defaults {
    /// By [`RenderStateId::index`].
    pub render: [u32; RENDER_STATES],
    /// By [`PipelineProperty::raw`]; slot 0xE is never enabled.
    pub pipeline: [Option<u32>; super::state::PIPELINE_SLOTS],
    /// `LOCKABLE_BACKBUFFER`: whether `read_back_buffer` may succeed.
    pub lockable_back_buffer: bool,
}

impl Default for Defaults {
    fn default() -> Self {
        Self::builtin()
    }
}

impl Defaults {
    /// The built-in tables, as before `Startup` reads the INI.
    #[must_use]
    pub fn builtin() -> Defaults {
        let mut render = [0; RENDER_STATES];
        for id in RenderStateId::ALL {
            render[id.index()] = id.builtin_default();
        }
        let mut pipeline = [None; super::state::PIPELINE_SLOTS];
        for p in PipelineProperty::ALL {
            pipeline[p.raw() as usize] = Some(p.builtin_default());
        }
        Defaults {
            render,
            pipeline,
            lockable_back_buffer: false,
        }
    }

    /// The built-in tables overridden by the keys of `section`, as
    /// `Startup` reads them. Hardware-quirk pipeline keys are ignored
    /// (decision T22, divergence D202). Without a section, the built-in
    /// values.
    #[must_use]
    pub fn from_section(section: Option<&EngineSection>) -> Defaults {
        let mut d = Self::builtin();
        let Some(section) = section else {
            return d;
        };
        let read = |key: &str, default: u32| {
            section
                .value(key.as_bytes())
                .map_or(default, profile_number)
        };
        for p in PipelineProperty::ALL {
            if !p.is_hardware_quirk() {
                d.pipeline[p.raw() as usize] = Some(read(p.name(), p.builtin_default()));
            }
        }
        for id in RenderStateId::ALL {
            d.render[id.index()] = read(id.name(), id.builtin_default());
        }
        d.lockable_back_buffer = read("LOCKABLE_BACKBUFFER", 0) != 0;
        d
    }

    /// The default of a render state.
    #[must_use]
    pub fn render_state(&self, id: RenderStateId) -> u32 {
        self.render[id.index()]
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use neuron_core::ini::{EngineIni, NoIncludes};

    #[test]
    fn reads_numbers_as_read_profile_number() {
        assert_eq!(profile_number(b"true"), 1);
        assert_eq!(profile_number(b"TRUE\t\t\t; comment"), 1);
        assert_eq!(profile_number(b"Yes"), 1);
        assert_eq!(profile_number(b"no"), 0);
        assert_eq!(profile_number(b"5 \t; D3DCMP_GREATER"), 5);
        assert_eq!(profile_number(b"0x3F800000"), 0x3F80_0000);
        assert_eq!(profile_number(b"-1"), u32::MAX);
        assert_eq!(profile_number(b"0xFFFFFFFFF"), u32::MAX);
        assert_eq!(profile_number(b"abc"), 0);
        assert_eq!(profile_number(b""), 0);
        // "tru" is a span of the set but no word: atoi reads 0.
        assert_eq!(profile_number(b"tru"), 0);
    }

    #[test]
    fn the_section_overrides_the_table() {
        let ini = b"[RenderPipeline]\nALPHAREF = 0\nALPHATESTENABLE = true\nALPHAFUNC = 5\n\
                    LOCALVIEWER = false\nzfunc = 4\nTEXTURE_ALLOW_DXT = false\nCLEAR_COLOR = 0x00ff00\n";
        let mut engine = EngineIni::new();
        engine.load_buffer(ini, &mut NoIncludes).unwrap();
        let d = Defaults::from_section(engine.section(b"RenderPipeline"));
        assert_eq!(d.render_state(RenderStateId::AlphaTestEnable), 1);
        assert_eq!(d.render_state(RenderStateId::AlphaFunc), 5);
        assert_eq!(d.render_state(RenderStateId::ZFunc), 4);
        assert_eq!(d.render_state(RenderStateId::ZEnable), 1);
        assert_eq!(d.render_state(RenderStateId::StencilMask), u32::MAX);
        assert_eq!(
            d.pipeline[PipelineProperty::ClearColor.raw() as usize],
            Some(0xff00)
        );
        // A quirk key keeps the built-in value.
        assert_eq!(
            d.pipeline[PipelineProperty::TextureAllowDxt.raw() as usize],
            Some(1)
        );
        assert_eq!(d.pipeline[0xE], None);
        assert!(!d.lockable_back_buffer);
    }
}
