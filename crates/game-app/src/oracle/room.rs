//! `layout.jsonl`'s records of a room's file (`OracleOnRoomLoaded`,
//! `oracle_mode.cpp`): after `RoomData::load` in `LoadLocationBase`, a
//! `room` head with the room's own fields and counts, then a record per
//! camera, light, prop, setpoint, character and ship placement, hotspot,
//! flashlight set and scene, in that order.

use super::json::OracleJson;
use crate::base::room_data::{Frame, NamedLight, RoomData};

/// The room as `BaseData` names it (`RoomData::init`).
#[derive(Clone, Copy, Debug)]
pub struct RoomName<'a> {
    pub room: u32,
    pub base: u32,
    pub nickname: Option<&'a [u8]>,
    pub file: Option<&'a [u8]>,
}

fn head(frame: u32, rec: &str) -> OracleJson {
    let mut j = OracleJson::new();
    j.uint("frame", frame).str("rec", rec);
    j
}

fn index(i: usize) -> u32 {
    u32::try_from(i).unwrap_or(u32::MAX)
}

fn floats(j: &mut OracleJson, name: &str, values: &[f32]) {
    j.begin_array(name);
    for &v in values {
        j.item_float(v);
    }
    j.end_array();
}

fn csys(j: &mut OracleJson, frame: &Frame) {
    floats(j, "csys", &frame.words());
}

fn text(j: &mut OracleJson, name: &str, value: Option<&[u8]>) {
    match value {
        Some(v) => j.str_bytes(name, v),
        None => j.null(name),
    };
}

fn item_text(j: &mut OracleJson, value: Option<&[u8]>) {
    match value {
        Some(v) => j.item_str_bytes(v),
        None => j.item_null(),
    };
}

fn named_light(j: &mut OracleJson, name: &str, l: &NamedLight) {
    j.begin_object(name)
        .bool("on", l.on)
        .bool("dir_on", l.world_dir);
    floats(j, "pos", &l.position);
    floats(j, "dir", &l.direction);
    floats(j, "color", &l.color);
    j.end_object();
}

fn record(frame: u32, rec: &str, room: u32, i: usize) -> OracleJson {
    let mut j = head(frame, rec);
    j.uint("room", room).uint("i", index(i));
    j
}

