// NebulaPS.hlsl -- the ambient field the fleet sits inside (ADR-006 §1).
//
// The corpus target frame runs `... Opaque -> Effects -> Nebula -> Tonemap
// -> Overlay ...`, and the position is the design: this composites *over* the
// geometry rather than behind it, so hulls read as being inside the cloud
// instead of pasted onto a wallpaper. It is not a skybox and it draws no
// celestial body -- celestials are data, not geometry (ADR-009 §9a).
//
// The field itself is baked on the CPU (NebulaField.h) and arrives as a single
// R8 tile. Nothing is evaluated here: this shader unprojects a pixel onto the
// plane, samples, and adds. That split is what makes the field testable without
// a GPU, and it means there is exactly one implementation of the noise.

#include "Nebula.hlsli"
#include "PassConstants.hlsli"

Texture2D<float> g_nebulaField : register(t1);
SamplerState g_wrapSampler : register(s1);

float4 PixelMain(VertexOutput _input) : SV_Target
{
  // The projection is orthographic, so NDC lands on the plane through an affine
  // map and no ray/plane intersection is needed (IsoCamera::PlaneMappingForNdc,
  // which NeuronClientTests round-trips against the real view-projection).
  //
  // This is what anchors the haze to the world: pan and the field moves past
  // you, orbit and it turns. Sampling in screen space instead would look like a
  // smudge on the monitor, which is the one thing a background must never do.
  const float2 plane = g_planeOrigin.xy + g_planeRightPerNdc.xy * _input.ndc.x + g_planeUpPerNdc.xy * _input.ndc.y;

  const float2 uv = plane * g_nebulaTile.x;
  const float field = g_nebulaField.Sample(g_wrapSampler, uv);

  // Additive, and deliberately dim: the ceiling on how much this may wash out a
  // hull is the whole readability budget (ADR-006 §7 keeps hull colour meaning
  // one thing). Alpha is left alone -- the blend state adds colour only.
  const float3 colour = g_nebulaTint.rgb * g_nebulaTint.w * field;
  return float4(colour, 0.0f);
}
