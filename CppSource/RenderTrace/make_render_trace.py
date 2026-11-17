#!/usr/bin/env python3
"""Generate the RenderTrace forwarding layer from include/render/IRenderPipeline.h.

Work package P0.5 (design/migration-plan.md). Writes RenderTraceGenerated.h (the
proxy class, every method of the seven renderer interfaces with the exact
signature from the header) and RenderTraceGenerated.cpp (the bodies of the
methods that only forward and record their arguments). Methods marked
"manual" below are written by hand in RenderTrace.cpp because they track
handles, locks or memory whose size the call does not state.

Run it from anywhere after a change to IRenderPipeline.h and commit both
outputs:

    python make_render_trace.py           # rewrite the outputs
    python make_render_trace.py --check   # exit 1 when they are out of date

The script fails when a method of the header has no entry here, when an entry
names a method the header lacks, or when its argument names differ, so the
proxy cannot drift from the interface. Output is deterministic: no dates, the
header's order.
"""

import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
HEADER = os.path.join(HERE, "..", "include", "render", "IRenderPipeline.h")
OUT_H = os.path.join(HERE, "RenderTraceGenerated.h")
OUT_CPP = os.path.join(HERE, "RenderTraceGenerated.cpp")

# Interface -> (IID constant, member holding the real object's face).
INTERFACES = [
    ("IRenderPipeline", "Iids::RenderPipeline8B", "RealPipeline_"),
    ("IVertexBufferManager", "Iids::VertexBufferManager", "RealVbMgr_"),
    ("IRPDraw", "Iids::RPDraw", "RealDraw_"),
    ("IRPIndexBuffer", "Iids::RPIndexBuffer", "RealIb_"),
    ("IRPVertexBuffer", "Iids::RPVertexBuffer", "RealVb_"),
    ("IGammaControl", "Iids::GammaControl", "RealGamma_"),
    ("IRPTexture", "Iids::RPTexture", "RealTexture_"),
]

CATEGORIES = {
    "always": "RenderTrace::kAlways",
    "state": "RenderTrace::kState",
    "frame": "RenderTrace::kFrame",
}

MANUAL = "manual"

