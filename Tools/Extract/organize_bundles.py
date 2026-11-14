#!/usr/bin/env python3
"""Arrange the per-bundle exports (Extract.ps1, step bundles) and add the bundles to manifest.json.

  organize_bundles.py BUNDLES_DIR ASSETS

BUNDLES_DIR is the installation's Bundles folder; ASSETS the DoK-Assets folder, whose
_raw_bundles/<bundle>/{content,objects} hold AssetStudioModCLI's export and JSON dump of each
bundle. Per bundle it writes ASSETS/Bundles/<bundle>/:
  <Type>/...              the converted assets (MonoBehaviour/<script class>/ when the script is known)
  objects/<Type>.jsonl    every object, one per line: {file, pathID, name, data}
  containers.json         the bundle's container paths (the assets' paths in the original Unity
                          project) -> type, file, pathID
and manifest.json gains "bundles": each bundle's dependencies (Bundles.manifest), size, the
serialized files inside it and counts.

The game also loads one encrypted bundle, encrypted/font: an AES-encrypted TextAsset in
resources.assets (LegalFonts, AssetBundleManager.LoadEncryptedBundleImmediate), holding licensed
fonts. It is not decrypted here; the manifest lists it as not extracted.
"""

import json
import os
import re
import sys
import xml.etree.ElementTree as ET
from collections import Counter, defaultdict
from pathlib import Path

PATH_ID = re.compile(r' @(-?\d+)(\.[^.]+)?$')
ENCRYPTED = {'encrypted/font': 'AES-encrypted TextAsset in resources.assets (licensed fonts); not decrypted'}


def safe(name):
  return re.sub(r'[<>:"/\\|?*\x00-\x1f]', '_', name).strip() or '_'


def read_manifest(bundles_dir):
  """{bundle name: [dependencies]} from Bundles/Bundles.manifest (Unity's YAML-like text)."""
  deps, current = {}, None
  for line in (bundles_dir / 'Bundles.manifest').read_text(encoding='utf-8').splitlines():
    s = line.strip()
    if s.startswith('Name:'):
      current = s[5:].strip()
      deps[current] = []
    elif s.startswith('Dependency_') and current:
      deps[current].append(s.split(':', 1)[1].strip())
  return deps


def asset_list(xml_path):
  out = []
  if not xml_path.exists():
    return out
  for _, el in ET.iterparse(xml_path):
    if el.tag == 'Asset':
      out.append({'name': el.findtext('Name') or '', 'container': el.findtext('Container') or '',
                  'type': el.findtext('Type') or 'Other', 'pathID': int(el.findtext('PathID')),
                  'file': Path((el.findtext('Source') or '').replace('\\', '/')).name})
      el.clear()
  return out


def main():
  bundles_dir, root = Path(sys.argv[1]), Path(sys.argv[2])
  raw = root / '_raw_bundles'
  deps = read_manifest(bundles_dir)
  rows = {}
  for name in sorted(deps):
    src = raw / name
    entry = {'dependencies': deps[name], 'location': f'Bundles/{name}'}
    if name in ENCRYPTED:
      entry.update(extracted=False, note=ENCRYPTED[name])
      rows[name] = entry
      continue
    bundle_file = bundles_dir / name
    entry['size'] = bundle_file.stat().st_size if bundle_file.exists() else None
    dest = root / 'Bundles' / name
    content = asset_list(src / 'content' / 'assets.xml')
    objects = asset_list(src / 'objects' / 'assets.xml')
    kinds = {(a['file'], a['pathID']): a['type'] for a in content + objects}

    # MonoScript class names inside the bundle, for grouping its component data.
    scripts = {}
    for p in (src / 'objects').rglob('*.json'):
      m = PATH_ID.search(p.stem + '.x')
      if m and kinds.get((p.parent.name, int(m.group(1)))) == 'MonoScript':
        j = json.loads(p.read_text(encoding='utf-8'))
        ns = j.get('m_Namespace') or ''
        scripts[(p.parent.name, int(m.group(1)))] = (ns + '.' if ns else '') + (j.get('m_ClassName') or '')

    moved = Counter()
    for p in sorted((src / 'content').rglob('*')):
      if not p.is_file() or p.name == 'assets.xml':
        continue
      m = PATH_ID.search(p.name)
      kind = kinds.get((p.parent.name, int(m.group(1))), 'Other') if m else 'Other'
      sub = Path(kind)
      if kind == 'MonoBehaviour' and p.suffix == '.json':
        try:
          ref = json.loads(p.read_text(encoding='utf-8')).get('m_Script') or {}
          script = scripts.get((p.parent.name, ref.get('m_PathID'))) if ref.get('m_FileID') == 0 else None
        except ValueError:
          script = None
        sub = sub / safe(script or '_unknown script')
      target = dest / sub / p.name
      target.parent.mkdir(parents=True, exist_ok=True)
      os.replace(p, target)
      moved[kind] += 1

    dumped, handles = Counter(), {}
    (dest / 'objects').mkdir(parents=True, exist_ok=True)
    for p in sorted((src / 'objects').rglob('*.json')):
      m = PATH_ID.search(p.stem + '.x')
      pid = int(m.group(1)) if m else None
      kind = kinds.get((p.parent.name, pid), 'Other')
      try:
        body = json.loads(p.read_text(encoding='utf-8'))
      except ValueError:
        body = {'_unreadable': True}
      h = handles.get(kind)
      if h is None:
        h = handles[kind] = open(dest / 'objects' / f'{safe(kind)}.jsonl', 'w', encoding='utf-8', newline='\n')
      h.write(json.dumps({'file': p.parent.name, 'pathID': pid, 'name': PATH_ID.sub('', p.stem + '.x'),
                          'data': body}, ensure_ascii=False) + '\n')
      dumped[kind] += 1
    for h in handles.values():
      h.close()

    containers = {a['container']: {'type': a['type'], 'file': a['file'], 'pathID': a['pathID']}
                  for a in content + objects if a['container']}
    (dest / 'containers.json').write_text(json.dumps(containers, indent=1, sort_keys=True), encoding='utf-8')
    left = [p for p in (src / 'content').rglob('*') if p.is_file() and p.name != 'assets.xml']
    entry.update(extracted=True, files=sorted({a['file'] for a in objects}), containers=len(containers),
                 content=dict(moved.most_common()), objects=dict(dumped.most_common()), unarranged=len(left))
    rows[name] = entry
    print(f'{name:28} {sum(moved.values()):5} assets, {sum(dumped.values()):6} objects, {len(containers):5} containers'
          + (f', {len(left)} NOT ARRANGED' if left else ''))

  manifest_path = root / 'manifest.json'
  manifest = json.loads(manifest_path.read_text(encoding='utf-8'))
  manifest['bundles'] = rows
  manifest_path.write_text(json.dumps(manifest, indent=1, ensure_ascii=False), encoding='utf-8')
  done = [r for r in rows.values() if r.get('extracted')]
  print(f'{len(done)} bundles arranged, {sum(sum(r["content"].values()) for r in done)} assets, '
        f'{sum(sum(r["objects"].values()) for r in done)} objects; not extracted: '
        f'{", ".join(n for n, r in rows.items() if not r.get("extracted")) or "none"}')
  return 1 if any(r.get('unarranged') for r in done) else 0


if __name__ == '__main__':
  sys.exit(main())
