#!/usr/bin/env python3
"""Generate sim_client_stub_iclient.cpp, the IClient stub of Oracle.exe sim.

Oracle instrumentation, work package P2.3 (design/reference-build.md section 10).

Reads the IClient and IChatClient declarations in CppSource/include/rpc_iclient.h through
the parser of the client's +oracle proxy generator (OutpostFrontier/Client/make_oracle_iclient.py)
and writes one class that implements both interfaces, records every call into iclient.jsonl
and forwards nothing: the scenario server has no client. Run it from any directory after
rpc_iclient.h changes and commit the result:

    python3 CppSource/Oracle/SimClientStub/make_sim_client_stub.py

With --check it writes nothing and exits 1 when the committed file differs.
"""

import argparse
import importlib.util
import pathlib
import sys

HERE = pathlib.Path(__file__).resolve().parent
CLIENT_GENERATOR = HERE.parent.parent / "OutpostFrontier" / "Client" / "make_oracle_iclient.py"
OUTPUT = HERE / "sim_client_stub_iclient.cpp"


def load_client_generator():
    spec = importlib.util.spec_from_file_location("make_oracle_iclient", CLIENT_GENERATOR)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


# Argument types recorded by value; every other reference or pointer is "present" (or null).
SCALARS = {
    "unsigned int": "Uint",
    "const unsigned int&": "Uint",
    "unsigned long": "Uint",
    "unsigned short": "Uint",
    "unsigned char": "Uint",
    "const unsigned char&": "Uint",
    "int": "Int",
    "bool": "Bool",
    "float": "Float",
    "double": "Double",
}
BUFFERS = {"const unsigned char*", "unsigned char*", "const void*"}
STRINGS = {"const wchar_t*": "WStr", "const char*": "Str"}
# Plain structs passed by value or reference whose bytes are recorded.
HEX_TYPES = {"CHAT_ID", "Costume", "const Vector&"}

# P3.3 (decision record 0007 point 11): the slots whose calls also go to replication.jsonl,
# each through its function of sim_client_stub_replication.h, called after the call is
# recorded with the call's seq and every argument in order.
REPLICATED = {
    "SendCommonFireWeapon": "SimStubReplFireWeapon",
    "SendCommonActivateEquip": "SimStubReplActivateEquip",
    "SendCommonActivateCruise": "SimStubReplActivateCruise",
    "SendCommonActivateThrusters": "SimStubReplActivateThrusters",
    "SendCommonSetTarget": "SimStubReplSetTarget",
    "SendCommonGoTradeLane": "SimStubReplGoTradeLane",
    "SendCommonStopTradeLane": "SimStubReplStopTradeLane",
    "SendCommonJettisonCargo": "SimStubReplJettisonCargo",
    "SendCommonUpdateObject": "SimStubReplUpdateObject",
    "SendServerCreateSolar": "SimStubReplCreateSolar",
    "SendServerCreateShip": "SimStubReplCreateShip",
    "SendServerCreateLoot": "SimStubReplCreateLoot",
    "SendServerCreateMine": "SimStubReplCreateMine",
    "SendServerCreateGuided": "SimStubReplCreateGuided",
    "SendServerCreateCounter": "SimStubReplCreateCounter",
    "SendServerSetGuidedTarget": "SimStubReplSetGuidedTarget",
    "SendServerDestroyObject": "SimStubReplDestroyObject",
    "SendServerActivateObject": "SimStubReplActivateObject",
    "SendServerSystemSwitchOut": "SimStubReplSystemSwitchOut",
    "SendServerSystemSwitchIn": "SimStubReplSystemSwitchIn",
    "SendServerLand": "SimStubReplLand",
    "SendServerLaunch": "SimStubReplLaunch",
    "SendServerDamageObject": "SimStubReplDamageObject",
    "SendServerSetReputation": "SimStubReplSetReputation",
    "SendServerBurnFuse": "SimStubReplBurnFuse",
}

# What a non-void slot answers: Startup must succeed for IServerImpl::Startup to go on.
RESULTS = {"bool": "true", "int": "1", "unsigned int": "0u", "double": "0.0"}


def record_lines(params):
    names = {name for _, name in params}
    lines = []
    for ptype, name in params:
        if ptype in SCALARS:
            lines.append(f'J.{SCALARS[ptype]}("{name}", {name});')
        elif ptype in BUFFERS:
            if "Size" not in names:
                sys.exit(f"make_sim_client_stub: byte block {name} has no Size parameter")
            lines.append(f'J.Bytes("{name}", {name}, Size);')
        elif ptype in STRINGS:
            lines.append(f'J.{STRINGS[ptype]}("{name}", {name});')
        elif ptype == "const unsigned int*":
            if "TextCount" not in names:
                sys.exit(f"make_sim_client_stub: array {name} has no count parameter")
            lines.append(f'SimStubUintArray(J, "{name}", {name}, TextCount);')
        elif ptype in HEX_TYPES:
            lines.append(f'J.Bytes("{name}", &{name}, sizeof({name}));')
        elif ptype.endswith("*"):
            lines.append(f'SimStubPresence(J, "{name}", {name} != nullptr);')
        elif ptype.endswith("&"):
            lines.append(f'SimStubPresence(J, "{name}", true);')
        else:
            sys.exit(f"make_sim_client_stub: parameter type '{ptype}' ({name}) has no recorder")
    return lines