# Per method: "manual", or (category, arguments, options).
#   category: always | state | frame (RenderTraceCore.h, ECategory)
#   arguments: "Name:kind | ..." in header order, every argument named once.
#     i u f        signed, unsigned, float (8 hex digits of its bits)
#     raw          pointer-sized value as a number
#     str          NUL-terminated string
#     words:E      E 32-bit words read from the pointer
#     bytes:E      E bytes read from the pointer (inline hex or a blob)
#     out_i out_u out_f out_ptr   the value behind an out-pointer after the call
#     tex vb ib light             a handle, written as its id
#     out_tex      the texture handle behind an out-pointer after the call
#   options: key = (format, [arguments]) of the state slot (state calls),
#            post = a statement run after the line while tracking.
SPEC = {
    "IRenderPipeline": {
        "Startup": ("always", "SectionName:str", {}),
        "ShutdownPipeline": ("always", "", {}),
        "SetPipelineState": ("always", "Id:i | Value:i", {}),
        "GetPipelineState": ("frame", "Id:i | Out:out_i", {}),
        "GetDeviceInfo": ("frame", "Out520:raw", {}),
        "QueryDeviceAbility": ("frame", "Index:u | Out:out_u", {}),
        "GetDisplayModeCount": ("frame", "Out:out_u", {}),
        "GetDisplayModeEnum": ("frame", "Index:u | Out:words:5", {}),
        "SelectMode": ("always", "Mode:words:10 | OutIndex:out_u", {}),
        "CreateBuffers": ("always", "Hwnd:raw | Mode:words:10 | OutMode:words:10",
                          {"post": "if (Result >= 0)\n            Trace_.OnDeviceReset();"}),
        "GetModeBlob": ("frame", "OutAdapter:raw | OutMode10Dwords:words:10", {}),
        "DestroyGpuResources": ("always", "", {"post": "Trace_.ResetSelection();"}),
        "ClearBuffers": ("frame", "Flags:u | Rects:words:4", {}),
        "SwapBuffers": MANUAL,
        "LockBuffer": MANUAL,
        "UnlockBuffer": MANUAL,
        "GetBufferInterface": ("frame", "Out:raw | Unused:raw", {}),
        "GetDeviceStats": ("frame", "Out:raw", {}),
        "SetViewportRect": ("state", "X:i | Y:i | W:i | H:i", {"key": ("VP", [])}),
        "GetViewportRect": ("frame", "OutX:out_i | OutY:out_i | OutW:out_i | OutH:out_i", {}),
        "SetDepthRange": ("state", "MinZ:f | MaxZ:f", {"key": ("DR", [])}),
        "GetDepthRange": ("frame", "OutMinZ:out_f | OutMaxZ:out_f", {}),
        "SetWindow": ("always", "Hwnd:raw | X:i | Y:i | W:i | H:i", {}),
        "GetWindow": ("frame", "OutHwnd:out_ptr | OutLeft:out_i | OutRight:out_i | OutW:out_i | OutH:out_i", {}),
        "SetWorld": ("state", "M12:words:12", {"key": ("XF:SetWorld", [])}),
        "GetWorld": ("frame", "Out12:words:12", {}),
        "SetView": ("state", "M12:words:12", {"key": ("XF:SetView", [])}),
        "GetView": ("frame", "Out12:words:12", {}),
        "SetModelview": ("state", "M12:words:12", {"key": ("XF:SetModelview", [])}),
        "GetModelview": ("frame", "Out12:words:12", {}),
        "SetProjection": ("state", "M16:words:16", {"key": ("XF:SetProjection", [])}),
        "GetProjection": ("frame", "Out16:words:16", {}),
        "SetLookat": ("state", "EyeX:f | EyeY:f | EyeZ:f | AtX:f | AtY:f | AtZ:f | UpX:f | UpY:f | UpZ:f",
                      {"key": ("XF:SetLookat", [])}),
        "SetOrtho": ("state", "Left:f | Right:f | Bottom:f | Top:f | NearPlane:f | FarPlane:f",
                     {"key": ("XF:SetOrtho", [])}),
        "SetPerspective": ("state", "FovDeg:f | Aspect:f | NearPlane:f | FarPlane:f",
                           {"key": ("XF:SetPerspective", [])}),
        "SetLight": MANUAL,
        "DestroyLight": MANUAL,
        # GetLight passes its first argument to the device as a light index.
        "GetLight": ("frame", "Handle:raw | OutLight:words:26", {}),
        "SetLightEnable": MANUAL,
        "GetLightEnable": ("frame", "Handle:light | OutEnable:out_i", {}),
        "SetMaterial": ("state", "Material:words:17", {"key": ("MAT", [])}),
        "GetMaterial": ("frame", "Material:words:17", {}),
        "CreateTexture": MANUAL,
        "DestroyTexture": MANUAL,
        "IsTexture": ("frame", "Handle:tex", {}),
        "LockTexture": MANUAL,
        "UnlockTexture": MANUAL,
        "GetTextureFormat": ("frame", "Handle:tex | OutFormat:out_i", {}),
        "GetTextureDim": ("frame", "Handle:tex | OutW:out_u | OutH:out_u | OutLevels:out_i", {}),
        "GetTextureInterface": ("frame", "Handle:tex | UnusedA:raw | UnusedB:raw", {}),
        "SetTextureLevelData": MANUAL,
        "BlitTexture": ("always", "DstHandle:tex | DstLevel:i | SrcHandle:tex | SrcLevel:i", {}),
        "SetRenderTarget": ("state", "Handle:tex | UnusedA:raw | UnusedB:raw", {"key": ("RT", [])}),
        "GetRenderTarget": ("frame", "Handle:raw", {}),
        "BeginScene": ("frame", "", {}),
        "EndScene": ("frame", "", {}),
        "ResetRenderStatesToDefaults": ("state", "", {
            "key": ("RESET", []),
            "post": "if (Result == GR_OK)\n            Trace_.OnResetStates();"}),
        "SetRenderState": ("state", "State:u | Value:i", {
            "key": ("RS:%u", ["(unsigned int)(State)"]),
            "post": "if (Result == GR_OK)\n            Trace_.OnRenderState((unsigned int)(State), Value);"}),
        "GetRenderState": ("frame", "State:u | Out:out_i", {}),
        "SetTextureStageState": ("state", "Stage:i | Type:i | Value:i", {
            "key": ("TSS:%d:%d", ["Stage", "Type"])}),
        "GetTextureStageState": ("frame", "Stage:i | Type:i | Out:out_i", {}),
        "SetTextureStageTransform": ("state", "Stage:i | Matrix16:words:16", {"key": ("TT:%d", ["Stage"])}),
        "GetTextureStageTransform": ("frame", "Stage:i | OutMatrix16:words:16", {}),
        "SetTextureStageTexture": ("state", "Stage:i | Handle:tex", {
            "key": ("TEX:%d", ["Stage"]),
            "post": "if (Result == GR_OK)\n            Trace_.OnStageTexture(Stage, Handle);"}),
        "GetTextureStageTexture": ("frame", "Stage:i | OutHandle:out_tex", {}),
        "VerifyState": ("frame", "", {}),
        "DrawPrimitive": ("frame",
                          "Prim:u | Fvf:u | Verts:bytes:RenderTrace::DrawUpVertexBytes(Prim, Fvf, Count) "
                          "| Count:u | Unused:raw",
                          {"post": "if (Result != GR_GENERIC && (int)(Count) > 0)\n"
                                   "            Trace_.ResetSelection();"}),
        "DrawIndexedPrimitiveUp": ("frame",
                                   "Prim:u | Fvf:u "
                                   "| Verts:bytes:RenderTrace::DrawIndexedUpVertexBytes(Prim, Fvf, VertCount, IndexCount) "
                                   "| VertCount:u "
                                   "| Indices:bytes:RenderTrace::DrawIndexedUpIndexBytes(Prim, IndexCount) "
                                   "| IndexCount:u | Unused:u",
                                   {"post": "if (Result != GR_GENERIC && (int)(RenderTrace::PrimCount(Prim, IndexCount)) > 0)\n"
                                            "            Trace_.ResetSelection();"}),
        "DrawPrimitiveVb": ("frame", "Prim:u | Vb:vb | StartVertex:u | Count:u | Unused:raw",
                            {"post": "if (Result != GR_GENERIC && Vb != nullptr && (int)(RenderTrace::PrimCount(Prim, Count)) > 0)\n"
                                     "            Trace_.SelectVb(Vb);"}),
        "DrawIndexedPrimitiveVb": ("frame",
                                   "Prim:u | Vb:vb | BaseVertex:u | NumVerts:u "
                                   "| Indices:bytes:RenderTrace::DrawIndexedVbIndexBytes(Prim, IndexCount) "
                                   "| IndexCount:u | Unused:u",
                                   {"post": "if (Result >= 0 && Vb != nullptr)\n"
                                            "        {\n"
                                            "            Trace_.SelectVb(Vb);\n"
                                            "            Trace_.SelectIb((const void*)((intptr_t)-1), BaseVertex);\n"
                                            "        }"}),
        "AddLight": MANUAL,
        "RemoveLight": MANUAL,
        "UpdateLight": MANUAL,
        "SetWorldN": ("state", "WorldIndex:i | M12:words:12", {"key": ("XF:SetWorldN:%d", ["WorldIndex"])}),
    },
    "IVertexBufferManager": {
        "VbMgrInit": ("always", "A:raw | B:raw | C:raw | D:raw", {}),
        "VbMgrReleaseAll": ("always", "", {"post": "Trace_.ResetSelection();"}),
        "LockVertexMemory": MANUAL,
        "UnlockVertexMemory": MANUAL,
        "UnsupportedManagerOperation": ("frame", "", {}),
        "CopyVertexData": MANUAL,
    },
    "IRPDraw": {
        "DrawIndexedPrimitive": ("frame", "Prim:u | MinIndex:u | NumVertices:u | StartIndex:u | Count:u", {}),
    },
    "IRPIndexBuffer": {
        "CreateIb": MANUAL,
        "DestroyIb": MANUAL,
        "ResizeIb": ("always", "Handle:ib | Count:u", {}),
        "CopyIndices": MANUAL,
        "LockIb": MANUAL,
        "UnlockIb": MANUAL,
        "SelectIb": ("frame", "Handle:ib | BaseVertexIndex:u | Unused3:u | Unused4:u", {
            "post": "if (Handle == nullptr)\n"
                    "            Trace_.ClearIb();\n"
                    "        else if (Result == GR_OK)\n"
                    "            Trace_.SelectIb(Handle, BaseVertexIndex);"}),
        "GetIbCapacity": ("frame", "Handle:ib | OutCapacity:out_u", {}),
        "IsIbValid": ("frame", "Handle:ib", {}),
    },
    "IRPVertexBuffer": {
        "CreateVb": MANUAL,
        "DestroyVb": MANUAL,
        "ResizeVb": ("always", "Handle:vb | Fvf:u | Count:u", {
            "post": "if (Result == GR_OK)\n            Trace_.OnVbGone(Handle);"}),
        "CopyVertices": MANUAL,
        "LockVb": MANUAL,
        "UnlockVb": MANUAL,
        "UnsupportedVertexBufferOperation": ("frame", "Handle:vb", {}),
        "SelectVb": ("frame", "Handle:vb", {
            "post": "if (Handle == nullptr)\n"
                    "            Trace_.ResetSelection();\n"
                    "        else if (Handle != (void*)((intptr_t)-1) && Result == GR_OK)\n"
                    "            Trace_.SelectVb(Handle);"}),
        "GetVbInfo": ("frame", "Handle:vb | OutFvf:out_u | OutCount:out_u", {}),
        "IsVbValid": ("frame", "Handle:vb", {}),
    },
    "IGammaControl": {
        "SetGammaFunction": ("always", "ChannelMask:u | Gamma:f | Brightness:f | Contrast:f | Bias:f", {}),
        "SetGammaRamp": ("always", "ChannelMask:u | RampData:bytes:512", {}),
        "GetGammaRamp": ("frame", "Channel:u | OutRamp:bytes:512", {}),
        "SetGammaEnable": ("always", "Enable:u", {}),
        "GetGammaEnable": ("frame", "", {}),
    },
    "IRPTexture": {
        "PrintScreen": ("frame", "ParentFileSystem:raw | FileName:str", {}),
        "LoadTexture": MANUAL,
        "LoadSurfaceData": ("always", "Handle:tex | Level:i | SrcSize:u | SrcData:bytes:SrcSize", {}),
        "GetImageInfo": ("always", "Info:words:sizeof(D3DXIMAGE_INFO) / 4 | SrcSize:u | SrcData:bytes:SrcSize", {}),
        "LoadCubemap": MANUAL,
    },
}

