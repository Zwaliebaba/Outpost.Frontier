#!/usr/bin/env python3
"""Arrange AssetStudioModCLI's per-file export (tools/export.ps1) per scene, and write manifest.json.

  organize.py DATA_DIR ROOT

DATA_DIR is DesertsOfKharak_Data, ROOT the DoK-Assets folder holding _raw/content and
_raw/objects. Files are moved, not copied; GI data is copied from the game. See README.md.
"""

import json
import os
import re
import shutil
import sys
import xml.etree.ElementTree as ET
from collections import Counter, defaultdict
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import unity_externals  # noqa: E402

PATH_ID = re.compile(r' @(-?\d+)(\.[^.]+)?$')
BUILTIN = {'library/unity default resources': 'Resources/unity default resources',
           'resources/unity_builtin_extra': 'Resources/unity_builtin_extra',
           'unity default resources': 'Resources/unity default resources',
           'unity_builtin_extra': 'Resources/unity_builtin_extra'}


def file_key(name):
  """A serialized file as the manifest names it (its path under DesertsOfKharak_Data)."""
  name = name.replace('\\', '/')
  low = name.lower()
  for k, v in BUILTIN.items():
    if low.endswith(k):
      return v
  return name.split('/')[-1]


def safe(name):
  return re.sub(r'[<>:"/\\|?*\x00-\x1f]', '_', name).strip() or '_'


def asset_list(xml_path):
  """{(file key, path id): (type, name)} from an --export-asset-list xml."""
  out = {}
  if not xml_path.exists():
    return out
  for _, el in ET.iterparse(xml_path):
    if el.tag != 'Asset':
      continue
    source = file_key(el.findtext('Source') or '')
    out[(source, int(el.findtext('PathID')))] = (el.findtext('Type') or 'Other', el.findtext('Name') or '')
    el.clear()
  return out


def export_dirs(base):
  """{file key: its '<file>_export' folder}."""
  out = {}
  for d in base.rglob('*_export'):
    if d.is_dir():
      out[file_key(d.name[:-len('_export')])] = d
  return out


