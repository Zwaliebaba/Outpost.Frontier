#!/usr/bin/env python3
"""Arrange the FBX export (tools/export_fbx.ps1) under Models/, one folder per model.

  organize_fbx.py ROOT

Each model folder keeps the textures its FBX refers to, but a texture identical to one already
exported under Scenes/ or Common/ becomes a hard link to it, so the copies take no space.
Writes Models/index.json: each model's source file, PathID, kind (prefab or scene object) and
animation controller, from _logs/fbx-selection.json.
"""

import hashlib
import json
import os
import re
import shutil
import sys
from collections import defaultdict
from pathlib import Path


def sha1(path):
  h = hashlib.sha1()
  with open(path, 'rb') as f:
    for block in iter(lambda: f.read(1 << 20), b''):
      h.update(block)
  return h.hexdigest()


def main():
  root = Path(sys.argv[1])
  raw, models = root / '_raw_fbx', root / 'Models'
  selection = json.load(open(root / '_logs' / 'fbx-selection.json', encoding='utf-8'))
  by_pid = {r['pathID']: r for r in selection}
  by_name = defaultdict(list)
  for r in selection:
    by_name[r['name']].append(r)

  # Existing textures by size, hashed on demand.
  by_size = defaultdict(list)
  for base in (root / 'Scenes', root / 'Common'):
    for p in base.rglob('*.png'):
      by_size[p.stat().st_size].append(p)
  hashes = {}

  def existing(p):
    for q in by_size.get(p.stat().st_size, ()):
      if q not in hashes:
        hashes[q] = sha1(q)
      if hashes[q] == sha1(p):
        return q
    return None

  index, linked, saved, copies, used = [], 0, 0, 0, set()
  for fbx in sorted(raw.rglob('*.fbx')):
    src = fbx.parent
    m = re.search(r' #(-?\d+)$', src.name)
    pid = int(m.group(1)) if m else None
    name = fbx.stem
    entry = by_pid.get(pid) if pid is not None else None
    if entry is None:
      # Instances carry a suffix the selection does not: "Name (1)", "Name 37".
      base = re.sub(r'(\s*\(\d+\)|\s+\d+)$', '', name)
      candidates = [r for r in by_name.get(name, []) + by_name.get(base, [])
                    if (r['file'], r['pathID']) not in used]
      entry = candidates[0] if candidates else None
    if entry is not None:
      used.add((entry['file'], entry['pathID']))
    folder = name if not (models / name).exists() else f'{name} #{pid if pid is not None else len(index)}'
    dest = models / folder
    dest.parent.mkdir(parents=True, exist_ok=True)
    shutil.move(str(src), str(dest))
    textures = 0
    for png in dest.glob('*.png'):
      textures += 1
      same = existing(png)
      if same is not None:
        size = png.stat().st_size
        png.unlink()
        os.link(same, png)
        linked += 1
        saved += size
      else:
        copies += 1
    index.append({'name': name, 'folder': f'Models/{folder}', 'fbx': f'Models/{folder}/{fbx.name}',
                  'textures': textures,
                  'source_file': entry['file'] if entry else None,
                  'pathID': entry['pathID'] if entry else pid,
                  'kind': entry['kind'] if entry else 'extra (same PathID in another file)',
                  'controller': entry.get('controller') if entry else None})
  (models / 'index.json').write_text(json.dumps(index, indent=1, ensure_ascii=False), encoding='utf-8')
  left = [p for p in raw.rglob('*') if p.is_file()]
  if not left:
    shutil.rmtree(raw)
  print(f'{len(index)} models; {linked} textures hard-linked ({saved / 2**30:.1f} GB not duplicated), '
        f'{copies} kept as files; {len(left)} files left in _raw_fbx')
  missing = [r['name'] for r in selection if not any(i['source_file'] == r['file'] and i['pathID'] == r['pathID'] for i in index)]
  print(f'{len(missing)} selected objects without an FBX: {missing[:20]}')


if __name__ == '__main__':
  main()