HANDLE_KINDS = {
    "tex": "RenderTrace::kTexture",
    "vb": "RenderTrace::kVertexBuffer",
    "ib": "RenderTrace::kIndexBuffer",
    "light": "RenderTrace::kLight",
}


class Method:
    def __init__(self, interface, name, params, slot):
        self.interface = interface
        self.name = name
        self.params = params  # [(type, name)]
        self.slot = slot

    def signature(self, qualified):
        args = ", ".join("%s %s" % p for p in self.params)
        prefix = "FRenderTrace::" if qualified else ""
        return "GENRESULT NEURONCOMPONENTS_API %s%s(%s)" % (prefix, self.name, args)


def fail(message):
    sys.stderr.write("make_render_trace.py: %s\n" % message)
    sys.exit(2)


def parse_header(path):
    with open(path, "r", encoding="utf-8") as handle:
        text = handle.read()
    text = re.sub(r"//[^\n]*", "", text)
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    found = {}
    for match in re.finditer(r"struct\s+(\w+)\s*:\s*INeuronComponent\s*\{(.*?)\n\};", text, re.S):
        interface, body = match.group(1), match.group(2)
        methods = []
        pattern = r"virtual\s+(\w+)\s+NEURONCOMPONENTS_API\s+(\w+)\s*\((.*?)\)\s*=\s*0\s*;"
        for slot, item in enumerate(re.finditer(pattern, body, re.S), start=3):
            result, name, raw = item.group(1), item.group(2), item.group(3)
            if result != "GENRESULT":
                fail("%s::%s returns %s, not GENRESULT" % (interface, name, result))
            params = []
            if raw.strip():
                for piece in raw.split(","):
                    piece = " ".join(piece.split())
                    words = re.match(r"^(.*?)(\w+)$", piece)
                    if words is None:
                        fail("cannot parse argument '%s' of %s::%s" % (piece, interface, name))
                    ptype = words.group(1).strip().replace(" *", "*")
                    params.append((ptype, words.group(2)))
            methods.append(Method(interface, name, params, slot))
        found[interface] = methods
    for interface, _, _ in INTERFACES:
        if interface not in found:
            fail("interface %s not found in %s" % (interface, path))
    return found


