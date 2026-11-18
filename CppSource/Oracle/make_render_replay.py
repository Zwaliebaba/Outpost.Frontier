#!/usr/bin/env python3
"""Generate the call decoding of Oracle.exe render-replay from the RenderTrace method table.

Work package P4.3, decision record 0004 point 15. `Oracle.exe render-replay`
(oracle_render_replay.cpp) plays a version-1 renderer trace into the real
DX8RENDERER. The trace writes every argument by the kind that
CppSource/RenderTrace/make_render_trace.py gives it in its table (SPEC); this
script reads that table and the signatures of include/render/IRenderPipeline.h
through the same module, and writes oracle_render_replay_generated.h: one
branch per method the proxy records by the generic path (the 80 methods that
are not "manual" there), which decodes the arguments, calls the method on the
real interface and checks the scalar out-values. The 30 hand-written methods
of the proxy are hand-written in oracle_render_replay.cpp too.

    python make_render_replay.py           # rewrite the output
    python make_render_replay.py --check   # exit 1 when it is out of date

Deterministic: the header's order, no dates.
"""

import importlib.util
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
TRACE_SCRIPT = os.path.join(HERE, "..", "RenderTrace", "make_render_trace.py")
OUT = os.path.join(HERE, "oracle_render_replay_generated.h")

# Record 0004, point 13: methods answered from the recorded result, not replayed (mode
# enumeration and device information, the stub getters, render targets, the unsupported
# slots, VbMgrInit, screenshots). Their pointer arguments are addresses of the traced
# process, so calling them would be wrong as well as useless.
ANSWERED = {
    "GetDeviceInfo",
    "QueryDeviceAbility",
    "GetDisplayModeCount",
    "GetDisplayModeEnum",
    "SelectMode",
    "GetModeBlob",
    "GetBufferInterface",
    "GetDeviceStats",
    "GetTextureInterface",
    "BlitTexture",
    "SetRenderTarget",
    "GetRenderTarget",
    "VbMgrInit",
    "UnsupportedManagerOperation",
    "UnsupportedVertexBufferOperation",
    "PrintScreen",
}

# Calls whose failure does not change the frame image: a different result is reported as a
# warning, not as a failed replay. Gamma (the image is taken before the ramp; its results
# depend on the device's gamma caps), getters and VerifyState (ValidateDevice).
def warn_only(method):
    return (method.startswith("Get") or method.startswith("Is") or method in (
        "SetGammaFunction", "SetGammaRamp", "SetGammaEnable", "VerifyState"))


def load_trace_module():
    sys.dont_write_bytecode = True  # leave no __pycache__ in CppSource/RenderTrace
    spec =importlib.util.spec_from_file_location("make_render_trace", TRACE_SCRIPT)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def fail(message):
    sys.stderr.write("make_render_replay.py: %s\n" % message)
    sys.exit(2)


def pointee(ptype):
    if not ptype.endswith("*"):
        fail("out argument of type %s is not a pointer" % ptype)
    return ptype[:-1].strip()


