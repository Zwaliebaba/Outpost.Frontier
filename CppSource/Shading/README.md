# Shading

Shading provides material instances, texture groups, material animation, and
draw batching for the client renderer.

- [MaterialLibrary.cpp](MaterialLibrary.cpp) builds materials and resolves names
  and aliases. Group ID zero is reserved. Renaming a group preserves its ID.
- [TextureLibrary.cpp](TextureLibrary.cpp) manages texture groups and frame
  playback. Setting a wrap mode updates the mode. A nonempty group reports the
  first frame's pixel format.
- [MaterialAnimation.cpp](MaterialAnimation.cpp) manages animation instances and
  the keys associated with each load handle. Unloading a handle releases its
  own records.
- [MaterialBatcher.cpp](MaterialBatcher.cpp) applies matching rules, sorts the
  batch descriptors, and dispatches draw submissions.
- [Material.cpp](Material.cpp) implements individual material behaviors and
  delegates texture and renderer work to the engine services.

The Rust destination for these systems is NeuronClient. Keep draw ordering,
resource ownership, and frame selection explicit when porting the behaviors.