def parse_arguments(text):
    result = []
    if not text.strip():
        return result
    for piece in text.split("|"):
        piece = piece.strip()
        name, _, kind = piece.partition(":")
        if not kind:
            fail("argument '%s' has no kind" % piece)
        result.append((name, kind))
    return result


def argument_code(name, kind):
    if kind == "i":
        return 'Call.Int("%s", (long long)(%s));' % (name, name)
    if kind == "u":
        return 'Call.UInt("%s", (unsigned long long)(%s));' % (name, name)
    if kind == "f":
        # Read the argument's bits from its stack slot: no x87 load, so a signalling NaN
        # is recorded as passed.
        return 'Call.FloatAt("%s", &%s);' % (name, name)
    if kind == "raw":
        return 'Call.Raw("%s", (const void*)(%s));' % (name, name)
    if kind == "str":
        return 'Call.Str("%s", %s);' % (name, name)
    if kind.startswith("words:"):
        return 'Call.Words("%s", (const void*)(%s), %s);' % (name, name, kind[len("words:"):])
    if kind.startswith("bytes:"):
        return 'Call.Bytes("%s", (const void*)(%s), (size_t)(%s));' % (name, name, kind[len("bytes:"):])
    if kind == "out_i":
        return 'Call.OutI32("%s", (const void*)(%s));' % (name, name)
    if kind == "out_u":
        return 'Call.OutU32("%s", (const void*)(%s));' % (name, name)
    if kind == "out_f":
        return 'Call.OutF32("%s", (const void*)(%s));' % (name, name)
    if kind == "out_ptr":
        return 'Call.OutPtr("%s", (const void*)(%s));' % (name, name)
    if kind in HANDLE_KINDS:
        return 'Call.Handle("%s", %s, (const void*)(%s));' % (name, HANDLE_KINDS[kind], name)
    if kind == "out_tex":
        return 'Call.OutHandle("%s", RenderTrace::kTexture, (const void*)(%s));' % (name, name)
    fail("unknown argument kind '%s'" % kind)
    return ""


