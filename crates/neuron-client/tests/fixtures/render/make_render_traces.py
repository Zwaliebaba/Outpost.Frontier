#!/usr/bin/env python3
"""Write the synthetic renderer traces of decision record 0004, point 15.

Each trace is a small scene in trace format version 1, written line by line
as RenderTrace.dll writes it (CppSource/RenderTrace/README.md "Trace format",
RenderTraceCore.cpp, RenderTrace.cpp and the generated proxy): the header,
the lifecycle of the real client in frame 0 (Initialize, Startup,
CreateBuffers with a 64x64 windowed back buffer, the defaults line, the
gamma calls, VbMgrInit) with the resources the scene uses, then the one
selected frame 1 with its snapshot, the clear, the states, the draws,
SwapBuffers, the frame image line and the end markers.

Outputs, per trace, under crates/neuron-client/tests/vectors/render/<name>/:

    trace.jsonl            the trace
    blobs/<hash>.bin       byte data above 64 bytes, named by FNV-1a 64

`expected.bmp` beside them is not written here: it is the C++ renderer's
image of the selected frame, made by `Oracle.exe render-replay` (record
0004, point 15) and committed from its output; it is never made from Rust.
The frame image line is therefore `"frame_image":null` with a note, the form
the proxy writes when it has no image.

The defaults line holds what DX8RENDERER's getters return after
CreateBuffers with an engine INI that sets no render state: the built-in
tables kRenderStateProperties and kPipelineProperties, read from
CppSource/DX8RENDERER/DX8RENDERER.cpp, with pipeline slot 6 replaced by the
stage count ApplyDeviceCaps stores there (MaxTextureBlendStages capped at 7;
7 on any device with eight blend stages).

    python make_render_traces.py           # rewrite the outputs
    python make_render_traces.py --check   # exit 1 when they are out of date

Deterministic: no dates, no randomness, fixed order.
"""

import os
import re
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", "..", "..", "..", ".."))
OUT = os.path.normpath(os.path.join(HERE, "..", "..", "vectors", "render"))
DX8RENDERER = os.path.join(ROOT, "CppSource", "DX8RENDERER", "DX8RENDERER.cpp")

WIDTH = 64
HEIGHT = 64
INLINE_BYTES = 64

# The stage count DX8RENDERER's ApplyDeviceCaps writes into pipeline slot 6.
PIPELINE_STAGE_COUNT = 7

# Direct3D 8 numbers.
D3DPT_TRIANGLELIST = 4
D3DPT_TRIANGLESTRIP = 5
D3DPT_TRIANGLEFAN = 6
FVF_XYZ = 0x2
FVF_XYZRHW = 0x4
FVF_NORMAL = 0x10
FVF_DIFFUSE = 0x40
FVF_TEX1 = 0x100
RS = {
    "ZWRITEENABLE": 0x0E,
    "ALPHATESTENABLE": 0x0F,
    "SRCBLEND": 0x13,
    "DESTBLEND": 0x14,
    "ALPHAREF": 0x18,
    "ALPHAFUNC": 0x19,
    "ALPHABLENDENABLE": 0x1B,
    "FOGENABLE": 0x1C,
    "FOGCOLOR": 0x22,
    "FOGTABLEMODE": 0x23,
    "FOGSTART": 0x24,
    "FOGEND": 0x25,
    "LIGHTING": 0x89,
    "AMBIENT": 0x8B,
    "FOGVERTEXMODE": 0x8C,
}
TSS = {"COLOROP": 1, "COLORARG1": 2, "COLORARG2": 3, "ALPHAOP": 4, "MAGFILTER": 0x10, "MINFILTER": 0x11}
D3DTOP_SELECTARG1 = 2
D3DTOP_MODULATE = 4
D3DTEXF_POINT = 1
D3DBLEND_SRCALPHA = 5
D3DBLEND_INVSRCALPHA = 6
D3DCMP_GREATER = 5
D3DFOG_NONE = 0
D3DFOG_LINEAR = 3
D3DLIGHT_DIRECTIONAL = 3
PF_A8R8G8B8 = 7

IDENTITY12 = [1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0]
IDENTITY16 = [1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0]


def fail(message):
    sys.stderr.write("make_render_traces.py: %s\n" % message)
    sys.exit(2)


def fbits(value):
    return struct.unpack("<I", struct.pack("<f", value))[0]


