#!/usr/bin/env python3
"""Compare Recorder traces of the same game (Design/Plan.md 1.1, Verification.md section 1).

  compare_traces.py TRACE [TRACE ...]

Each trace is one game session: per tick the frame, the checksum the simulation computed, the
checksum a replay expected, the scheduler's result and the commands. Reports, per trace, its
frames, mode and any SyncError or computed-versus-expected difference (in a replay, the
expected checksums are the ones recorded when the game was played); then, between the first
trace and each other one, the first frame whose checksum or commands differ.

Exit 0 when every trace agrees with itself and with the first; 1 otherwise.
"""

import sys
from pathlib import Path


def read(path):
  frames, meta = {}, []
  for line in Path(path).read_text(encoding='utf-8').splitlines():
    if line.startswith('#'):
      meta.append(line[1:].strip())
      continue
    cols = line.split('\t')
    if len(cols) < 6:
      continue
    frame, checksum, expected, result, mode, commands = cols[:6]
    frames[int(frame)] = {'checksum': int(checksum), 'expected': None if expected == '-' else int(expected),
                          'result': result, 'mode': mode, 'commands': commands}
  return frames, meta


def main():
  paths = sys.argv[1:]
  if not paths:
    sys.exit(__doc__)
  traces = [(p, *read(p)) for p in paths]
  ok = True
  for path, frames, meta in traces:
    modes = sorted({f['mode'] for f in frames.values()})
    with_commands = sum(1 for f in frames.values() if f['commands'] != '-')
    sync_errors = [n for n, f in frames.items() if f['result'] == 'SyncError']
    mismatches = [n for n, f in frames.items() if f['expected'] is not None and f['expected'] != f['checksum']]
    checked = sum(1 for f in frames.values() if f['expected'] is not None)
    print(f'{path}')
    print(f'  {len(frames)} frames ({min(frames, default=0)}-{max(frames, default=0)}), mode {"/".join(modes)}, '
          f'{with_commands} with commands; {checked} checked against recorded checksums')
    if sync_errors or mismatches:
      ok = False
      first = min(sync_errors + mismatches)
      print(f'  DIFFERS from its recorded checksums: {len(mismatches)} frames, SyncError at {len(sync_errors)}; '
            f'first at frame {first}')
  base_path, base, _ = traces[0]
  for path, frames, _ in traces[1:]:
    common = sorted(set(base) & set(frames))
    first = next((n for n in common if base[n]['checksum'] != frames[n]['checksum']), None)
    first_cmd = next((n for n in common if base[n]['commands'] != frames[n]['commands']), None)
    print(f'{Path(path).name} vs {Path(base_path).name}: {len(common)} common frames; ', end='')
    if first is None and first_cmd is None:
      print('identical checksums and commands')
    else:
      ok = False
      if first is not None:
        print(f'checksums differ from frame {first} ({base[first]["checksum"]} vs {frames[first]["checksum"]})', end='; ')
      if first_cmd is not None:
        print(f'commands differ from frame {first_cmd}', end='')
      print()
  return 0 if ok else 1


if __name__ == '__main__':
  sys.exit(main())
