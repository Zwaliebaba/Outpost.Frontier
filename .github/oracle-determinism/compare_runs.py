"""TEMPORARY (branch claude/oracle-determinism): repeated runs of one build on one machine.

Layout: <root>/<mode>/<scenario>_<seed>/run_NN/*.jsonl (Oracle.exe sim --runs N). For each
mode and pair: each run's env (heap, stack, module bases), the runs grouped by their
records, and for every run that differs from run_01 the first differing records of each
file with their fields. The personality records' bytes are compared apart: their padding
holds whatever the stack held, so only the differing byte offsets are listed.
"""

import hashlib
import json
import os
import struct
import sys
from collections import defaultdict

SKIP = {"session.jsonl", "load_rand.jsonl", "iclient.jsonl"}
MAX_DIFFS_PER_FILE = 3


def decode_hex(value):
    if isinstance(value, str) and len(value) in (8, 16):
        try:
            bits = int(value, 16)
        except ValueError:
            return None
        if len(value) == 8:
            return struct.unpack("<f", struct.pack("<I", bits))[0]
        return struct.unpack("<d", struct.pack("<Q", bits))[0]
    return None


def show(value):
    text = json.dumps(value)
    number = decode_hex(value)
    if number is not None:
        text += " (%r)" % number
    return text if len(text) < 300 else text[:300] + "..."


def field_diffs(a, b, path, out):
    if len(out) >= 30:
        return
    if isinstance(a, dict) and isinstance(b, dict):
        for key in list(a.keys()) + [k for k in b.keys() if k not in a]:
            field_diffs(a.get(key), b.get(key), path + "." + key, out)
    elif isinstance(a, list) and isinstance(b, list) and len(a) == len(b):
        for index, (x, y) in enumerate(zip(a, b)):
            field_diffs(x, y, "%s[%d]" % (path, index), out)
    elif a != b:
        out.append("%s: %s | %s" % (path, show(a), show(b)))


def parse(line):
    try:
        return json.loads(line)
    except ValueError:
        return None


def normalise(line, padding):
    """The line with a personality record's bytes removed (their differing offsets are
    collected in `padding`, keyed by the line number)."""
    if '"rec":"personality"' not in line:
        return line, None
    rec = parse(line)
    if not isinstance(rec, dict):
        return line, None
    data = rec.pop("bytes", None)
    return json.dumps(rec, sort_keys=False), data


def read_lines(path):
    with open(path, errors="replace") as handle:
        return [line.rstrip("\n") for line in handle]


def env_of(run_dir):
    path = os.path.join(run_dir, "session.jsonl")
    if not os.path.exists(path):
        return None
    for line in read_lines(path):
        rec = parse(line)
        if rec and rec.get("rec") == "env":
            return rec
    return None


def first_contact(run_dir):
    path = os.path.join(run_dir, "events.jsonl")
    if not os.path.exists(path):
        return None
    for line in read_lines(path):
        rec = parse(line)
        if rec and rec.get("rec") in ("munition_hit", "collision"):
            return rec.get("tick")
    return None


def compare_pair(pair_dir):
    runs = sorted(d for d in os.listdir(pair_dir) if d.startswith("run_"))
    if not runs:
        print("  no runs")
        return
    files = sorted(f for f in os.listdir(os.path.join(pair_dir, runs[0])) if f.endswith(".jsonl") and f not in SKIP)
    content = {}
    personality = {}
    for run in runs:
        for name in files:
            path = os.path.join(pair_dir, run, name)
            lines = read_lines(path) if os.path.exists(path) else []
            norm = []
            for number, line in enumerate(lines):
                text, data = normalise(line, None)
                norm.append(text)
                if data is not None:
                    personality[(run, name, number)] = data
            content[(run, name)] = norm

    for run in runs:
        env = env_of(os.path.join(pair_dir, run)) or {}
        mods = {m.get("name"): m.get("base") for m in env.get("modules", []) if m.get("loaded")}
        digest = hashlib.sha256("\n".join("\n".join(content[(run, n)]) for n in files).encode()).hexdigest()[:12]
        print("  %s: records %s first contact %s heap %s stack %s Oracle %s Server %s Common %s ucrt %s"
              % (run, digest, first_contact(os.path.join(pair_dir, run)), env.get("process_heap"),
                 env.get("stack_local"), mods.get("Oracle.exe"), mods.get("Server.dll"), mods.get("Common.dll"),
                 mods.get("ucrtbase.dll")))

    offsets = set()
    for (run, name, number), data in personality.items():
        ref = personality.get((runs[0], name, number))
        if ref is None or ref == data:
            continue
        for index in range(0, min(len(ref), len(data)), 2):
            if ref[index:index + 2] != data[index:index + 2]:
                offsets.add(index // 2)
    listed = ", ".join("0x%x" % o for o in sorted(offsets))
    print("  personality bytes that differ between runs: %s" % (listed or "none"))

    ref = runs[0]
    for run in runs[1:]:
        found = []
        for name in files:
            a = content[(ref, name)]
            b = content[(run, name)]
            diffs = []
            for number in range(max(len(a), len(b))):
                la = a[number] if number < len(a) else ""
                lb = b[number] if number < len(b) else ""
                if la != lb:
                    diffs.append(number)
                    if len(diffs) >= MAX_DIFFS_PER_FILE:
                        break
            if diffs:
                rec = parse(a[diffs[0]]) if diffs[0] < len(a) else None
                tick = rec.get("tick") if isinstance(rec, dict) else None
                found.append((tick if tick is not None else 1 << 30, name, diffs))
        if not found:
            print("  %s: every record equal to %s" % (run, ref))
            continue
        found.sort()
        print("  %s differs from %s:" % (run, ref))
        for index, (tick, name, diffs) in enumerate(found):
            a = content[(ref, name)]
            b = content[(run, name)]
            print("    %s: first at line %d, tick %s" % (name, diffs[0] + 1, tick))
            if index >= 3:
                continue
            if index == 0:
                for line in a[max(0, diffs[0] - 4):diffs[0]]:
                    print("      before: " + line[:300])
            for number in diffs:
                ra = parse(a[number]) if number < len(a) else None
                rb = parse(b[number]) if number < len(b) else None
                if isinstance(ra, dict) and isinstance(rb, dict):
                    out = []
                    field_diffs(ra, rb, "", out)
                    head = " ".join("%s=%s" % (k, ra.get(k)) for k in ("rec", "tick", "seq", "beh", "obj", "id", "slot", "m", "kind") if k in ra)
                    print("      line %d %s" % (number + 1, head))
                    for item in out:
                        print("        " + item)
                else:
                    print("      line %d ref:   %s" % (number + 1, (a[number] if number < len(a) else "")[:400]))
                    print("      line %d other: %s" % (number + 1, (b[number] if number < len(b) else "")[:400]))


def main(root):
    for mode in sorted(os.listdir(root)):
        mode_dir = os.path.join(root, mode)
        if not os.path.isdir(mode_dir):
            continue
        for pair in sorted(os.listdir(mode_dir)):
            pair_dir = os.path.join(mode_dir, pair)
            if os.path.isdir(pair_dir):
                print("\n== %s %s" % (mode, pair))
                compare_pair(pair_dir)


if __name__ == "__main__":
    main(sys.argv[1])
