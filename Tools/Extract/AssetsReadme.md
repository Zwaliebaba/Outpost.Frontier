# Deserts of Kharak: extracted assets

Every asset and every object of Homeworld: Deserts of Kharak (Unity 5.2.2f1, build
"DisjointedFalafel" of 2018-05-25), extracted from the installation's `DesertsOfKharak_Data` and arranged the way
the game loads them: per scene. Private study material; the game's code is decompiled separately
in `..\DoK-Decompiled`.

## Layout

```
manifest.json                 scenes, mission groups, files, dependency tables, counts
Scenes\<path of the scene>\   one folder per scene of the build (81), e.g. Campaign\M04\M04-WorldArt
  scene\                      what the scene file itself holds (mainData for scene 0, levelK for scene K+1)
  shared\                     the assets this scene was the first to use (sharedassetsN.assets)
    <Type>\                   Texture2D (png), Mesh (obj), AudioClip (wav), MovieTexture (ogv, with
                              its audio), Shader, TextAsset, MonoBehaviour\<script class>\ (json)
    objects\<Type>.jsonl      every object of the file, one per line: {pathID, name, data}
  GI\                         baked lighting (WorldArt scenes only), copied as is
Bundles\<bundle>\              the asset bundles (fe\, fe\loading\, vo\, dlc\): <Type>\, objects\<Type>.jsonl,
                              containers.json (each asset's path in the original Unity project)
Models\<name>\                the animated objects as FBX (skeleton, skinning, animation clips) with
                              their textures; Models\index.json gives each one's source file,
                              PathID, kind (prefab / scene object) and animation controller
Common\Resources\             resources.assets: what the code loads by name (Resources.Load)
Common\Builtin\               Unity's built-in resources (default shaders, meshes, fonts)
_logs\                        export logs and the tools' asset lists
```

File names end in ` @<pathID>`, the object's id inside its file.

## How the game puts a level together

- **A mission is several scenes loaded together.** `manifest.json` → `groups` lists them:
  `Campaign/M04` is `M04-WorldArt` (terrain, props, lighting), `M04-Audio` and `M04-Design-NIS`
  (gameplay setup, triggers, cinematics). Multiplayer maps are similar (`MP_07-WorldArt` with
  `MP_07-Design` or `MP_07-Outpost-FFA-Design`).
- **An asset is stored once,** in the `shared\` folder of the first scene that uses it; later
  scenes refer to it there. So a scene folder holds what is new in that scene, and
  `scenes[i].requires` lists every other file it needs loaded. Scene 0 (`master`, the front end
  and global setup) introduces the most (`sharedassets0.assets` is 700 MB).
- **References** are `{m_FileID, m_PathID}`. `m_FileID` 0 means the file the object is in;
  `m_FileID` n means entry n-1 of that file's `files[<file>].externals` in the manifest, whose
  `location` says which folder holds it. `m_PathID` is the `pathID` key in that folder's
  `objects\*.jsonl` (and the ` @<pathID>` in exported file names).
- **The scene hierarchy** is in `objects\GameObject.jsonl` (each lists its components) and
  `objects\Transform.jsonl` (position, rotation, scale, parent and children). Component data of the
  game's own scripts is in `objects\MonoBehaviour.jsonl` and, readable, in `MonoBehaviour\<script>\`;
  the script classes are in `..\DoK-Decompiled`.

## Counts

97,764 converted assets (91,123 MonoBehaviour, 2,578 Texture2D, 2,124 Mesh, 1,632 AudioClip,
217 Shader, 63 TextAsset, 27 MovieTexture) and all 446,610 objects. Not converted, and not lost:
the audio tracks of the 27 movies (inside the .ogv files) and 4 dynamic font textures (made at
runtime). 33 MonoBehaviours could not be matched to their script class
(`MonoBehaviour\_unknown script\`).

## Bundles

The 48 asset bundles the game loads from `Bundles\` (`manifest.json` -> `bundles`: dependencies,
size, the files inside each, counts): the front end and credits (`fe\`), the loading screens
(`fe\loading\`), the voice-over per faction and mission (`vo\`) and the two DLCs (`dlc\`): 73,001
assets, 187,039 objects. Unlike the scenes they keep their **container paths**, the assets' paths in
Blackbird's Unity project (`assets/content/audio/bundled/audio/vo/english/missions/m01/...`); voice
lines end in the `NumberId` of the string tables (`..._16522.ogg`). Not extracted: `encrypted/font`,
an AES-encrypted TextAsset in `resources.assets` holding licensed fonts, which the game decrypts at
runtime; it is left as it is.

## Models

191 FBX models: the 150 animated objects chosen in `_logs\fbx-selection.json` (41 unit prefabs and
the other prefabs with an Animator; the scene-only objects with an animation controller: the
multiplayer and cinematic unit versions, landers, cranes, turbines, salvage wrecks) and 41 more that
the PathID filter caught in other files. The other ~5,400 Animators are scenery without
animation; their meshes are in the OBJ export. The textures beside each FBX are **hard links** to
the identical PNGs under `Scenes\`/`Common\` (1,350 links, 1.3 GB not duplicated): delete freely,
but editing one edits all of them.

## Remaking it

This folder is generated: `Tools\Extract\Extract.ps1` in the Outpost.Frontier repository rebuilds it,
and `..\DoK-Decompiled` with it, from a local installation of the game (about 30 minutes). See
`Tools\Extract\README.md` there.
