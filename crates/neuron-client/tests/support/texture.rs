//! P4.4: the texture library at run time and the large-image tiler, as
//! `CppSource/Oracle/oracle_texture.cpp` and `oracle_big_image.cpp` write
//! them (line formats at the top of those files), over
//! `tests/fixtures/oracle/{texture_runtime,big_image}.list`.
//!
//! The renderer is [`ContractPipeline`], which answers as the harness's
//! recording renderer (`oracle_texture.h`) does; its calls become lines
//! in the order they were made.
//!
//! The cases must keep to what the reference defines; the runtime suite
//! panics where a case would make the reference read a destroyed record
//! or a frame past its table.

use neuron_client::render::texture::big_image::{BigImage, BigImageDesc, Rect};
use neuron_client::render::texture::pipeline::{Abilities, ContractPipeline, PipelineCall};
use neuron_client::render::texture::playback::FrameOut;
use neuron_client::render::texture::{LibraryConfig, RecordId, TextureLibrary};
use neuron_core::image::{FormatMaps, PixelFormat, TextureFrame, TextureRecord};

use super::{hex, input_lines};

const UNTOUCHED: u32 = 0xCDCD_CDCD;

fn fnv64(bytes: &[u8]) -> String {
    let hash = bytes.iter().fold(0xCBF2_9CE4_8422_2325_u64, |hash, &b| {
        (hash ^ u64::from(b)).wrapping_mul(0x0000_0100_0000_01B3)
    });
    format!("{hash:016x}")
}

fn int(text: &str) -> i32 {
    text.parse::<i64>()
        .unwrap_or_else(|e| panic!("{text}: {e}")) as i32
}

fn bits(text: &str) -> f32 {
    f32::from_bits(u32::from_str_radix(text, 16).unwrap_or_else(|e| panic!("{text}: {e}")))
}

/// The renderer calls made since `from`, as the harness writes them.
fn call_lines(case: usize, pipeline: &ContractPipeline, from: usize, lines: &mut Vec<String>) {
    for call in &pipeline.calls[from..] {
        let fields = match call {
            PipelineCall::Create {
                width,
                height,
                format,
                levels,
                handle,
                result,
            } => format!(
                "\"ev\":\"create\",\"w\":\"{width}\",\"h\":\"{height}\",\"fmt\":\"{}\",\"levels\":\"{levels}\",\"handle\":\"{handle}\",\"result\":\"{result}\"",
                format.0
            ),
            PipelineCall::Level {
                handle,
                level,
                width,
                height,
                pitch,
                format,
                bytes,
                result,
            } => format!(
                "\"ev\":\"level\",\"handle\":\"{handle}\",\"level\":\"{level}\",\"w\":\"{width}\",\"h\":\"{height}\",\"pitch\":\"{pitch}\",\"fmt\":\"{}\",\"got\":\"{}\",\"fnv\":\"{}\",\"result\":\"{result}\"",
                format.0,
                bytes.len(),
                fnv64(bytes)
            ),
            PipelineCall::Cube { handle, result } => {
                format!("\"ev\":\"cube\",\"handle\":\"{handle}\",\"result\":\"{result}\"")
            }
            PipelineCall::Destroy { handle } => {
                format!("\"ev\":\"destroy\",\"handle\":\"{handle}\"")
            }
        };
        lines.push(format!("{{\"case\":\"{case}\",{fields}}}"));
    }
}

fn frame_fields(out: &FrameOut) -> String {
    format!(
        ",\"h\":\"{}\",\"uv\":\"{:08x},{:08x},{:08x},{:08x}\"",
        out.handle, out.uv[0], out.uv[1], out.uv[2], out.uv[3]
    )
}

const UNTOUCHED_FRAME: FrameOut = FrameOut {
    handle: UNTOUCHED,
    uv: [UNTOUCHED; 4],
};

struct RuntimeCase<'a> {
    case: usize,
    lib: TextureLibrary,
    pipe: ContractPipeline,
    lines: &'a mut Vec<String>,
}