def s32(value):
    """A 32-bit pattern as the C++ int it is passed as."""
    value &= 0xFFFFFFFF
    return value - 0x100000000 if value & 0x80000000 else value


def fnv1a64(data):
    h = 0xCBF29CE484222325
    for b in data:
        h ^= b
        h = (h * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return "%016x" % h


def table(name):
    """(id, default) rows of a FDx8RendererPropertyInit table of DX8RENDERER.cpp."""
    with open(DX8RENDERER, "r", encoding="utf-8") as handle:
        text = handle.read()
    match = re.search(r"const FDx8RendererPropertyInit %s\[\] = \{(.*?)\n    \};" % name, text, re.S)
    if match is None:
        fail("table %s not found in %s" % (name, DX8RENDERER))
    rows = re.findall(r'\{(0x[0-9A-Fa-f]+), "(\w+)", (0x[0-9A-Fa-f]+), (\d)\}', match.group(1))
    return [(int(i, 16), int(d, 16)) for i, _, d, enabled in rows if enabled == "1"]


# --- JSON values written as the proxy writes them -------------------------------------


class Words(list):
    """32-bit words, each as 8 hex digits (floats by their bits)."""


class Hexf(float):
    """One float argument: its bits as 8 hex digits."""


class Data(bytes):
    """Byte data: inline hex up to 64 bytes, a blob above."""


def words_of_floats(values):
    return Words(fbits(v) for v in values)


class Trace:
    """The proxy's session (RenderTraceCore.cpp FSession) driven by the calls of one scene."""

    def __init__(self, name, frames):
        self.name = name
        self.lines = []
        self.blobs = {}
        self.seq = 0
        self.frame = 0
        self.frames = frames
        self.next_id = 1
        self.slots = {}
        self.slot_order = 0
        self.selection = None  # None, or ("vb", id)
        self.selection_ib = None  # None, or (id, base vertex)
        self.selection_seq = 0
        self.selection_frame = 0
        self.live_lights = {}
        self.pending_lights = {}
        self.closed = False
        self.lines.append('{"format":"outpost-render-trace","version":1,"renderer":"DirectX8",'
                          '"section":null,"frames":"%s"}' % ",".join(str(f) for f in frames))

    # Values.
    def value(self, v):
        if v is None:
            return "null"
        if isinstance(v, bool):
            return "true" if v else "false"
        if isinstance(v, Hexf):
            return '"%08x"' % fbits(v)
        if isinstance(v, Words):
            return "[" + ",".join('"%08x"' % (w & 0xFFFFFFFF) for w in v) + "]"
        if isinstance(v, Data):
            if len(v) <= INLINE_BYTES:
                return '{"size":%d,"hex":"%s"}' % (len(v), v.hex())
            name = fnv1a64(v)
            self.blobs[name] = bytes(v)
            return '{"size":%d,"blob":"%s"}' % (len(v), name)
        if isinstance(v, dict):
            return "{" + ",".join('"%s":%s' % (k, self.value(x)) for k, x in v.items()) + "}"
        if isinstance(v, int):
            return str(v)
        if isinstance(v, str):
            return '"' + v.replace("\\", "\\\\").replace('"', '\\"') + '"'
        fail("no JSON form for %r" % (v,))
        return ""

    def recorded(self):
        return self.frame in self.frames

    def new_id(self):
        result = self.next_id
        self.next_id += 1
        return result

    def store_slot(self, key, line):
        self.slot_order += 1
        self.slots[key] = (self.slot_order, line)

    def call(self, interface, method, args, ret=0, category="always", key=None, first_seen=None,
             synthetic=False):
        if self.closed:
            fail("%s: a call after the trace closed" % self.name)
        writes = category == "always" or self.recorded()
        if not writes and category != "state":
            fail("%s: %s is recorded in selected frames only" % (self.name, method))
        self.seq += 1
        line = '{"seq":%d,"frame":%d,"interface":"%s","method":"%s","args":%s' % (
            self.seq, self.frame, interface, method, self.value(args))
        if ret is not None:
            line += ',"ret":%d' % ret
        if first_seen:
            line += ',"first_seen":[%s]' % ",".join(str(i) for i in first_seen)
        if synthetic:
            line += ',"synthetic":true'
        line += "}"
        if writes:
            self.lines.append(line)
        if key is not None:
            self.store_slot(key, line)
        return line

    def reset_selection(self):
        self.selection = None
        self.selection_ib = None
        self.selection_seq = self.seq
        self.selection_frame = self.frame

    def select_vb(self, vb):
        self.selection = ("vb", vb)
        self.selection_seq = self.seq
        self.selection_frame = self.frame

    def select_ib(self, ib, base):
        self.selection_ib = (ib, base)
        self.selection_seq = self.seq
        self.selection_frame = self.frame

    # Frames.
    def begin_frame(self):
        if not self.recorded():
            return
        ordered = [line for _, line in sorted(self.slots.values())]
        count = len(ordered) + (2 if self.selection_ib is not None else 1)
        self.lines.append('{"frame":%d,"begin_frame":true,"seq":%d,"snapshot_lines":%d}'
                          % (self.frame, self.seq, count))
        prefix = '{"snapshot":%d,' % self.frame
        for line in ordered:
            self.lines.append(prefix + line[1:])
        head = '%s"seq":%d,"frame":%d,"synthetic":true,"interface":' % (
            prefix, self.selection_seq, self.selection_frame)
        vb = 0 if self.selection is None else self.selection[1]
        self.lines.append(head + '"IRPVertexBuffer","method":"SelectVb","args":{"Handle":%d}}' % vb)
        if self.selection_ib is not None:
            self.lines.append(head + '"IRPIndexBuffer","method":"SelectIb","args":{"Handle":%d,'
                              '"BaseVertexIndex":%d,"Unused3":0,"Unused4":0}}' % self.selection_ib)

    def swap_buffers(self):
        self.call("IRenderPipeline", "SwapBuffers", {})
        if self.recorded():
            self.lines.append('{"frame":%d,"frame_image":null,"note":"synthetic trace: the reference '
                              'image is expected.bmp beside this file (Oracle.exe render-replay)"}'
                              % self.frame)
            self.lines.append('{"frame":%d,"end_frame":true,"seq":%d}' % (self.frame, self.seq))
        if self.frame >= max(self.frames):
            self.lines.append('{"end":true,"frame":%d,"seq":%d}' % (self.frame, self.seq))
            self.closed = True
            return
        self.frame += 1
        self.begin_frame()

    # Lifecycle, as OutpostFrontier's engine_init.cpp runs it.
    def startup(self):
        self.call("IAggregateComponent", "Initialize", {})
        self.call("IRenderPipeline", "Startup", {"SectionName": None})

    def create_buffers(self):
        # DisplaySettings of a windowed mode after RenderMgrCreateBuffers: mode -1, refresh 60,
        # the size, colour and depth bits -1 (the adapter's), stencil 0, interval 0, two
        # buffers, border byte 1, windowed. OutMode is DX8RENDERER's mode blob: mode -1, the
        # size, 32-bit colour, D24X8 (24, 0), interval 0, one back buffer plus the front, COPY.
        mode = Words([0xFFFFFFFF, 60, WIDTH, HEIGHT, 0xFFFFFFFF, 0xFFFFFFFF, 0, 0, 2, 0x00000001])
        out = Words([0xFFFFFFFF, 0, WIDTH, HEIGHT, 32, 24, 0, 0, 2, 0x00000001])
        self.call("IRenderPipeline", "CreateBuffers", {"Hwnd": 0, "Mode": mode, "OutMode": out})
        # FSession::OnDeviceReset, then the defaults line.
        self.slots = {k: v for k, v in self.slots.items() if k.startswith("L:")}
        self.reset_selection()
        self.pending_lights = dict(self.live_lights)
        render = table("kRenderStateProperties")
        if len(render) != 76:
            fail("kRenderStateProperties has %d enabled rows, not 76" % len(render))
        pipeline = dict(table("kPipelineProperties"))
        pipeline[6] = PIPELINE_STAGE_COUNT
        line = '{"defaults":{"render":{%s},"pipeline":{%s}},"seq":%d,"frame":%d,"synthetic":true}' % (
            ",".join('"%d":%d' % (i, d) for i, d in render),
            ",".join('"%d":%d' % (i, s32(pipeline.get(i, 0))) for i in range(15)),
            self.seq, self.frame)
        self.lines.append(line)
        self.store_slot("DEFAULTS", line)
        # RenderMgrApplyGamma (windowed: gamma off, the curve at gamma 1) and VbMgrInit.
        self.call("IGammaControl", "SetGammaEnable", {"Enable": 0})
        self.call("IGammaControl", "SetGammaFunction", {"ChannelMask": 7, "Gamma": Hexf(1.0),
                                                         "Brightness": Hexf(0.0), "Contrast": Hexf(1.0),
                                                         "Bias": Hexf(0.0)})
        self.call("IVertexBufferManager", "VbMgrInit", {"A": 1, "B": 0x252, "C": 0xFC00, "D": 0})

    # Resources (always written).
    def create_texture(self, width, height, fmt, pixels):
        tex = self.new_id()
        self.call("IRenderPipeline", "CreateTexture", {"Width": width, "Height": height, "Format": fmt,
                                                       "Levels": 1, "Flags": 0, "OutHandle": tex})
        pitch = width * 4
        out = {"Width": width, "Height": height, "Pitch": pitch, "Format": fmt}
        self.call("IRenderPipeline", "LockTexture", {"Handle": tex, "Level": 0, "Out": out})
        self.call("IRenderPipeline", "UnlockTexture", {"Handle": tex, "Level": 0, "Width": width,
                                                       "Height": height, "Pitch": pitch, "Format": fmt,
                                                       "data": Data(pixels)})
        return tex

    def create_vb(self, fvf, stride, vertices):
        vb = self.new_id()
        count = len(vertices) // stride
        self.call("IRPVertexBuffer", "CreateVb", {"Fvf": fvf, "Count": count, "Out": vb, "Flags": 0})
        self.call("IRPVertexBuffer", "LockVb", {"Handle": vb, "OffsetIn": 0, "Offset": 0, "Count": count,
                                                "Stride": stride})
        self.call("IRPVertexBuffer", "UnlockVb", {"Handle": vb, "FirstVertex": 0, "Count": count,
                                                  "Stride": stride, "data": Data(vertices)})
        return vb

    def create_ib(self, indices):
        ib = self.new_id()
        count = len(indices)
        self.call("IRPIndexBuffer", "CreateIb", {"Count": count, "Out": ib, "Flags": 0})
        self.call("IRPIndexBuffer", "LockIb", {"Handle": ib, "OffsetIn": 0, "Offset": 0, "Count": count})
        self.call("IRPIndexBuffer", "UnlockIb", {"Handle": ib, "FirstIndex": 0, "Count": count,
                                                 "data": Data(struct.pack("<%dH" % count, *indices))})
        return ib

    # Frame calls.
    def set_pipeline_state(self, ident, value):
        self.call("IRenderPipeline", "SetPipelineState", {"Id": ident, "Value": s32(value)})

    def clear(self, flags, rect=None):
        self.call("IRenderPipeline", "ClearBuffers",
                  {"Flags": flags, "Rects": None if rect is None else Words(rect)}, category="frame")

    def begin_scene(self):
        self.call("IRenderPipeline", "BeginScene", {}, category="frame")

    def end_scene(self):
        self.call("IRenderPipeline", "EndScene", {}, category="frame")

    def render_state(self, name, value):
        state = RS[name]
        self.call("IRenderPipeline", "SetRenderState", {"State": state, "Value": s32(value)},
                  category="state", key="RS:%u" % state)

    def stage_state(self, stage, name, value):
        kind = TSS[name]
        self.call("IRenderPipeline", "SetTextureStageState", {"Stage": stage, "Type": kind, "Value": value},
                  category="state", key="TSS:%d:%d" % (stage, kind))

    def stage_texture(self, stage, tex):
        self.call("IRenderPipeline", "SetTextureStageTexture", {"Stage": stage, "Handle": tex},
                  category="state", key="TEX:%d" % stage)

    def modelview(self, m12):
        self.call("IRenderPipeline", "SetModelview", {"M12": words_of_floats(m12)}, category="state",
                  key="XF:SetModelview")

    def projection(self, m16):
        self.call("IRenderPipeline", "SetProjection", {"M16": words_of_floats(m16)}, category="state",
                  key="XF:SetProjection")

    def material(self, diffuse, ambient, specular, emissive, power):
        self.call("IRenderPipeline", "SetMaterial",
                  {"Material": words_of_floats(diffuse + ambient + specular + emissive + [power])},
                  category="state", key="MAT")

    def add_light(self, light_words):
        light = self.new_id()
        self.call("IRenderPipeline", "AddLight", {"Handle": light, "Light": light_words}, category="state",
                  key="L:%d:AddLight" % light, first_seen=[light])
        self.live_lights[light] = light_words
        self.pending_lights[light] = light_words
        return light

    def light_enable(self, light, enable):
        self.call("IRenderPipeline", "SetLightEnable", {"Handle": light, "Enable": enable}, category="state",
                  key="L:%d:SetLightEnable" % light)
        if enable and light in self.live_lights:
            self.pending_lights[light] = self.live_lights[light]

    def flush_lights(self):
        """The LightData lines before a call that flushes the renderer's lights."""
        for light in sorted(self.pending_lights):
            self.call("IRenderPipeline", "LightData", {"Handle": light, "Light": self.pending_lights[light]},
                      ret=None, category="state", key="L:%d:LightData" % light, synthetic=True)
        self.pending_lights = {}

    def draw_primitive(self, prim, fvf, vertices, count):
        self.flush_lights()
        self.call("IRenderPipeline", "DrawPrimitive", {"Prim": prim, "Fvf": fvf, "Verts": Data(vertices),
                                                       "Count": count, "Unused": 0}, category="frame")
        self.reset_selection()

    def select_vb_call(self, vb):
        self.call("IRPVertexBuffer", "SelectVb", {"Handle": vb}, category="frame")
        self.select_vb(vb)

    def select_ib_call(self, ib, base):
        self.call("IRPIndexBuffer", "SelectIb", {"Handle": ib, "BaseVertexIndex": base, "Unused3": 0,
                                                 "Unused4": 0}, category="frame")
        self.select_ib(ib, base)

    def draw_indexed(self, prim, min_index, num_vertices, start_index, count):
        self.flush_lights()
        self.call("IRPDraw", "DrawIndexedPrimitive", {"Prim": prim, "MinIndex": min_index,
                                                      "NumVertices": num_vertices, "StartIndex": start_index,
                                                      "Count": count}, category="frame")

    def text(self):
        if not self.closed:
            fail("%s: the trace did not reach its last selected frame" % self.name)
        return "".join(line + "\n" for line in self.lines)


# --- Vertices -------------------------------------------------------------------------


def argb(a, r, g, b):
    return (a << 24) | (r << 16) | (g << 8) | b


def xyz_diffuse(points):
    """FVF XYZ | DIFFUSE (16 bytes): (x, y, z, colour)."""
    return b"".join(struct.pack("<fffI", x, y, z, c) for x, y, z, c in points)


def xyzrhw_diffuse(points):
    """FVF XYZRHW | DIFFUSE (20 bytes): (x, y, z, rhw, colour), x and y in pixels."""
    return b"".join(struct.pack("<ffffI", x, y, z, w, c) for x, y, z, w, c in points)


def xyz_diffuse_tex1(points):
    """FVF XYZ | DIFFUSE | TEX1 (24 bytes): (x, y, z, colour, u, v)."""
    return b"".join(struct.pack("<fffIff", x, y, z, c, u, v) for x, y, z, c, u, v in points)


def xyz_normal(points):
    """FVF XYZ | NORMAL (24 bytes): (x, y, z, nx, ny, nz)."""
    return b"".join(struct.pack("<ffffff", *p) for p in points)


def normalized(x, y, z):
    length = (x * x + y * y + z * z) ** 0.5
    return (x / length, y / length, z / length)


# --- The scenes -----------------------------------------------------------------------
#
# Positions are engine coordinates. After CreateBuffers DX8RENDERER has VIEWSPACE_LIGHTS on,
# so SetModelview(identity) gives a world matrix that flips Z and an identity view; with an
# identity projection a vertex at engine z = -d lands at depth d. Every scene draws inside
# x, y in [-1, 1] and z in [-1, 0].


def begin(name):
    t = Trace(name, [1])
    t.startup()
    t.create_buffers()
    return t


def frame_start(t, colour, transforms=True):
    """Frame 0 ends; frame 1 clears colour and depth and opens the scene."""
    t.swap_buffers()
    t.set_pipeline_state(8, colour)
    t.clear(3)
    t.begin_scene()
    if transforms:
        t.modelview(IDENTITY12)
        t.projection(IDENTITY16)


def frame_end(t):
    t.end_scene()
    t.swap_buffers()
    return t


def scene_clear():
    """Clear only: the whole target, then a rectangle of the colour target alone."""
    t = begin("clear")
    t.swap_buffers()
    t.set_pipeline_state(8, argb(255, 0x33, 0x66, 0x99))
    t.clear(3)
    t.set_pipeline_state(8, argb(255, 0xCC, 0x33, 0x00))
    t.clear(1, [16, 12, 48, 40])
    t.begin_scene()
    return frame_end(t)


def scene_untextured():
    """Pre-lit diffuse triangles, lighting off: Gouraud colours through stage 0 disabled."""
    t = begin("untextured")
    frame_start(t, argb(255, 0x20, 0x20, 0x20))
    t.render_state("LIGHTING", 0)
    verts = xyz_diffuse([
        (-0.85, -0.80, -0.5, argb(255, 255, 0, 0)),
        (0.10, 0.85, -0.5, argb(255, 0, 255, 0)),
        (0.30, -0.70, -0.5, argb(255, 0, 0, 255)),
        (0.05, 0.60, -0.3, argb(255, 255, 255, 0)),
        (0.90, 0.75, -0.3, argb(255, 0, 255, 255)),
        (0.70, -0.20, -0.7, argb(255, 255, 0, 255)),
    ])
    t.draw_primitive(D3DPT_TRIANGLELIST, FVF_XYZ | FVF_DIFFUSE, verts, 6)
    return frame_end(t)


def scene_pretransformed():
    """XYZRHW vertices in pixels: a strip and a nearer triangle (depth test on RHW input)."""
    t = begin("pretransformed")
    frame_start(t, argb(255, 0x10, 0x18, 0x30), transforms=False)
    strip = xyzrhw_diffuse([
        (6.25, 52.75, 0.6, 1.0, argb(255, 255, 128, 0)),
        (6.25, 8.25, 0.6, 1.0, argb(255, 255, 255, 255)),
        (44.75, 56.25, 0.6, 1.0, argb(255, 0, 64, 255)),
        (40.25, 4.75, 0.6, 1.0, argb(255, 0, 200, 80)),
    ])
    t.draw_primitive(D3DPT_TRIANGLESTRIP, FVF_XYZRHW | FVF_DIFFUSE, strip, 4)
    tri = xyzrhw_diffuse([
        (30.4, 20.2, 0.3, 1.0, argb(255, 240, 240, 240)),
        (60.6, 34.8, 0.3, 1.0, argb(255, 240, 20, 20)),
        (26.2, 60.4, 0.3, 1.0, argb(255, 20, 20, 240)),
    ])
    t.draw_primitive(D3DPT_TRIANGLELIST, FVF_XYZRHW | FVF_DIFFUSE, tri, 3)
    return frame_end(t)


def checker_texture():
    """4x4 A8R8G8B8, rows top down, B G R A in memory."""
    colours = [argb(255, 255, 255, 255), argb(255, 255, 0, 0), argb(255, 0, 160, 0), argb(255, 0, 0, 255)]
    pixels = b""
    for y in range(4):
        for x in range(4):
            pixels += struct.pack("<I", colours[(x + 2 * y) % 4])
    return pixels


def scene_textured():
    """A8R8G8B8 texture, point filtering, stage 0 colour MODULATE with the diffuse colour."""
    t = begin("textured")
    tex = t.create_texture(4, 4, PF_A8R8G8B8, checker_texture())
    frame_start(t, argb(255, 0x40, 0x40, 0x40))
    t.stage_texture(0, tex)
    t.stage_state(0, "COLOROP", D3DTOP_MODULATE)
    t.stage_state(0, "ALPHAOP", D3DTOP_SELECTARG1)
    t.stage_state(0, "MAGFILTER", D3DTEXF_POINT)
    t.stage_state(0, "MINFILTER", D3DTEXF_POINT)
    # Corners off the pixel centres so no texel edge falls on one, and the
    # first and last covered rows and columns at least 0.3 px inside the
    # quad: with -0.72 the last row sampled v = 0.9991, and Direct3D 8 on
    # CI (v >= 1, wrapped) and Direct3D 12 (v < 1) disagreed there (D278).
    quad = xyz_diffuse_tex1([
        (-0.71, -0.71, -0.5, argb(255, 255, 255, 255), 0.0, 1.0),
        (-0.71, 0.68, -0.5, argb(255, 255, 255, 0), 0.0, 0.0),
        (0.68, -0.71, -0.5, argb(255, 0, 255, 255), 1.0, 1.0),
        (0.68, 0.68, -0.5, argb(255, 255, 255, 255), 1.0, 0.0),
    ])
    t.draw_primitive(D3DPT_TRIANGLESTRIP, FVF_XYZ | FVF_DIFFUSE | FVF_TEX1, quad, 4)
    return frame_end(t)


def scene_alpha_blend():
    """An opaque quad, then a half-transparent triangle over it: SRCALPHA, INVSRCALPHA."""
    t = begin("alpha_blend")
    frame_start(t, argb(255, 0x00, 0x00, 0x00))
    quad = xyz_diffuse([
        (-0.8, -0.8, -0.6, argb(255, 255, 0, 0)),
        (-0.8, 0.8, -0.6, argb(255, 255, 255, 0)),
        (0.8, -0.8, -0.6, argb(255, 0, 255, 0)),
        (0.8, 0.8, -0.6, argb(255, 0, 255, 255)),
    ])
    t.draw_primitive(D3DPT_TRIANGLESTRIP, FVF_XYZ | FVF_DIFFUSE, quad, 4)
    t.render_state("ALPHABLENDENABLE", 1)
    t.render_state("SRCBLEND", D3DBLEND_SRCALPHA)
    t.render_state("DESTBLEND", D3DBLEND_INVSRCALPHA)
    t.render_state("ZWRITEENABLE", 0)
    tri = xyz_diffuse([
        (-0.9, 0.9, -0.3, argb(128, 0, 0, 255)),
        (0.9, 0.3, -0.3, argb(64, 255, 255, 255)),
        (-0.3, -0.9, -0.3, argb(192, 0, 0, 255)),
    ])
    t.draw_primitive(D3DPT_TRIANGLELIST, FVF_XYZ | FVF_DIFFUSE, tri, 3)
    return frame_end(t)


def scene_alpha_test():
    """Alpha falling from 255 to 0 across a quad, ALPHATESTENABLE with GREATER 0x80."""
    t = begin("alpha_test")
    frame_start(t, argb(255, 0x30, 0x10, 0x10))
    t.render_state("ALPHATESTENABLE", 1)
    t.render_state("ALPHAREF", 0x80)
    t.render_state("ALPHAFUNC", D3DCMP_GREATER)
    quad = xyz_diffuse([
        (-0.9, -0.7, -0.5, argb(255, 255, 220, 0)),
        (-0.9, 0.7, -0.5, argb(255, 255, 220, 0)),
        (0.9, -0.7, -0.5, argb(0, 255, 220, 0)),
        (0.9, 0.7, -0.5, argb(0, 255, 220, 0)),
    ])
    t.draw_primitive(D3DPT_TRIANGLESTRIP, FVF_XYZ | FVF_DIFFUSE, quad, 4)
    return frame_end(t)


def scene_fog():
    """Linear vertex fog from depth 0 to 1 over a quad whose depth runs from 0.1 to 0.9."""
    t = begin("fog")
    frame_start(t, argb(255, 0x00, 0x00, 0x40))
    t.render_state("FOGENABLE", 1)
    t.render_state("FOGCOLOR", argb(0, 0xC0, 0xC0, 0xC0))
    t.render_state("FOGTABLEMODE", D3DFOG_NONE)
    t.render_state("FOGVERTEXMODE", D3DFOG_LINEAR)
    t.render_state("FOGSTART", fbits(0.0))
    t.render_state("FOGEND", fbits(1.0))
    quad = xyz_diffuse([
        (-0.85, -0.6, -0.1, argb(255, 255, 0, 0)),
        (-0.85, 0.6, -0.1, argb(255, 255, 0, 0)),
        (0.85, -0.6, -0.9, argb(255, 255, 0, 0)),
        (0.85, 0.6, -0.9, argb(255, 255, 0, 0)),
    ])
    t.draw_primitive(D3DPT_TRIANGLESTRIP, FVF_XYZ | FVF_DIFFUSE, quad, 4)
    return frame_end(t)


def directional_light(diffuse, direction):
    """D3DLIGHT8 (26 words): type, diffuse, specular, ambient, position, direction, range,
    falloff, attenuation 0..2, theta, phi."""
    values = [0.0] * 26
    values[1:5] = diffuse
    values[16:19] = direction
    words = Words(fbits(v) for v in values)
    words[0] = D3DLIGHT_DIRECTIONAL
    return words


def scene_directional_light():
    """Lighting on: a material, a little ambient and one directional light over a quad whose
    vertex normals lean four ways."""
    t = begin("directional_light")
    frame_start(t, argb(255, 0x08, 0x08, 0x10))
    t.render_state("LIGHTING", 1)
    t.render_state("AMBIENT", argb(0, 0x20, 0x20, 0x20))
    t.material([1.0, 0.8, 0.3, 1.0], [1.0, 1.0, 1.0, 1.0], [0.0, 0.0, 0.0, 0.0], [0.0, 0.0, 0.0, 0.0], 0.0)
    # Direction in the device's space (after the Z flip): into the screen, from the upper left.
    light = t.add_light(directional_light([1.0, 1.0, 1.0, 1.0], list(normalized(0.35, -0.3, 0.887))))
    t.light_enable(light, 1)
    n = [normalized(-0.6, 0.4, 0.7), normalized(-0.5, -0.5, 0.7), normalized(0.6, 0.5, 0.6),
         normalized(0.4, -0.6, 0.7)]
    quad = xyz_normal([
        (-0.8, 0.8, -0.5) + n[0],
        (-0.8, -0.8, -0.5) + n[1],
        (0.8, 0.8, -0.5) + n[2],
        (0.8, -0.8, -0.5) + n[3],
    ])
    t.draw_primitive(D3DPT_TRIANGLESTRIP, FVF_XYZ | FVF_NORMAL, quad, 4)
    return frame_end(t)


def scene_fan():
    """A triangle fan from a static index buffer, drawn through IRPDraw with base vertex 2:
    the first two vertices of the buffer are never read."""
    t = begin("fan")
    rim = [(0.0, 0.85), (0.8, 0.25), (0.5, -0.7), (-0.5, -0.7), (-0.8, 0.25)]
    colours = [argb(255, 255, 0, 0), argb(255, 255, 200, 0), argb(255, 0, 255, 0), argb(255, 0, 128, 255),
               argb(255, 200, 0, 255)]
    points = [(5.0, 5.0, -0.5, argb(255, 255, 255, 255)), (-5.0, 5.0, -0.5, argb(255, 255, 255, 255)),
              (0.0, 0.0, -0.5, argb(255, 255, 255, 255))]
    points += [(x, y, -0.5, c) for (x, y), c in zip(rim, colours)]
    vb = t.create_vb(FVF_XYZ | FVF_DIFFUSE, 16, xyz_diffuse(points))
    ib = t.create_ib([0, 1, 2, 3, 4, 5, 1])
    frame_start(t, argb(255, 0x18, 0x18, 0x18))
    t.select_vb_call(vb)
    t.select_ib_call(ib, 2)
    t.draw_indexed(D3DPT_TRIANGLEFAN, 0, 6, 0, 7)
    return frame_end(t)


SCENES = [scene_clear, scene_untextured, scene_pretransformed, scene_textured, scene_alpha_blend,
          scene_alpha_test, scene_fog, scene_directional_light, scene_fan]


def outputs():
    """{relative path: bytes} of every file this script owns."""
    files = {}
    for scene in SCENES:
        t = scene()
        files[os.path.join(t.name, "trace.jsonl")] = t.text().encode("ascii")
        for name, data in sorted(t.blobs.items()):
            files[os.path.join(t.name, "blobs", name + ".bin")] = data
    return files


def owned_on_disk():
    """Relative paths of the files under OUT this script would own (traces and blobs)."""
    found = set()
    if not os.path.isdir(OUT):
        return found
    for name in sorted(os.listdir(OUT)):
        folder = os.path.join(OUT, name)
        if not os.path.isdir(folder):
            continue
        if os.path.exists(os.path.join(folder, "trace.jsonl")):
            found.add(os.path.join(name, "trace.jsonl"))
        blobs = os.path.join(folder, "blobs")
        if os.path.isdir(blobs):
            for blob in os.listdir(blobs):
                found.add(os.path.join(name, "blobs", blob))
    return found


def main(argv):
    check = "--check" in argv[1:]
    files = outputs()
    stale = []
    for relative, data in sorted(files.items()):
        path = os.path.join(OUT, relative)
        current = None
        if os.path.exists(path):
            with open(path, "rb") as handle:
                current = handle.read()
        if current == data:
            continue
        if check:
            stale.append(relative)
        else:
            os.makedirs(os.path.dirname(path), exist_ok=True)
            with open(path, "wb") as handle:
                handle.write(data)
            print("wrote %s" % relative.replace(os.sep, "/"))
    for relative in sorted(owned_on_disk() - set(files)):
        if check:
            stale.append(relative)
        else:
            os.remove(os.path.join(OUT, relative))
            print("removed %s" % relative.replace(os.sep, "/"))
    if stale:
        for relative in stale:
            sys.stderr.write("out of date: %s\n" % relative.replace(os.sep, "/"))
        return 1
    print("%d traces, %d files" % (len(SCENES), len(files)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