def check_spec(found):
    for interface, _, _ in INTERFACES:
        spec = SPEC.get(interface)
        if spec is None:
            fail("no SPEC entry for %s" % interface)
        names = [m.name for m in found[interface]]
        for name in names:
            if name not in spec:
                fail("%s::%s has no SPEC entry" % (interface, name))
        for name in spec:
            if name not in names:
                fail("SPEC names %s::%s, which the header does not declare" % (interface, name))
        for method in found[interface]:
            entry = spec[method.name]
            if entry == MANUAL:
                continue
            category, arguments, options = entry
            if category not in CATEGORIES:
                fail("%s::%s: unknown category %s" % (interface, method.name, category))
            spec_names = [n for n, _ in parse_arguments(arguments)]
            header_names = [n for _, n in method.params]
            if spec_names != header_names:
                fail("%s::%s: SPEC arguments %s differ from the header's %s"
                     % (interface, method.name, spec_names, header_names))
            if category == "state" and "key" not in options and "post" not in options:
                fail("%s::%s: a state call needs a key or a post hook" % (interface, method.name))
            for option in options:
                if option not in ("key", "post"):
                    fail("%s::%s: unknown option %s" % (interface, method.name, option))


HEADER_TEMPLATE = """\
// Generated by make_render_trace.py from include/render/IRenderPipeline.h. Do not edit:
// change the script (or the hand-written RenderTrace.cpp) and run it again.
//
// FRenderTrace is the RenderTrace component (design/migration-plan.md P0.5): it
// aggregates the real DirectX8 renderer and exposes the same seven interfaces, each
// method forwarding to the real object and recording the call (README.md).

#pragma once

#include "RenderTraceCore.h"

static const char kRenderTraceComponentName[] = "RenderTrace";

class FRenderTrace;

// The non-delegating unknown the [System] container holds (FAGGDESC14 InnerUnknown).
class FRenderTraceInner final : public INeuronComponent
{
public:
    FRenderTraceInner() : RefCount(1), Owner(nullptr) {}

    GENRESULT NEURONCOMPONENTS_API QueryInterface(const char* Iid, void** Out) override;
    unsigned long NEURONCOMPONENTS_API AddRef() override;
    unsigned long NEURONCOMPONENTS_API Release() override;

    unsigned long RefCount;
    FRenderTrace* Owner;
};

class FRenderTrace final : public IRenderPipeline,
                           public IVertexBufferManager,
                           public IRPDraw,
                           public IRPIndexBuffer,
                           public IRPVertexBuffer,
                           public IGammaControl,
                           public IRPTexture,
                           public IAggregateComponent
{
public:
    FRenderTrace();
    ~FRenderTrace();

    // RenderTrace.cpp: creates the real renderer aggregated under this object.
    GENRESULT CreateReal(const char* Section);
    GENRESULT QueryLocal(const char* Iid, void** Out);

    // INeuronComponent and IAggregateComponent (RenderTrace.cpp).
    GENRESULT NEURONCOMPONENTS_API QueryInterface(const char* Iid, void** Out) override;
    unsigned long NEURONCOMPONENTS_API AddRef() override;
    unsigned long NEURONCOMPONENTS_API Release() override;
    GENRESULT NEURONCOMPONENTS_API Initialize() override;
%(methods)s
    // The controlling unknown: the container, or Inner_ when not aggregated.
    INeuronComponent* Outer_;
    FRenderTraceInner Inner_;
    // The real renderer's non-delegating unknown; this object owns its one reference.
    INeuronComponent* RealInner_;
    // The real renderer's faces. Their AddRef was balanced on Outer_ when they were
    // obtained (COM aggregation rule), so they hold no reference and are not released.
%(members)s    IAggregateComponent* RealAggregate_;
    RenderTrace::FSession Trace_;
};
"""