def main():
  data, root = Path(sys.argv[1]), Path(sys.argv[2])
  raw = root / '_raw'

  # Files, their dependency tables and object counts.
  names = (['mainData', 'resources.assets'] + sorted((p.name for p in data.glob('level*')), key=lambda n: int(n[5:])) +
           sorted((p.name for p in data.glob('sharedassets*.assets')), key=lambda n: int(n[12:-7])) +
           ['Resources/unity default resources', 'Resources/unity_builtin_extra'])
  files = {}
  for n in names:
    info = unity_externals.read(data / n)
    files[n] = {'externals': [file_key(e) for e in info['externals']],
                'objects': dict(info['objects'].most_common()), 'object_count': info['object_count']}

  # Scenes: BuildSettings from the object dump. mainData is scene 0, levelK scene K+1, and
  # sharedassetsN holds the assets scene N used first.
  build = next((p for p in (raw / 'objects').rglob('BuildSettings*.json')), None)
  scenes = json.loads(build.read_text(encoding='utf-8'))['scenes']
  location = {'resources.assets': 'Common/Resources',
              'Resources/unity default resources': 'Common/Builtin/unity default resources',
              'Resources/unity_builtin_extra': 'Common/Builtin/unity_builtin_extra'}
  scene_rows = []
  for i, path in enumerate(scenes):
    rel = path[len('Assets/Content/Scenes/'):] if path.startswith('Assets/Content/Scenes/') else path
    folder = 'Scenes/' + rel[:-len('.unity')]
    scene_file = 'mainData' if i == 0 else f'level{i - 1}'
    shared = f'sharedassets{i}.assets'
    location[scene_file] = folder + '/scene'
    if shared in files:
      location[shared] = folder + '/shared'
    gi = data / 'GI' / scene_file
    scene_rows.append({'index': i, 'path': path, 'folder': folder, 'scene_file': scene_file,
                       'shared_file': shared if shared in files else None,
                       'gi': f'{folder}/GI' if gi.is_dir() else None})
  for name, f in files.items():
    f['location'] = location.get(name)

  # Everything a scene needs loaded: its files and, transitively, their dependencies.
  def closure(start):
    seen, stack = [], list(start)
    while stack:
      n = stack.pop(0)
      if n in seen or n not in files:
        continue
      seen.append(n)
      stack.extend(files[n]['externals'])
    return seen
  for s in scene_rows:
    own = [s['scene_file']] + ([s['shared_file']] if s['shared_file'] else [])
    s['requires'] = [n for n in closure(own) if n not in own]

  # Missions and maps: scenes in the same folder are loaded together (WorldArt, Audio, Design).
  groups = defaultdict(list)
  for s in scene_rows:
    groups[str(Path(s['folder']).parent).replace('\\', '/')].append(s['index'])

  # MonoScript class names, for grouping the game's component data by script.
  objects_xml = asset_list(raw / 'objects' / 'assets.xml')
  content_xml = asset_list(raw / 'content' / 'assets.xml')
  object_dirs = export_dirs(raw / 'objects')
  scripts = {}
  for key, d in object_dirs.items():
    for p in d.rglob('*.json'):
      m = PATH_ID.search(p.stem + '.x')
      if not m:
        continue
      kind = objects_xml.get((key, int(m.group(1))), ('',))[0]
      if kind == 'MonoScript':
        j = json.loads(p.read_text(encoding='utf-8'))
        ns = j.get('m_Namespace') or ''
        scripts[(key, int(m.group(1)))] = (ns + '.' if ns else '') + (j.get('m_ClassName') or '')

  def script_of(source, j):
    ref = j.get('m_Script') or {}
    fid, pid = ref.get('m_FileID'), ref.get('m_PathID')
    if fid is None or pid is None:
      return None
    target = source if fid == 0 else (files[source]['externals'][fid - 1] if source in files and
                                      fid - 1 < len(files[source]['externals']) else None)
    return scripts.get((target, pid))

  # Content: move each exported file to <location>/<Type>/ (MonoBehaviour/<script>/).
  moved = Counter()
  for key, d in export_dirs(raw / 'content').items():
    dest_root = root / (location.get(key) or f'Unsorted/{safe(key)}')
    for p in list(d.rglob('*')):
      if not p.is_file():
        continue
      m = PATH_ID.search(p.name)
      kind, _ = content_xml.get((key, int(m.group(1))), ('Other', '')) if m else ('Other', '')
      sub = Path(kind)
      if kind == 'MonoBehaviour' and p.suffix == '.json':
        try:
          script = script_of(key, json.loads(p.read_text(encoding='utf-8')))
        except (ValueError, UnicodeDecodeError):
          script = None
        sub = sub / safe(script or '_unknown script')
      target = dest_root / sub / p.relative_to(d)
      target.parent.mkdir(parents=True, exist_ok=True)
      os.replace(p, target)
      moved[kind] += 1

  # Objects: one JSON line per object, per type, per file.
  dumped = Counter()
  for key, d in object_dirs.items():
    dest = root / (location.get(key) or f'Unsorted/{safe(key)}') / 'objects'
    dest.mkdir(parents=True, exist_ok=True)
    handles = {}
    for p in sorted(d.rglob('*.json')):
      m = PATH_ID.search(p.stem + '.x')
      pid = int(m.group(1)) if m else None
      kind, name = objects_xml.get((key, pid), (p.parent.name if p.parent != d else 'Other', p.stem))
      try:
        body = json.loads(p.read_text(encoding='utf-8'))
      except ValueError:
        body = {'_unreadable': p.read_text(encoding='utf-8', errors='replace')[:2000]}
      h = handles.get(kind)
      if h is None:
        h = handles[kind] = open(dest / f'{safe(kind)}.jsonl', 'w', encoding='utf-8', newline='\n')
      h.write(json.dumps({'pathID': pid, 'name': name, 'data': body}, ensure_ascii=False) + '\n')
      dumped[kind] += 1
    for h in handles.values():
      h.close()

  # Baked lighting, copied from the game.
  for s in scene_rows:
    if s['gi']:
      shutil.copytree(data / 'GI' / s['scene_file'], root / s['gi'], dirs_exist_ok=True)

  manifest = {
    'game': 'Homeworld: Deserts of Kharak', 'unity': '5.2.2f1',
    'references': 'An object reference is (m_FileID, m_PathID). m_FileID 0 is the file the '
                  'object is in; m_FileID n is files[<that file>].externals[n - 1]. Objects of a '
                  'file are in <location>/objects/<Type>.jsonl, keyed by pathID.',
    'scenes': scene_rows,
    'groups': dict(groups),
    'files': files,
    'counts': {'content': dict(moved.most_common()), 'objects': dict(dumped.most_common())},
  }
  (root / 'manifest.json').write_text(json.dumps(manifest, indent=1, ensure_ascii=False), encoding='utf-8')
  print(f'{sum(moved.values())} content files and {sum(dumped.values())} objects arranged for '
        f'{len(scene_rows)} scenes')
  for k, v in moved.most_common():
    print(f'  {v:7} {k}')


if __name__ == '__main__':
  main()