def method_text(interface, slot, ret, name, params):
    label = name if interface == "IClient" else f"{interface}::{name}"
    decl = ", ".join(f"{ptype} {pname}" for ptype, pname in params)
    out = [f"    // {interface} slot {slot}", f"    {ret} {name}({decl}) override", "    {"]
    out.append("        FOracleJson J;")
    out.append(f'        unsigned const Seq = SimStubCallBegin(J, {slot}, "{label}");')
    for line in record_lines(params):
        out.append(f"        {line}")
    if ret == "void":
        out.append("        SimStubCallEnd(J);")
        if interface == "IClient" and name in REPLICATED:
            args = ", ".join(["Seq"] + [pname for _, pname in params])
            out.append(f"        {REPLICATED[name]}({args});")
        else:
            out.append("        (void)Seq;")
    else:
        out.append("        SimStubCallEnd(J);")
        out.append(f"        {ret} const Result = {RESULTS[ret]};")
        out.append("        FOracleJson R;")
        out.append(f'        SimStubReturnBegin(R, {slot}, "{label}", Seq);')
        out.append(f'        R.{SCALARS[ret]}("r", Result);')
        out.append("        SimStubCallEnd(R);")
        out.append("        return Result;")
    out.append("    }")
    return out


def generate():
    client = load_client_generator()
    text = client.strip_comments(client.HEADER.read_text(encoding="utf-8"))
    iclient = client.methods(client.class_body(text, "IClient"))
    ichat = client.methods(client.class_body(text, "IChatClient"))
    if len(iclient) != 126 or len(ichat) != 1:
        sys.exit(f"make_sim_client_stub: expected 126 + 1 slots, found {len(iclient)} + {len(ichat)}")
    void_slots = {name for ret, name, _ in iclient if ret == "void"}
    missing = sorted(set(REPLICATED) - void_slots)
    if missing:
        sys.exit(f"make_sim_client_stub: REPLICATED names no void IClient slot: {', '.join(missing)}")

    out = [
        "// GENERATED by make_sim_client_stub.py from CppSource/include/rpc_iclient.h -- do not edit.",
        "// Oracle instrumentation (work package P2.3, design/reference-build.md section 10).",
        "",
        "// The client interface of Oracle.exe sim's server. One static object implements both",
        "// interfaces with the exact signatures of rpc_iclient.h, so its vtables have the same",
        "// 126 + 1 slots in the same order. Every call is written to iclient.jsonl",
        "// (design/sim-records.md) and answered without a client: void slots do nothing, Startup",
        "// returns true, Update 1, the queue queries 0. The slots of the replication comparison",
        "// (P3.3) also go to replication.jsonl (sim_client_stub_replication.cpp).",
        "",
        '#include "rpc_iclient.h"',
        '#include "common_ini.h"           // Vector',
        '#include "common_reputation.h"    // Costume (by value)',
        '#include "common_server_types.h"  // CHAT_ID (by value)',
        "",
        '#include "sim_client_stub.h"',
        '#include "sim_client_stub_replication.h"',
        "",
        "#include <list>",
        "#include <string>",
        "#include <vector>",
        "",
        "namespace",
        "{",
        "class FSimClientStub final : public IClient, public IChatClient",
        "{",
        "public:",
    ]
    for slot, (ret, name, params) in enumerate(iclient):
        out.extend(method_text("IClient", slot, ret, name, params))
        out.append("")
    for slot, (ret, name, params) in enumerate(ichat):
        out.extend(method_text("IChatClient", slot, ret, name, params))
    out += [
        "};",
        "",
        "FSimClientStub G_SimClientStub;",
        "}",
        "",
        "IClient* SimStubClient()",
        "{",
        "    return static_cast<IClient*>(&G_SimClientStub);",
        "}",
        "",
        "IChatClient* SimStubChatClient()",
        "{",
        "    return static_cast<IChatClient*>(&G_SimClientStub);",
        "}",
        "",
    ]
    return "\n".join(out)


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--check", action="store_true", help="compare instead of writing")
    options = parser.parse_args()
    text = generate()
    if options.check:
        current = OUTPUT.read_text(encoding="utf-8") if OUTPUT.exists() else ""
        if current != text:
            print(f"{OUTPUT} is out of date; run make_sim_client_stub.py", file=sys.stderr)
            return 1
        return 0
    OUTPUT.write_text(text, encoding="utf-8", newline="\n")
    print(f"wrote {OUTPUT}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