/// The lines `OracleOnRoomLoaded` writes at `frame` for `room`.
#[must_use]
#[allow(clippy::too_many_lines)]
pub fn room_lines(data: &RoomData, name: RoomName<'_>, frame: u32) -> Vec<String> {
    let mut lines = Vec::new();
    let mut j = head(frame, "room");
    j.uint("room", name.room).uint("base", name.base);
    text(&mut j, "nickname", name.nickname);
    text(&mut j, "file", name.file);
    j.begin_array("animations");
    for (anim, looping) in data.animations.iter().take(0x20) {
        j.begin_object_item()
            .str_bytes("name", anim)
            .bool("loop", *looping)
            .end_object();
    }
    j.end_array();
    text(&mut j, "goodscart", data.goodscart_script.as_deref());
    j.uint("goodscart_priority", data.goodscart_priority)
        .bool("cutoff_on", data.cutoff_shadow.is_some())
        .float("cutoff", data.cutoff_shadow.unwrap_or(0.0))
        .float("extra_shadow", data.extra_shadow_height)
        .uint("music", data.music)
        .bool("music_loop", data.music_loop)
        .uint("ambient", data.ambient)
        .bool("ambient_loop", data.ambient_loop);
    floats(&mut j, "ambient_color", &data.ambient_color);
    named_light(&mut j, "shadow", &data.shadow_light);
    named_light(&mut j, "glare", &data.glare_light);
    text(&mut j, "camera", data.camera_name.as_deref());
    j.uint("cameras", index(data.cameras.len()))
        .uint("lights", index(data.lights.len()))
        .uint("props", index(data.props.len()))
        .uint("setpoints", index(data.setpoints.len()))
        .uint("chars", index(data.characters.len()))
        .uint("ships", index(data.ships.len()))
        .uint("hotspots", index(data.hotspots.len()))
        .uint("flashlights", index(data.flashlights.len()))
        .uint("scenes", index(data.scenes.len()));
    lines.push(j.finish());

    let room = name.room;
    for (i, c) in data.cameras.iter().enumerate() {
        let mut j = record(frame, "roomcam", room, i);
        text(&mut j, "name", c.name.as_deref());
        csys(&mut j, &c.frame);
        j.float("fov", c.fov)
            .float("near", c.near)
            .float("far", c.far);
        lines.push(j.finish());
    }
    for (i, l) in data.lights.iter().enumerate() {
        let mut j = record(frame, "roomlight", room, i);
        csys(&mut j, &l.frame);
        floats(&mut j, "dir", &l.direction);
        let bytes: Vec<u8> = l.d3d.iter().flat_map(|w| w.to_le_bytes()).collect();
        j.bytes("d3d", &bytes)
            .uint("type", u32::from(l.light_group));
        lines.push(j.finish());
    }
    for (i, p) in data.props.iter().enumerate() {
        let mut j = record(frame, "roomprop", room, i);
        text(&mut j, "template", p.template.as_deref());
        text(&mut j, "name", p.name.as_deref());
        j.int("category", p.category);
        csys(&mut j, &p.frame);
        j.bool("visible", p.visible)
            .bool("lit", p.lit)
            .bool("fog", p.fog)
            .bool("main", p.room_main)
            .uint("light_group", u32::from(p.light_group))
            .uint("layer", u32::from(p.layer))
            .uint("render", p.render_type)
            .str_bytes("loadout", &p.loadout)
            .uint("priority", p.priority)
            .bool("running_lights", p.running_lights);
        lines.push(j.finish());
    }
    for (i, s) in data.setpoints.iter().enumerate() {
        let mut j = record(frame, "roomsetpoint", room, i);
        text(&mut j, "name", s.name.as_deref());
        csys(&mut j, &s.frame);
        let p = &s.props;
        j.bool("valid", p.valid)
            .bool("zs", p.zone_script)
            .int("char_type", p.char_type);
        text(&mut j, "segment", p.segment.as_deref());
        j.int("zone", p.zone)
            .str_bytes("pname", &p.name)
            .int("posture", p.posture);
        lines.push(j.finish());
    }
    for (i, c) in data.characters.iter().enumerate() {
        let mut j = record(frame, "roomchar", room, i);
        text(&mut j, "name", c.name.as_deref());
        csys(&mut j, &c.frame);
        j.int("char_type", c.char_type)
            .int("posture", c.posture)
            .bool("zs", c.zone_script);
        text(&mut j, "start_script", c.start_script.as_deref());
        lines.push(j.finish());
    }
    for (i, s) in data.ships.iter().enumerate() {
        let mut j = record(frame, "roomship", room, i);
        text(&mut j, "name", s.nickname.as_deref());
        csys(&mut j, &s.frame);
        j.bool("player", s.player_ship).bool("for_sale", s.for_sale);
        text(&mut j, "landing", s.landing_script.as_deref());
        text(&mut j, "launching", s.launching_script.as_deref());
        lines.push(j.finish());
    }
    for (i, h) in data.hotspots.iter().enumerate() {
        let mut j = record(frame, "roomhotspot", room, i);
        text(&mut j, "name", h.name.as_deref());
        text(&mut j, "room_switch", h.room_switch.as_deref());
        text(&mut j, "virtual_room", h.virtual_room.as_deref());
        text(&mut j, "set_virtual_room", h.set_virtual_room.as_deref());
        j.int("behavior", h.behavior)
            .uint("state_send", h.state_send)
            .uint("state_read", h.state_read)
            .begin_array("transitions");
        for t in &h.state_transitions {
            item_text(&mut j, t.as_deref());
        }
        j.end_array();
        lines.push(j.finish());
    }
    for (i, f) in data.flashlights.iter().enumerate() {
        let mut j = record(frame, "roomflash", room, i);
        floats(&mut j, "params", &f.params);
        let count = f.hardpoints.len();
        j.int("mode", f.mode)
            .int("numlights", i32::try_from(count).unwrap_or(i32::MAX))
            .int("numlights_alt", f.num_lights_alt)
            .begin_array("hardpoints");
        for h in f.hardpoints.iter().take(64) {
            item_text(&mut j, h.as_deref());
        }
        j.end_array().begin_array("setpoints");
        for s in f.setpoints.iter().take(64) {
            item_text(&mut j, s.as_deref());
        }
        j.end_array();
        lines.push(j.finish());
    }
    for (i, s) in data.scenes.iter().enumerate() {
        let mut j = record(frame, "roomscene", room, i);
        j.int("type", s.kind)
            .str_bytes("path", &s.path)
            .uint("priority", s.priority);
        lines.push(j.finish());
    }
    lines
}