CPP_TEMPLATE = """\
// Generated by make_render_trace.py from include/render/IRenderPipeline.h. Do not edit:
// change the script (or the hand-written RenderTrace.cpp) and run it again.
//
// The methods that only forward and record their arguments. Methods that track
// handles, locks or memory sizes are in RenderTrace.cpp.

#include "RenderTraceGenerated.h"

#include "d3dx8tex.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
%(bodies)s"""


def generate(found):
    declarations = []
    bodies = []
    members = []
    for interface, _, member in INTERFACES:
        methods = found[interface]
        members.append("    %s* %s;\n" % (interface, member))
        declarations.append("")
        declarations.append("    // %s (%d method%s, slots 3..%d)"
                            % (interface, len(methods), "" if len(methods) == 1 else "s", methods[-1].slot))
        for method in methods:
            entry = SPEC[interface][method.name]
            where = "hand-written" if entry == MANUAL else "generated"
            declarations.append("    %s override; // slot %d, %s" % (method.signature(False), method.slot, where))
            if entry == MANUAL:
                continue
            category, arguments, options = entry
            forward = "%s->%s(%s)" % (member, method.name, ", ".join(n for _, n in method.params))
            lines = []
            lines.append("")
            lines.append("// %s slot %d" % (interface, method.slot))
            lines.append(method.signature(True))
            lines.append("{")
            lines.append("    const GENRESULT Result = %s;" % forward)
            lines.append("    if (Trace_.Tracking())")
            lines.append("    {")
            lines.append('        RenderTrace::FCall Call(Trace_, %s, "%s", "%s");'
                         % (CATEGORIES[category], interface, method.name))
            lines.append("        if (Call.Active())")
            lines.append("        {")
            for name, kind in parse_arguments(arguments):
                lines.append("            " + argument_code(name, kind))
            lines.append("            Call.Return(Result);")
            if "key" in options:
                fmt, key_args = options["key"]
                if key_args:
                    lines.append("            char Key[64];")
                    lines.append('            snprintf(Key, sizeof(Key), "%s", %s);' % (fmt, ", ".join(key_args)))
                    lines.append("            Call.KeepAsState(Key);")
                else:
                    lines.append('            Call.KeepAsState("%s");' % fmt)
            lines.append("        }")
            if "post" in options:
                lines.append("        " + options["post"])
            lines.append("    }")
            lines.append("    return Result;")
            lines.append("}")
            bodies.append("\n".join(lines))
    header = HEADER_TEMPLATE % {"methods": "\n".join(declarations) + "\n", "members": "".join(members)}
    cpp = CPP_TEMPLATE % {"bodies": "\n".join(bodies) + "\n"}
    return header, cpp


def main(argv):
    check = "--check" in argv[1:]
    found = parse_header(HEADER)
    check_spec(found)
    header, cpp = generate(found)
    stale = []
    for path, text in ((OUT_H, header), (OUT_CPP, cpp)):
        current = None
        if os.path.exists(path):
            # Universal newlines: a checkout with CRLF line ends compares equal.
            with open(path, "r", encoding="utf-8") as handle:
                current = handle.read()
        if current == text:
            continue
        if check:
            stale.append(path)
        else:
            with open(path, "w", encoding="utf-8", newline="\n") as handle:
                handle.write(text)
            print("wrote %s" % os.path.relpath(path, HERE))
    if stale:
        for path in stale:
            sys.stderr.write("out of date: %s\n" % os.path.relpath(path, HERE))
        return 1
    counts = ", ".join("%s %d" % (i, len(found[i])) for i, _, _ in INTERFACES)
    manual = sum(1 for i, _, _ in INTERFACES for m in found[i] if SPEC[i][m.name] == MANUAL)
    print("%s; %d hand-written" % (counts, manual))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