def branch(interface, member, method, arguments):
    """The C++ lines of one method's branch."""
    params = dict((name, ptype) for ptype, name in method.params)
    lines = []
    lines.append('    if (strcmp(Interface, "%s") == 0 && strcmp(Method, "%s") == 0)' % (interface, method.name))
    lines.append("    {")
    if method.name in ANSWERED:
        lines.append("        // Record 0004, point 13: the recorded result stands; nothing is called.")
        lines.append("        Out->bCalled = false;")
        lines.append("        return true;")
        lines.append("    }")
        return lines
    names = []
    after = []
    for name, kind in arguments:
        ptype = params[name]
        local = "Arg" + name
        if kind == "i":
            lines.append("        const %s %s = (%s)(Int(Args, \"%s\"));" % (ptype, local, ptype, name))
        elif kind == "u":
            lines.append("        const %s %s = (%s)(UInt(Args, \"%s\"));" % (ptype, local, ptype, name))
        elif kind == "f":
            lines.append("        const float %s = Float(Args, \"%s\");" % (local, name))
        elif kind == "raw":
            if ptype == "HWND":
                lines.append("        const HWND %s = Window_; // the recorded HWND is the traced process's" % local)
            else:
                lines.append("        %s const %s = (%s)((uintptr_t)(UInt(Args, \"%s\")));" % (ptype, local, ptype, name))
        elif kind == "str":
            lines.append("        bool bNull%s = false;" % name)
            lines.append("        const std::string Text%s = Str(Args, \"%s\", &bNull%s);" % (name, name, name))
            lines.append("        const char* %s = bNull%s ? nullptr : Text%s.c_str();" % (local, name, name))
        elif kind.startswith("words:"):
            lines.append("        bool bNull%s = false;" % name)
            lines.append("        std::vector<unsigned int> Words%s = WordsArg(Args, \"%s\", &bNull%s);" % (name, name, name))
            lines.append("        %s const %s = (%s)(bNull%s ? nullptr : WordPtr(Words%s));" % (ptype, local, ptype, name, name))
        elif kind.startswith("bytes:"):
            lines.append("        bool bNull%s = false;" % name)
            lines.append("        std::vector<unsigned char> Bytes%s = BytesArg(Args, \"%s\", &bNull%s);" % (name, name, name))
            lines.append("        %s const %s = (%s)(bNull%s ? nullptr : BytePtr(Bytes%s));" % (ptype, local, ptype, name, name))
        elif kind in ("out_i", "out_u", "out_f", "out_ptr"):
            base = pointee(ptype)
            lines.append("        %s Store%s{};" % (base, name))
            lines.append("        %s const %s = IsNull(Args, \"%s\") ? nullptr : &Store%s;" % (ptype, local, name, name))
            if kind == "out_f":
                after.append("        if (%s != nullptr)" % local)
                after.append("            CompareOutBits(Args, \"%s\", FloatBits(Store%s));" % (name, name))
            elif kind != "out_ptr":
                after.append("        if (%s != nullptr)" % local)
                after.append("            CompareOutBits(Args, \"%s\", (unsigned int)(Store%s));" % (name, name))
        elif kind == "tex":
            lines.append("        void* const %s = TextureArg(Args, \"%s\");" % (local, name))
        elif kind == "vb":
            lines.append("        void* const %s = VbArg(Args, \"%s\");" % (local, name))
        elif kind == "ib":
            lines.append("        void* const %s = IbArg(Args, \"%s\");" % (local, name))
        elif kind == "light":
            lines.append("        void* const %s = LightArg(Args, \"%s\");" % (local, name))
        elif kind == "out_tex":
            lines.append("        void* Store%s = nullptr;" % name)
            lines.append("        void** const %s = IsNull(Args, \"%s\") ? nullptr : &Store%s;" % (local, name, name))
            after.append("        if (%s != nullptr)" % local)
            after.append("            CompareOutTexture(Args, \"%s\", Store%s);" % (name, name))
        else:
            fail("%s::%s: no decoding for argument kind %s" % (interface, method.name, kind))
        names.append(local)
    lines.append("        Out->Result = %s->%s(%s);" % (member, method.name, ", ".join(names)))
    lines.append("        Out->bCalled = true;")
    if warn_only(method.name):
        lines.append("        Out->bWarnOnly = true;")
    lines.extend(after)
    lines.append("        return true;")
    lines.append("    }")
    return lines


TEMPLATE = """\
// Generated by make_render_replay.py from CppSource/RenderTrace/make_render_trace.py (its
// method table) and include/render/IRenderPipeline.h. Do not edit: change the script (or
// the hand-written oracle_render_replay.cpp) and run it again.
//
// FPlayer::DispatchGenerated: the methods RenderTrace records by its generic path, each
// decoded from the trace line's arguments and called on the real renderer (record 0004,
// point 13). Included once, by oracle_render_replay.cpp, inside its namespace.

bool FPlayer::DispatchGenerated(const char* Interface, const char* Method, const FJson& Args, FOutcome* Out)
{
%(branches)s
    return false;
}
"""


def generate(trace):
    found = trace.parse_header(trace.HEADER)
    trace.check_spec(found)
    branches = []
    count = 0
    for interface, _, real in trace.INTERFACES:
        # The proxy's RealPipeline_ face is the player's Pipeline_, and so on.
        member = real[len("Real"):]
        for method in found[interface]:
            entry = trace.SPEC[interface][method.name]
            if entry == trace.MANUAL:
                continue
            _, arguments, _ = entry
            branches.append("    // %s slot %d" % (interface, method.slot))
            branches.extend(branch(interface, member, method, trace.parse_arguments(arguments)))
            count += 1
    return TEMPLATE % {"branches": "\n".join(branches)}, count


def main(argv):
    check = "--check" in argv[1:]
    text, count = generate(load_trace_module())
    current = None
    if os.path.exists(OUT):
        with open(OUT, "r", encoding="utf-8") as handle:
            current = handle.read()
    if current != text:
        if check:
            sys.stderr.write("out of date: %s\n" % os.path.relpath(OUT, HERE))
            return 1
        with open(OUT, "w", encoding="utf-8", newline="\n") as handle:
            handle.write(text)
        print("wrote %s" % os.path.relpath(OUT, HERE))
    print("%d generated methods" % count)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
