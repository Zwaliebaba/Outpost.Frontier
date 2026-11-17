#!/usr/bin/env python3
"""Generate oracle_iclient_proxy.cpp, the IClient logging proxy of the client's +oracle mode.

Oracle instrumentation, work package P0.5 (design/reference-build.md section 8).

Reads the IClient and IChatClient declarations in CppSource/include/rpc_iclient.h and
writes one class that implements both, records every call into iclient.jsonl through
oracle_iclient_args.h, and forwards it unchanged to the real client. Run it from any
directory after rpc_iclient.h changes and commit the result:

    python3 CppSource/OutpostFrontier/Client/make_oracle_iclient.py

With --check it writes nothing and exits 1 when the committed file differs.
"""

import argparse
import pathlib
import re
import sys

HERE = pathlib.Path(__file__).resolve().parent
HEADER = HERE.parent.parent / "include" / "rpc_iclient.h"
OUTPUT = HERE / "oracle_iclient_proxy.cpp"

# Scalars: the FOracleJson call that records them.
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

# Byte blocks: the pointer types, sized by the parameter named Size.
BUFFERS = {"const unsigned char*", "unsigned char*", "const void*"}

# Strings.
STRINGS = {"const wchar_t*": "WStr", "const char*": "Str"}

# Every other type goes to an OracleArg overload; the set must match oracle_iclient_args.h.
OVERLOADS = {
    "const XFireWeaponInfo&", "const XActivateEquip&", "const XActivateCruise&",
    "const XActivateThrusters&", "const XSetTarget&", "const XTractorObjects&",
    "const XGoTradelane&", "const XJettisonCargo&", "const CCharacterSet&",
    "const FServerCharSelectVerifiedInfo&", "const std::list<CollisionGroupDesc>&",
    "const EquipDescVector&", "const EquipDesc&", "const CSolarCreateInfo&",
    "const CShipCreateInfo&", "const FServerCreateLootRequest&",
    "const FServerCreateProjectileInfo&", "const FServerCreateGuidedInfo&",
    "const FMineExplosion&", "const FCommonUpdateObjectInfo&",
    "const FServerDestroyObjectInfo&", "const FPayloadServerActivateObject&",
    "const FServerSystemSwitchOutInfo&", "const FPayloadServerSystemSwitchIn&",
    "const FServerLandInfo&", "const FServerLaunchInfo&",
    "const FServerRequestSpaceScriptInfo&", "const DamageList&",
    "const FPayloadServerItemTractored&", "const FPayloadServerUseItem&",
    "const FServerSetReputationInfo&", "const FPayloadServerAdjustAttitude&", "Costume",
    "const CSetMissionMessage&", "const FServerReplaceMissionObjectiveInfo&",
    "const FServerSetMissionObjectivesInfo&", "const FServerMissionObjectiveStateInfo&",
    "const FPayloadServerCfgInterfaceNotification&", "BaseHint*",
    "const FServerSetGroupFeelingsInfo&", "const FServerRepairObjectInfo&",
    "const FServerBurnFuseInfo&", "const FPayloadServerSetCamera&",
    "const FPayloadServerSetDirective&", "const std::wstring&",
    "const std::vector<unsigned int>&", "const Vector&", "CHAT_ID",
}

# Types the client cannot measure here: recorded as "present" (null for a null pointer).
OPAQUE_REFS = {"const CCharacterSet&", "const FmtStr&"}
OPAQUE_POINTERS = {"CDAPacket*"}

RETURNS = {"void", "bool", "int", "unsigned int", "double"}


def strip_comments(text):
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    return re.sub(r"//[^\n]*", " ", text)


def class_body(text, name):
    match = re.search(r"\bclass\s+(?:RPC_ICLIENT_API\s+)?" + name + r"\s*\{", text)
    if match is None:
        sys.exit(f"make_oracle_iclient: class {name} not found in {HEADER}")
    depth = 1
    pos = match.end()
    while depth:
        if text[pos] == "{":
            depth += 1
        elif text[pos] == "}":
            depth -= 1
        pos += 1
    return text[match.end():pos - 1]


def split_params(params):
    result, depth, start = [], 0, 0
    for index, char in enumerate(params):
        if char == "<":
            depth += 1
        elif char == ">":
            depth -= 1
        elif char == "," and depth == 0:
            result.append(params[start:index])
            start = index + 1
    if params.strip():
        result.append(params[start:])
    out = []
    for param in result:
        param = " ".join(param.split())
        match = re.match(r"^(.*?)([A-Za-z_][A-Za-z0-9_]*)$", param)
        if match is None:
            sys.exit(f"make_oracle_iclient: cannot read parameter '{param}'")
        ptype = match.group(1).strip()
        ptype = re.sub(r"\s+([&*])", r"\1", ptype)
        out.append((ptype, match.group(2)))
    return out


def methods(body):
    found = []
    for match in re.finditer(r"virtual\s+([^;{}]*?)\s*=\s*0\s*;", body, flags=re.S):
        decl = " ".join(match.group(1).split())
        head = re.match(r"^(.*?)\b([A-Za-z_][A-Za-z0-9_]*)\s*\((.*)\)$", decl)
        if head is None:
            sys.exit(f"make_oracle_iclient: cannot read declaration '{decl}'")
        ret = head.group(1).strip()
        if ret not in RETURNS:
            sys.exit(f"make_oracle_iclient: return type '{ret}' of {head.group(2)} not handled")
        found.append((ret, head.group(2), split_params(head.group(3))))
    return found


