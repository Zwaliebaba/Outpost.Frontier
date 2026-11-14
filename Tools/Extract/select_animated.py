#!/usr/bin/env python3
"""Choose the animated objects for the FBX pass, from an arranged DoK-Assets folder.

  select_animated.py ASSETS

Writes ASSETS/_logs/fbx-selection.json and ASSETS/_logs/fbx-pathids.txt (the PathID filter for
AssetStudioModCLI's animator mode). Selected:
- every Animator in a shared or Common file: the prefabs (units, wrecks, props);
- every Animator in a scene file whose name no prefab has, if it has an animation controller or a
  unit-like name: multiplayer and cinematic unit versions, landers, cranes, turbines, salvage.
The ~5,400 others are scenery with an idle Animator; their meshes are in the OBJ export.
"""

import json
import re
import sys
from pathlib import Path

UNIT_LIKE = re.compile(r'^(unit|c_|g_|gaalsi|coalition|kiith|nis|carrier)', re.I)
SUFFIX = re.compile(r'(\s*\(\d+\)|\(Clone\)|\s+\d+)+$')


def animators(root, location):
  path = root / location / 'objects' / 'Animator.jsonl'
  if not path.exists():
    return
  with open(path, encoding='utf-8') as f:
    for line in f:
      yield json.loads(line)


def main():
  root = Path(sys.argv[1])
  manifest = json.loads((root / 'manifest.json').read_text(encoding='utf-8'))
  files = manifest['files']
  rows, prefab_names, seen = [], set(), set()
  for key, f in files.items():
    location = f['location'] or ''
    if location.endswith('/scene'):
      continue
    for j in animators(root, location):
      base = SUFFIX.sub('', j['name']).strip()
      if base in prefab_names:
        continue
      prefab_names.add(base)
      rows.append({'name': base, 'file': key, 'pathID': j['pathID'], 'kind': 'prefab',
                   'controller': (j['data'].get('m_Controller') or {}).get('Name', '')})
  for key, f in files.items():
    location = f['location'] or ''
    if not location.endswith('/scene'):
      continue
    for j in animators(root, location):
      d = j['data']
      has_controller = not (d.get('m_Controller') or {}).get('IsNull', True)
      base = SUFFIX.sub('', j['name']).strip()
      if base in prefab_names or base in seen or not (has_controller or UNIT_LIKE.match(base)):
        continue
      seen.add(base)
      rows.append({'name': base, 'file': key, 'pathID': j['pathID'], 'kind': 'scene',
                   'controller': (d.get('m_Controller') or {}).get('Name', '')})
  logs = root / '_logs'
  logs.mkdir(exist_ok=True)
  (logs / 'fbx-selection.json').write_text(json.dumps(rows, indent=1, ensure_ascii=False), encoding='utf-8')
  (logs / 'fbx-pathids.txt').write_text(','.join(sorted({str(r['pathID']) for r in rows})), encoding='utf-8')
  print(f'{len(rows)} animated objects selected ({len(prefab_names)} prefabs, {len(seen)} scene objects)')


if __name__ == '__main__':
  main()