impl RuntimeCase<'_> {
    fn rec(&self, name: &str) -> Option<RecordId> {
        if name == "-" {
            return None;
        }
        let record = self.lib.find(name.as_bytes())?;
        assert!(
            self.lib.record(record).is_some_and(|r| r.alive),
            "case {}: {name} names a destroyed record",
            self.case
        );
        Some(record)
    }

    fn name(&self, record: Option<RecordId>) -> String {
        match record {
            None => "-".to_owned(),
            Some(r) => {
                let record = self.lib.record(r).expect("a record");
                assert!(
                    record.alive,
                    "case {}: a destroyed record is read",
                    self.case
                );
                hex(&record.name)
            }
        }
    }

    fn line(&mut self, op: usize, result: i32, extra: &str) {
        self.lines.push(format!(
            "{{\"case\":\"{}\",\"op\":\"{op}\",\"r\":\"{result}\"{extra}}}",
            self.case
        ));
    }

    fn dump(&mut self) {
        let mut out = Vec::new();
        for (_, r) in self.lib.loader().canonical() {
            let record = self.lib.record(r).expect("indexed");
            let frames: Vec<String> = record
                .frames
                .iter()
                .map(|f| {
                    format!(
                        "{}:{:08x},{:08x},{:08x},{:08x}",
                        f.child, f.uv[0], f.uv[1], f.uv[2], f.uv[3]
                    )
                })
                .collect();
            let children: Vec<String> = record.children.iter().map(|c| self.name(*c)).collect();
            out.push(format!(
                "{{\"case\":\"{}\",\"rec\":\"{}\",\"handle\":\"{}\",\"refs\":\"{}\",\"rate\":\"{:08x}\",\"frames\":\"{}\",\"children\":\"{}\"}}",
                self.case,
                self.name(Some(r)),
                record.handle,
                record.ref_count,
                record.tile_rate,
                frames.join("|"),
                children.join(",")
            ));
        }
        for (id, p) in self.lib.playbacks() {
            out.push(format!(
                "{{\"case\":\"{}\",\"pb\":\"{id}\",\"src\":\"{}\",\"cur\":\"{}\",\"count\":\"{}\",\"el\":\"{:016x}\",\"dur\":\"{:016x}\",\"dir\":\"{:08x}\",\"wrap\":\"{}\",\"refs\":\"{}\",\"local\":\"{:08x}\",\"use\":\"{}\"}}",
                self.case,
                self.name(Some(p.source)),
                p.current_frame,
                p.frame_count,
                p.elapsed.to_bits(),
                p.duration.to_bits(),
                p.direction.to_bits(),
                p.wrap,
                p.ref_count,
                p.local_rate.to_bits(),
                u8::from(p.use_local_rate)
            ));
        }
        let active: Vec<String> = self.lib.active().map(|id| id.to_string()).collect();
        out.push(format!(
            "{{\"case\":\"{}\",\"active\":\"{}\"}}",
            self.case,
            active.join(",")
        ));
        for (id, g) in self.lib.groups() {
            let members: Vec<String> = g.members.iter().map(ToString::to_string).collect();
            out.push(format!(
                "{{\"case\":\"{}\",\"group\":\"{id}\",\"name\":\"{}\",\"members\":\"{}\"}}",
                self.case,
                g.name.as_deref().map_or("-".to_owned(), hex),
                members.join(",")
            ));
        }
        self.lines.extend(out);
    }

    /// Asserts that reading frame `frame` of a playback stays inside the
    /// frame table and its children (`ReadPlaybackFrame`).
    fn check_frame_read(&self, id: i32, frame: i32) {
        let Some(p) = self.lib.playback(id) else {
            return;
        };
        let record = self.lib.record(p.source).expect("a source");
        let count = record.frames.len() as u32;
        if count == 0 {
            return;
        }
        let selected = match frame.cast_unsigned() {
            u32::MAX if count <= 1 => 0,
            u32::MAX => p.current_frame,
            f if f >= count => count - 1,
            f => f,
        };
        assert!(
            selected < count,
            "case {}: frame {selected} of {count} is read",
            self.case
        );
        let child = record.frames[selected as usize].child as usize;
        let child = record.children.get(child).copied().flatten();
        assert!(
            child.is_some_and(|c| self.lib.record(c).is_some_and(|r| r.alive)),
            "case {}: a missing child is read",
            self.case
        );
    }

    #[allow(clippy::too_many_lines)]
    fn run(&mut self, op: usize, f: &[&str]) {
        let start = self.pipe.calls.len();
        let (result, extra): (i32, String) = match f[0] {
            "tex" => {
                self.lib
                    .create_record(int(f[2]).cast_unsigned(), f[1].as_bytes(), false);
                (0, String::new())
            }
            "anim" => {
                let children = f[3].split(',').map(|c| self.rec(c)).collect();
                let frames = f[4]
                    .split('|')
                    .map(|frame| {
                        let v: Vec<&str> = frame.split(':').collect();
                        TextureFrame {
                            child: int(v[0]).cast_unsigned(),
                            uv: [1, 2, 3, 4].map(|i| bits(v[i]).to_bits()),
                        }
                    })
                    .collect();
                self.lib.insert_record(TextureRecord {
                    name: f[1].as_bytes().to_vec(),
                    handle: 0,
                    ref_count: 0,
                    frames,
                    children,
                    tile_rate: bits(f[2]).to_bits(),
                    alive: true,
                });
                (0, String::new())
            }
            "named" => (
                self.lib
                    .find_or_create_by_name(f[1].as_bytes(), true)
                    .map_or_else(|e| e, |_| 0),
                String::new(),
            ),
            "find" => (
                self.lib
                    .find_texture(f[1].as_bytes(), int(f[2]) != 0)
                    .map_or_else(|e| e, |_| 0),
                String::new(),
            ),
            "play" => {
                let record = self.rec(f[1]);
                match self.lib.create_playback(record, true) {
                    Ok(id) => (0, format!(",\"id\":\"{}\"", id.unwrap_or(-1))),
                    Err(e) => (e, ",\"id\":\"-1\"".to_owned()),
                }
            }
            "playnull" => {
                let record = self.rec(f[1]);
                (
                    self.lib
                        .create_playback(record, false)
                        .map_or_else(|e| e, |_| 0),
                    String::new(),
                )
            }
            "release" => {
                let record = self.rec(f[1]);
                (
                    self.lib.release_record(record, &mut self.pipe),
                    String::new(),
                )
            }
            "addref" => (self.lib.add_ref_id(int(f[1])), String::new()),
            "decref" => (
                self.lib.dec_ref_id(int(f[1]), &mut self.pipe),
                String::new(),
            ),
            "setel" => (self.lib.set_elapsed(int(f[1]), bits(f[2])), String::new()),
            "getel" | "getrate" => {
                let got = if f[0] == "getel" {
                    self.lib.elapsed(int(f[1]))
                } else {
                    self.lib.effective_rate(int(f[1]))
                };
                let (r, v) = got.map_or((-1, UNTOUCHED), |v| (0, v.to_bits()));
                (r, format!(",\"v\":\"{v:08x}\""))
            }
            "setrate" => (
                self.lib.set_local_rate(int(f[1]), bits(f[2])),
                String::new(),
            ),
            "setwrap" => (self.lib.set_wrap(int(f[1]), int(f[2])), String::new()),
            "getwrap" => {
                let (r, v) = self.lib.wrap(int(f[1])).map_or((-1, -7), |v| (0, v));
                (r, format!(",\"v\":\"{v}\""))
            }
            "setidx" => (
                self.lib
                    .set_frame_index(int(f[1]), f[2].parse().expect("an index")),
                String::new(),
            ),
            "getidx" => {
                let (r, v) = self
                    .lib
                    .frame_index_of(int(f[1]))
                    .map_or((-1, UNTOUCHED), |v| (0, v));
                (r, format!(",\"v\":\"{v}\""))
            }
            "adv" => (self.lib.advance(int(f[1]), bits(f[2])), String::new()),
            "tick" => (self.lib.tick_all(int(f[1]), bits(f[2])), String::new()),
            "frame" => {
                self.check_frame_read(int(f[1]), int(f[2]));
                let (r, out) = match self.lib.frame(int(f[1]), int(f[2])) {
                    Ok(out) => (0, out),
                    Err(e) => (e, UNTOUCHED_FRAME),
                };
                (r, frame_fields(&out))
            }
            "tframe" => {
                let record = self.rec(f[1]);
                let (r, out) = self.lib.texture_frame(record, int(f[2]));
                (r, frame_fields(&out))
            }
            "fhandle" => {
                let record = self.rec(f[1]);
                (
                    self.lib
                        .set_frame_handle(record, int(f[2]), int(f[3]).cast_unsigned()),
                    String::new(),
                )
            }
            "frect" => {
                let record = self.rec(f[1]);
                let uv = [bits(f[3]), bits(f[4]), bits(f[5]), bits(f[6])];
                (
                    self.lib.set_frame_rect(record, int(f[2]), uv),
                    String::new(),
                )
            }
            "settile" => {
                let record = self.rec(f[1]);
                (self.lib.set_tile_rate(record, bits(f[2])), String::new())
            }
            "gettile" => {
                let record = self.rec(f[1]);
                let (r, v) = self.lib.tile_rate(record);
                (r, format!(",\"v\":\"{:08x}\"", v.to_bits()))
            }
            "count" => {
                let record = self.rec(f[1]);
                let (r, v) = self.lib.frame_count(record);
                (r, format!(",\"v\":\"{v}\""))
            }
            "sub" => {
                let record = self.rec(f[1]);
                (
                    self.lib.set_sub_texture(
                        record,
                        int(f[2]),
                        int(f[3]).cast_unsigned(),
                        &mut self.pipe,
                    ),
                    String::new(),
                )
            }
            "rename" => {
                let record = self.rec(f[1]);
                (
                    self.lib.set_record_name(record, f[2].as_bytes()),
                    String::new(),
                )
            }
            "copy" | "getname" => {
                let size = int(f[2]).cast_unsigned();
                let got = if f[0] == "copy" {
                    let record = self.rec(f[1]);
                    self.lib.copy_name(record, size)
                } else {
                    self.lib.group_name(int(f[1]), size)
                };
                let (r, text) = got.map_or((-1, Vec::new()), |t| (0, t));
                (r, format!(",\"text\":\"{}\"", hex(&text)))
            }
            "group" => {
                let id = self.lib.allocate_group();
                (0, format!(",\"id\":\"{id}\""))
            }
            "gadd" => {
                let record = self.rec(f[2]);
                (self.lib.add_sub_texture(int(f[1]), record), String::new())
            }
            "gdel" => {
                let record = self.rec(f[2]);
                (
                    self.lib
                        .remove_sub_texture(int(f[1]), record, &mut self.pipe),
                    String::new(),
                )
            }
            "gname" => {
                let name = (f[2] != "-").then(|| f[2].as_bytes());
                (self.lib.set_group_name(int(f[1]), name), String::new())
            }
            "gc" => (
                self.lib.garbage_collect(int(f[1]), &mut self.pipe),
                String::new(),
            ),
            "enumg" | "subcount" => {
                let got = if f[0] == "enumg" {
                    self.lib.enum_group(int(f[1]))
                } else {
                    self.lib.sub_count(int(f[1]))
                };
                let (r, v) = got.map_or((-1, -7), |v| (0, v));
                (r, format!(",\"v\":\"{v}\""))
            }
            "enumt" => {
                let (r, record) = self
                    .lib
                    .enum_texture(int(f[1]), int(f[2]), int(f[3]) != 0)
                    .map_or((-1, None), |r| (0, r));
                (r, format!(",\"rec\":\"{}\"", self.name(record)))
            }
            "resolve" => {
                let (r, record) = self
                    .lib
                    .resolve(int(f[1]))
                    .map_or((-1, None), |r| (0, Some(r)));
                (r, format!(",\"rec\":\"{}\"", self.name(record)))
            }
            "peek" => (0, format!(",\"v\":\"{}\"", self.lib.peek_next_group_id())),
            "slot" => (
                self.lib
                    .set_generic_slot(int(f[1]), int(f[2]).cast_unsigned()),
                String::new(),
            ),
            "getslot" => {
                let v = self.lib.generic_slot(int(f[1])).expect("a slot in range");
                (0, format!(",\"v\":\"{v}\""))
            }
            "pick" => {
                let got = self
                    .lib
                    .pick_gpu_format(PixelFormat(int(f[1]).cast_unsigned()));
                let (r, v) = got.map_or((-1, u32::MAX), |p| (0, p.0));
                (r, format!(",\"v\":\"{v}\""))
            }
            "fmt" => {
                let record = self.rec(f[1]);
                let (r, v) = self.lib.group_pixel_format(record, &self.pipe);
                (r, format!(",\"v\":\"{}\"", v.0))
            }
            "dump" => {
                self.dump();
                return;
            }
            other => panic!("texture_runtime: unknown operation {other}"),
        };
        call_lines(self.case, &self.pipe, start, self.lines);
        self.line(op, result, &extra);
    }
}