def record_lines(params):
    names = {name for _, name in params}
    lines = []
    for ptype, name in params:
        if ptype in SCALARS:
            lines.append(f'J.{SCALARS[ptype]}("{name}", {name});')
        elif ptype in BUFFERS:
            if "Size" not in names:
                sys.exit(f"make_oracle_iclient: byte block {name} has no Size parameter")
            lines.append(f'J.Bytes("{name}", {name}, Size);')
        elif ptype in STRINGS:
            lines.append(f'J.{STRINGS[ptype]}("{name}", {name});')
        elif ptype == "const unsigned int*":
            if "TextCount" not in names:
                sys.exit(f"make_oracle_iclient: array {name} has no count parameter")
            lines.append(f'OracleArgUintArray(J, "{name}", {name}, TextCount);')
        elif ptype in OPAQUE_REFS:
            lines.append(f'OracleArgPresence(J, "{name}", true);')
        elif ptype in OPAQUE_POINTERS:
            lines.append(f'OracleArgPresence(J, "{name}", {name} != nullptr);')
        elif ptype in OVERLOADS:
            lines.append(f'OracleArg(J, "{name}", {name});')
        else:
            sys.exit(f"make_oracle_iclient: parameter type '{ptype}' ({name}) has no recorder; "
                     "add an OracleArg overload and list it in OVERLOADS")
    return lines


def method_text(interface, slot, ret, name, params, real):
    # IClient records carry the bare method name; IChatClient's one slot is prefixed, so
    # "slot" stays the index within the record's own interface.
    label = name if interface == "IClient" else f"{interface}::{name}"
    decl = ", ".join(f"{ptype} {pname}" for ptype, pname in params)
    args = ", ".join(pname for _, pname in params)
    out = [f"    // {interface} slot {slot}", f"    {ret} {name}({decl}) override", "    {"]
    out.append("        unsigned Seq = 0;")
    out.append("        {")
    out.append("            FOracleJson J;")
    out.append(f'            Seq = OracleIClientBegin(J, {slot}, "{label}");')
    for line in record_lines(params):
        out.append(f"            {line}")
    out.append("            OracleIClientEnd(J);")
    out.append("        }")
    if ret == "void":
        out.append(f"        {real}->{name}({args});")
        out.append("        (void)Seq;")
    else:
        out.append(f"        {ret} const Result = {real}->{name}({args});")
        out.append("        FOracleJson R;")
        out.append(f'        OracleIClientReturnBegin(R, {slot}, "{label}", Seq);')
        out.append(f'        R.{SCALARS[ret]}("r", Result);')
        out.append("        OracleIClientReturnEnd(R);")
        out.append("        return Result;")
    out.append("    }")
    return out


def generate():
    text = strip_comments(HEADER.read_text(encoding="utf-8"))
    iclient = methods(class_body(text, "IClient"))
    ichat = methods(class_body(text, "IChatClient"))
    if len(iclient) != 126 or len(ichat) != 1:
        sys.exit(f"make_oracle_iclient: expected 126 + 1 slots, found {len(iclient)} + {len(ichat)}")

    out = [
        "// GENERATED by make_oracle_iclient.py from CppSource/include/rpc_iclient.h -- do not edit.",
        "// Oracle instrumentation (work package P0.5, design/reference-build.md section 8).",
        "",
        "// The IClient / IChatClient logging proxy of the client's +oracle mode. One static object",
        "// implements both interfaces with the exact signatures of rpc_iclient.h, so its vtables have",
        "// the same 126 + 1 slots in the same order. Every call is written to iclient.jsonl",
        "// (oracle_iclient_args.h has the record layout) and then forwarded unchanged to the real",
        "// object GetClientInterface / GetChatClientInterface would have returned.",
        "",
        "// Every argument type the records read is complete here (the includes of",
        "// oracle_iclient_args.cpp), so each OracleArg call resolves against complete types.",
        '#include "OutpostFrontierClient.h"',
        '#include "common_archetype.h"',
        '#include "common_cobject.h"',
        '#include "common_goods.h"',
        '#include "common_misc.h"',
        '#include "common_reputation.h"',
        '#include "common_server_types.h"',
        '#include "common_text.h"',
        '#include "rpc_iclient_args.h"',
        '#include "server_createinfo.h"',
        '#include "server_sysdb.h"',
        "",
        '#include "oracle_iclient_args.h"',
        '#include "oracle_json.h"',
        '#include "oracle_mode.h"',
        "",
        "#include <list>",
        "#include <string>",
        "#include <vector>",
        "",
        "namespace",
        "{",
        "class FOracleClientProxy final : public IClient, public IChatClient",
        "{",
        "public:",
        "    FOracleClientProxy() : RealClient(nullptr), RealChat(nullptr) {}",
        "",
        "    IClient* RealClient;",
        "    IChatClient* RealChat;",
        "",
    ]
    for slot, (ret, name, params) in enumerate(iclient):
        out.extend(method_text("IClient", slot, ret, name, params, "RealClient"))
        out.append("")
    for slot, (ret, name, params) in enumerate(ichat):
        out.extend(method_text("IChatClient", slot, ret, name, params, "RealChat"))
    out += [
        "};",
        "",
        "FOracleClientProxy G_OracleClientProxy;",
        "}",
        "",
        "IClient* OracleClientProxyFor(IClient* Real)",
        "{",
        "    G_OracleClientProxy.RealClient = Real;",
        "    return static_cast<IClient*>(&G_OracleClientProxy);",
        "}",
        "",
        "IChatClient* OracleChatClientProxyFor(IChatClient* Real)",
        "{",
        "    G_OracleClientProxy.RealChat = Real;",
        "    return static_cast<IChatClient*>(&G_OracleClientProxy);",
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
            print(f"{OUTPUT} is out of date; run make_oracle_iclient.py", file=sys.stderr)
            return 1
        return 0
    OUTPUT.write_text(text, encoding="utf-8", newline="\n")
    print(f"wrote {OUTPUT}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
