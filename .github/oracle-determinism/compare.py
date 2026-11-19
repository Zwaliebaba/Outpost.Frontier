"""TEMPORARY (branch claude/oracle-determinism): the oracle reproducibility probe's comparison.

Reads the probe artifacts (probe-<machine>/<toolset>/seed_<S>/<scenario>/*.jsonl and
probe-<machine>/hashes.txt), prints each run's environment, which runs agree, and for
every run that differs from the reference (the first machine, the latest toolset) the
first differing record of each file with the fields that differ.
"""

import json
import os
import struct
import sys
from collections import defaultdict


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
    return text if len(text) < 400 else text[:400] + "..."


def field_diffs(a, b, path, out):
    if len(out) >= 24:
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


def first_contact(path):
    try:
        with open(path) as handle:
            for line in handle:
                rec = parse(line)
                if rec and rec.get("rec") in ("munition_hit", "collision"):
                    return rec.get("tick")
    except OSError:
        pass
    return None


def compare_file(ref_path, other_path):
    """Returns None when equal, else (line number, tick, ref line, other line)."""
    with open(ref_path, errors="replace") as ref, open(other_path, errors="replace") as other:
        number = 0
        last_tick = None
        while True:
            a = ref.readline()
            b = other.readline()
            number += 1
            if not a and not b:
                return None
            if a != b:
                ra = parse(a) if a else None
                tick = ra.get("tick") if isinstance(ra, dict) else None
                return number, tick if tick is not None else last_tick, a.rstrip("\n"), b.rstrip("\n")
            rec = parse(a)
            if isinstance(rec, dict) and "tick" in rec:
                last_tick = rec["tick"]


def env_of(run_dir):
    try:
        with open(os.path.join(run_dir, "session.jsonl")) as handle:
            for line in handle:
                rec = parse(line)
                if rec and rec.get("rec") == "env":
                    return rec
    except OSError:
        pass
    return None


def main(root):
    machines = sorted(d for d in os.listdir(root) if d.startswith("probe-"))
    runs = {}  # (machine, toolset, seed, scenario) -> dir
    for machine in machines:
        mdir = os.path.join(root, machine)
        for toolset in sorted(os.listdir(mdir)):
            tdir = os.path.join(mdir, toolset)
            if not os.path.isdir(tdir):
                continue
            for seed_dir in sorted(os.listdir(tdir)):
                for scenario in sorted(os.listdir(os.path.join(tdir, seed_dir))):
                    runs[(machine, toolset, seed_dir, scenario)] = os.path.join(tdir, seed_dir, scenario)

    print("== environments")
    seen = set()
    for (machine, toolset, _, _), run_dir in sorted(runs.items()):
        if (machine, toolset) in seen:
            continue
        seen.add((machine, toolset))
        env = env_of(run_dir)
        if env is None:
            print("%s %s: no env record" % (machine, toolset))
            continue
        cpu = env.get("cpu", {})
        mods = {m.get("name"): m for m in env.get("modules", [])}
        print("%s %s: msc %s.%s | %s | %s sig %s ecx %s edx %s l7ebx %s | os %s.%s | heap %s"
              % (machine, toolset, env.get("msc_full_ver"), env.get("msc_build"), cpu.get("brand", "").strip(),
                 cpu.get("vendor"), cpu.get("signature"), cpu.get("leaf1_ecx"), cpu.get("leaf1_edx"),
                 cpu.get("leaf7_ebx"), env.get("os", {}).get("build"), env.get("os", {}).get("ubr"),
                 env.get("process_heap")))
        print("    " + " ".join("%s@%s v%s" % (n, m.get("base"), m.get("version")) for n, m in mods.items() if m.get("loaded")))

    print("\n== hash classes (single = the ensemble step's command line, run_NN = --runs 3)")
    classes = defaultdict(dict)
    for machine in machines:
        path = os.path.join(root, machine, "hashes.txt")
        if not os.path.exists(path):
            continue
        with open(path, encoding="utf-8-sig") as handle:
            for line in handle:
                parts = line.split()
                if len(parts) != 6:
                    continue
                toolset, scenario, seed, run, name, digest = parts
                classes[(scenario, seed, name)]["%s/%s/%s" % (machine[6:], toolset, run)] = digest
    for key in sorted(classes):
        letters = {}
        row = []
        for run, digest in sorted(classes[key].items()):
            letters.setdefault(digest, chr(ord("A") + len(letters)))
            row.append("%s=%s" % (run, letters[digest]))
        print("%s %s %s: %d class(es): %s" % (key[0], key[1], key[2], len(letters), " ".join(row)))

    print("\n== first differences from the reference")
    by_pair = defaultdict(list)
    for (machine, toolset, seed, scenario) in runs:
        by_pair[(seed, scenario)].append((machine, toolset))
    for (seed, scenario), members in sorted(by_pair.items()):
        members.sort(key=lambda m: (m[1] != "latest", m[0], m[1]))
        ref = members[0]
        ref_dir = runs[(ref[0], ref[1], seed, scenario)]
        print("\n-- %s %s: reference %s/%s, first contact tick %s"
              % (scenario, seed, ref[0], ref[1], first_contact(os.path.join(ref_dir, "events.jsonl"))))
        for other in members[1:]:
            other_dir = runs[(other[0], other[1], seed, scenario)]
            found = []
            for name in sorted(os.listdir(ref_dir)):
                if not name.endswith(".jsonl") or name == "session.jsonl":
                    continue
                other_path = os.path.join(other_dir, name)
                if not os.path.exists(other_path):
                    continue
                result = compare_file(os.path.join(ref_dir, name), other_path)
                if result is not None:
                    found.append((name, result))
            if not found:
                print("  %s/%s: every record equal" % other)
                continue
            print("  %s/%s: differs (first contact tick %s)"
                  % (other[0], other[1], first_contact(os.path.join(other_dir, "events.jsonl"))))
            found.sort(key=lambda item: (item[1][1] if item[1][1] is not None else 1 << 30))
            for name, (number, tick, a, b) in found:
                print("    %s line %d tick %s" % (name, number, tick))
                ra, rb = parse(a), parse(b)
                if isinstance(ra, dict) and isinstance(rb, dict):
                    diffs = []
                    field_diffs(ra, rb, "", diffs)
                    print("      rec %s obj %s seq %s" % (ra.get("rec"), ra.get("obj"), ra.get("seq")))
                    for diff in diffs:
                        print("      " + diff)
                else:
                    print("      ref:   " + a[:600])
                    print("      other: " + b[:600])


if __name__ == "__main__":
    main(sys.argv[1])
