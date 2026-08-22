// LampPS.hlsl -- the glow itself (ADR-006 §6a).
//
// A radial falloff computed from the quad's own corner coordinate: white core,
// then the lamp's colour, then nothing. **Procedural rather than a texture**,
// which is a deviation from the sprite atlas the proposal sketched and a
// deliberate one -- a falloff is two `pow`s, and a texture for it would be a
// bake to maintain, a descriptor to bind, a sampler to choose and a resolution
// to be wrong at some zoom. The atlas earns its place when lamps stop being
// radially symmetric; today they are.
//
// The white core is what makes a coloured light read as a *light*. A disc of
// flat red at 80% opacity reads as a painted dot; the same disc with a blown-out
// centre reads as something emitting, which is the entire trick and costs one
// lerp.

#include "Lamp.hlsli"

float4 PixelMain(VertexOutput _input) : SV_Target
{
  // 1 at the centre, 0 at the inscribed circle, negative in the corners --
  // saturate turns the quad into a disc with no alpha test and no discard.
  const float glow = saturate(1.0f - length(_input.local));

  // Two exponents over one falloff: the halo is wide and the core is tight, and
  // the gap between the two is what separates the bulb from its bloom.
  const float halo = glow * glow * sqrt(glow); // pow(glow, 2.5), spelled cheaply.
  const float core = glow * glow * glow * glow;
  const float3 rgb = lerp(_input.colour, float3(1.0f, 1.0f, 1.0f), core * core);

  // Premultiplied, for the ONE/ONE blend the pass sets: a glow adds light to
  // whatever is behind it and never darkens it, which is also why it needs no
  // sorting against the other lamps in the frame.
  return float4(rgb * halo * _input.opacity, 0.0f);
}
