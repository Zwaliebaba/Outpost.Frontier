//! The lines of `CppSource/Oracle/oracle_effect_arch.cpp`'s `PrintTables`,
//! shared by the synthetic suite (`oracle_vectors.rs`) and the data dump
//! (`effects_data.rs`).

use std::fmt::Write as _;

use game_app::effect_arch::EffectArchDb;

pub fn hex(bytes: &[u8]) -> String {
    bytes.iter().fold(String::new(), |mut out, b| {
        let _ = write!(out, "{b:02x}");
        out
    })
}

fn f32_hex(v: f32) -> String {
    format!("{:08x}", v.to_bits())
}

fn hex_or_dash(text: Option<&[u8]>) -> String {
    text.map_or_else(|| "-".to_owned(), hex)
}

/// `oracle_effect_arch.cpp`'s tables at a case's end.
pub fn render(case: &str, db: &EffectArchDb, out: &mut Vec<String>) {
    let head = format!("{{\"case\":\"{case}\",");
    for (i, t) in db.types.iter().enumerate() {
        out.push(format!(
            "{head}\"type\":\"{i}\",\"v\":\"{},{},{},{},{},{},{},{},{},{}\"}}",
            f32_hex(t.priority),
            f32_hex(t.generic_priority),
            t.lod_type,
            f32_hex(t.radius),
            u8::from(t.visibility_offscreen),
            u8::from(t.update_offscreen),
            f32_hex(t.run_time),
            f32_hex(t.fps),
            f32_hex(t.pbubble_near),
            f32_hex(t.pbubble_far),
        ));
    }
    for (i, l) in db.lods.iter().enumerate() {
        out.push(format!(
            "{head}\"lod\":\"{i}\",\"v\":\"{},{},{}\"}}",
            f32_hex(l.max_lod_screen_size),
            f32_hex(l.min_lod_screen_size),
            f32_hex(l.min_screen_size),
        ));
    }
    for v in db.vis_effects.values() {
        let textures: Vec<String> = v.textures.iter().map(|t| hex(t)).collect();
        out.push(format!(
            "{head}\"vis\":\"{}\",\"v\":\"{:08x},{},{},{}\"}}",
            hex(&v.nickname),
            v.effect_crc,
            hex(&v.particlefx),
            textures.join(":"),
            u8::from(v.has_alpha),
        ));
    }
    for b in db.beams.values() {
        let floats: Vec<f32> = [
            b.tip_color.as_slice(),
            &b.tail_color,
            &b.core_color,
            &b.sec_core_color,
            &b.outer_color,
            &b.sec_outer_color,
            &[
                b.head_brightness,
                b.trail_brightness,
                b.tip_length,
                b.tail_length,
                b.core_length,
                b.core_width,
                b.sec_core_width,
                b.head_width,
                b.flash_size,
                b.pad_7c,
            ],
        ]
        .concat();
        let floats: String = floats.iter().map(|v| format!(",{}", f32_hex(*v))).collect();
        out.push(format!(
            "{head}\"beam\":\"{}\",\"v\":\"{},{},{}{floats}\"}}",
            hex(&b.nickname),
            b.segment_mode,
            b.uv_mode_v,
            b.uv_mode_u,
        ));
    }
    for (id, e) in &db.effects {
        // The parts by their current nicknames, as the C++ reads them through
        // its pointers.
        let vis = |n: &Option<Vec<u8>>| {
            n.as_deref()
                .and_then(|n| db.vis_effect(n))
                .map(|v| v.nickname.as_slice())
        };
        let beam = e
            .vis_beam
            .as_deref()
            .and_then(|n| db.beam(n))
            .map(|b| b.nickname.as_slice());
        let color = u32::from_le_bytes(e.lgt_color);
        out.push(format!(
            "{head}\"effect\":\"{id:08x}\",\"v\":\"{},{},{},{},{},{},{},{},{},{color:08x},{}\"}}",
            e.effect_type,
            hex_or_dash(vis(&e.vis_effect)),
            hex_or_dash(vis(&e.vis_generic)),
            hex_or_dash(beam),
            u8::from(e.snd_effect.is_some()),
            hex(&e.lgt_effect),
            f32_hex(e.lgt_range_scale),
            f32_hex(e.lgt_time_scale),
            f32_hex(e.lgt_radius),
            e.flags,
        ));
    }
}
