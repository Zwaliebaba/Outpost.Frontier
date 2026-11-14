"""The dependency (externals) table and object counts of a Unity 5.x serialized file.

In a serialized file, a reference is (m_FileID, m_PathID): m_FileID 0 is the file itself and
m_FileID n is the n-th entry of the file's externals table. This reads that table, in order,
so the JSON dumps' references can be resolved to files.

Supports format versions 9 to 17 without type trees (player builds); Deserts of Kharak is
Unity 5.2.2f1, format 15.
"""

import struct
from collections import Counter

# The Unity class IDs that matter most for reading a dump.
CLASS_NAMES = {
  1: 'GameObject', 4: 'Transform', 21: 'Material', 23: 'MeshRenderer', 28: 'Texture2D',
  33: 'MeshFilter', 43: 'Mesh', 48: 'Shader', 49: 'TextAsset', 64: 'MeshCollider',
  65: 'BoxCollider', 74: 'AnimationClip', 82: 'AudioSource', 83: 'AudioClip', 91: 'AnimatorController',
  95: 'Animator', 108: 'Light', 111: 'Animation', 114: 'MonoBehaviour', 115: 'MonoScript',
  128: 'Font', 137: 'SkinnedMeshRenderer', 152: 'MovieTexture', 198: 'ParticleSystem',
  199: 'ParticleSystemRenderer', 218: 'Terrain', 156: 'TerrainData', 154: 'TerrainCollider',
  1001: 'Prefab', 1032: 'SceneSettings',
}


class Reader:
  def __init__(self, data, pos, big_endian):
    self.d, self.p, self.e = data, pos, '>' if big_endian else '<'

  def u8(self):
    self.p += 1
    return self.d[self.p - 1]

  def i16(self):
    self.p += 2
    return struct.unpack_from(self.e + 'h', self.d, self.p - 2)[0]

  def u16(self):
    self.p += 2
    return struct.unpack_from(self.e + 'H', self.d, self.p - 2)[0]

  def i32(self):
    self.p += 4
    return struct.unpack_from(self.e + 'i', self.d, self.p - 4)[0]

  def u32(self):
    self.p += 4
    return struct.unpack_from(self.e + 'I', self.d, self.p - 4)[0]

  def i64(self):
    self.p += 8
    return struct.unpack_from(self.e + 'q', self.d, self.p - 8)[0]

  def cstr(self):
    end = self.d.index(b'\0', self.p)
    s = self.d[self.p:end].decode('utf-8', 'replace')
    self.p = end + 1
    return s

  def align(self):
    self.p = (self.p + 3) & ~3

  def skip(self, n):
    self.p += n


def read(path):
  """{'version', 'unity', 'platform', 'objects': Counter(class name), 'externals': [path, ...]}."""
  with open(path, 'rb') as f:
    head = f.read(20)
    metadata_size, file_size, version, data_offset = struct.unpack('>IIII', head[:16])
    big_endian = head[16] != 0
    data = head + f.read(metadata_size + 64)
  if version < 9 or version > 17:
    raise ValueError(f'{path}: serialized file format {version} is not supported')
  r = Reader(data, 20, big_endian)
  unity = r.cstr()
  platform = r.i32()
  type_tree = bool(r.u8()) if version >= 13 else True
  if type_tree:
    raise ValueError(f'{path}: type trees are present; this reader handles player builds only')
  type_count = r.i32()
  for _ in range(type_count):
    class_id = r.i32()
    if version >= 16:
      r.skip(1)                    # stripped
    if version >= 17:
      r.skip(2)                    # script type index
    if version >= 13:
      if (version < 16 and class_id < 0) or (version >= 16 and class_id == 114):
        r.skip(16)                 # script id
      r.skip(16)                   # old type hash
  objects = Counter()
  object_count = r.i32()
  for _ in range(object_count):
    if version >= 14:
      r.align()
      r.i64()                      # path id
    else:
      r.i32()
    r.u32()                        # byte start
    r.u32()                        # byte size
    type_id = r.i32()
    class_id = type_id
    if version < 16:
      class_id = r.u16()
    if version < 11:
      r.u16()                      # is destroyed
    if 11 <= version < 17:
      r.i16()                      # script type index
    if version in (15, 16):
      r.u8()                       # stripped
    objects[CLASS_NAMES.get(class_id, f'class {class_id}')] += 1
  if version >= 11:
    script_count = r.i32()
    for _ in range(script_count):
      r.i32()
      if version >= 14:
        r.align()
        r.i64()
      else:
        r.i32()
  externals = []
  external_count = r.i32()
  for _ in range(external_count):
    if version >= 6:
      r.cstr()                     # empty
    if version >= 5:
      r.skip(16)                   # guid
    r.i32()                        # type
    externals.append(r.cstr())
  return {'version': version, 'unity': unity, 'platform': platform, 'objects': objects,
          'object_count': object_count, 'externals': externals}