/// `texture_runtime.jsonl`.
pub fn runtime_lines() -> Vec<String> {
    let mut lines = Vec::new();
    for (case, text) in input_lines("texture_runtime.list").iter().enumerate() {
        let mut runner = RuntimeCase {
            case,
            lib: TextureLibrary::new(LibraryConfig::default()),
            pipe: ContractPipeline::new(Abilities {
                square_only: 0,
                max_width: 256,
                max_height: 256,
            }),
            lines: &mut lines,
        };
        for (op, text) in text.split(" ; ").enumerate() {
            let fields: Vec<&str> = text.split(' ').collect();
            runner.run(op, &fields);
        }
    }
    lines
}

fn pattern(len: usize, step: usize, start: usize) -> Vec<u8> {
    (0..len)
        .map(|i| ((i * step + start) & 0xFF) as u8)
        .collect()
}

/// `big_image.jsonl`.
pub fn big_image_lines() -> Vec<String> {
    let mut lines = Vec::new();
    let maps = FormatMaps::defaults();
    for (case, text) in input_lines("big_image.list").iter().enumerate() {
        let ops: Vec<&str> = text.split(" ; ").collect();
        let i: Vec<&str> = ops[0].split(' ').collect();
        let mut pipe = ContractPipeline::new(Abilities {
            square_only: int(i[1]).cast_unsigned(),
            max_width: int(i[2]).cast_unsigned(),
            max_height: int(i[3]).cast_unsigned(),
        });
        let pixels = pattern(int(i[9]) as usize, 31, 7);
        let desc = BigImageDesc {
            src_format: PixelFormat(int(i[4]).cast_unsigned()),
            dst_format: PixelFormat(int(i[5]).cast_unsigned()),
            pixels: &pixels,
            width: int(i[6]).cast_unsigned(),
            height: int(i[7]).cast_unsigned(),
            pitch: int(i[8]).cast_unsigned(),
        };
        let created = BigImage::create(&desc, &maps, &mut pipe);
        call_lines(case, &pipe, 0, &mut lines);
        let line = |op: usize, r: i64, extra: &str| {
            format!("{{\"case\":\"{case}\",\"op\":\"{op}\",\"r\":\"{r}\"{extra}}}")
        };
        let mut image = match created {
            Ok(image) => {
                lines.push(line(0, 0, ""));
                image
            }
            Err(e) => {
                lines.push(line(0, i64::from(e), ""));
                continue;
            }
        };
        for (op, text) in ops.iter().enumerate().skip(1) {
            let f: Vec<&str> = text.split(' ').collect();
            let start = pipe.calls.len();
            let rect = |a: &[&str]| Rect {
                left: int(a[0]),
                top: int(a[1]),
                right: int(a[2]),
                bottom: int(a[3]),
            };
            let (r, extra): (i64, String) = match f[0] {
                "draw" => {
                    let (src, dest) = if f[1] == "-" {
                        (None, rect(&f[2..6]))
                    } else {
                        (Some(rect(&f[1..5])), rect(&f[5..9]))
                    };
                    let (r, draws) = image.draw(src, dest);
                    for (k, d) in draws.iter().enumerate() {
                        let uv: Vec<String> =
                            d.uv.iter()
                                .map(|v| format!("{:08x}", v.to_bits()))
                                .collect();
                        lines.push(format!(
                            "{{\"case\":\"{case}\",\"tile\":\"{k}\",\"h\":\"{}\",\"color\":\"{:08x}\",\"dest\":\"{:08x},{:08x},{:08x},{:08x}\",\"uv\":\"{}\"}}",
                            d.handle,
                            d.color,
                            d.dest[0].to_bits(),
                            d.dest[1].to_bits(),
                            d.dest[2].to_bits(),
                            d.dest[3].to_bits(),
                            uv.join(",")
                        ));
                    }
                    (i64::from(r), String::new())
                }
                "alpha" => (i64::from(image.set_alpha(bits(f[1]))), String::new()),
                "color" => {
                    image.set_color([bits(f[1]), bits(f[2]), bits(f[3])]);
                    (0, String::new())
                }
                "fill" => {
                    let (w, h, pitch) = (
                        int(f[2]).cast_unsigned(),
                        int(f[3]).cast_unsigned(),
                        int(f[4]).cast_unsigned(),
                    );
                    let fill = pattern((pitch * h) as usize, 17, 3);
                    let r = image.set_from_buffer(
                        &fill,
                        PixelFormat(int(f[1]).cast_unsigned()),
                        w,
                        h,
                        pitch,
                        &mut pipe,
                    );
                    (i64::from(r), String::new())
                }
                "tile" => {
                    let (r, h, rc) = match image.tile(int(f[1])) {
                        Ok((h, rc)) => (0, h, rc),
                        Err(e) => (
                            e,
                            UNTOUCHED,
                            Rect {
                                left: -7,
                                top: -7,
                                right: -7,
                                bottom: -7,
                            },
                        ),
                    };
                    (
                        i64::from(r),
                        format!(
                            ",\"h\":\"{h}\",\"rect\":\"{},{},{},{}\"",
                            rc.left, rc.top, rc.right, rc.bottom
                        ),
                    )
                }
                "size" => {
                    let (w, h) = image.size();
                    (i64::from(h.cast_signed()), format!(",\"v\":\"{w},{h}\""))
                }
                "count" => (i64::from(image.tile_count()), String::new()),
                other => panic!("big_image: unknown operation {other}"),
            };
            call_lines(case, &pipe, start, &mut lines);
            lines.push(line(op, r, &extra));
        }
        let start = pipe.calls.len();
        image.destroy(&mut pipe);
        call_lines(case, &pipe, start, &mut lines);
    }
    lines
}
