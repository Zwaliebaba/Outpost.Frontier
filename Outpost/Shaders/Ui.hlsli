#ifndef OUTPOST_UI_HLSLI
#define OUTPOST_UI_HLSLI

// The Ui pass's shared declarations (ADR-006 §9, §10).
//
// One instance stream carries both halves of the HUD: solid panels and glyphs.
// They differ by a flag rather than by a pipeline, because they are the same
// quad in the same space blended the same way, and two pipelines would mean two
// draws over one upload for no gain -- panel, text, panel, text is the order the
// HUD is built in, and splitting it would need a sort.

#include "PassConstants.hlsli"

// Must match UiDrawList.h.
#define UI_FLAG_GLYPH 1u

// Sweep the quad along `axis` rather than aligning it to the screen: `rect`
// then means centre and (length, thickness). This is what lets the pass draw a
// dashed lane at an angle, which ADR-006 §8a says a quad around a point cannot.
#define UI_FLAG_ORIENTED 2u

Texture2D<float> g_glyphAtlas : register(t0);
SamplerState g_clampSampler : register(s1);

// Per instance. Screen pixels with the origin top-left -- the space the prints
// are drawn in, converted to clip space here and nowhere else.
struct UiInstanceInput
{
  float4 rect : UI_RECT;   // xy = top-left, zw = size, all in pixels.
  // Normalised atlas coordinates, xy = top-left and zw = size. Normalised on
  // the CPU rather than here, because the alternative is the atlas's dimensions
  // in `PassConstants` -- a per-pass constant carrying a number that changes
  // once, at boot, for the benefit of one shader.
  float4 uv : UI_UV;       // Zero for a panel.
  float4 colour : UI_COLOUR;
  uint flags : UI_FLAGS;
  float2 axis : UI_AXIS;   // Unit direction; read only for UI_FLAG_ORIENTED.
};

struct UiVertexOutput
{
  float4 clipPosition : SV_Position;
  float2 uv : TEXCOORD;    // Normalised atlas coordinates; unused by a panel.
  float4 colour : COLOUR;
  nointerpolation uint flags : FLAGS;
};

// Quad corners from the vertex id as a triangle strip, in 0..1 rather than the
// overlay's -1..1: a Ui rect is a top-left and a size, so a corner that runs
// from the origin is one multiply-add instead of a halving first.
float2 UiCorner(uint _vertexId)
{
  return float2((_vertexId & 1u) != 0u ? 1.0f : 0.0f, (_vertexId & 2u) != 0u ? 1.0f : 0.0f);
}

// The HUD's colours are authored in display terms -- `HudPalette`'s values are
// read off the prints -- but the render target is an `_SRGB` view, which
// encodes on write. A colour passed through untouched is therefore encoded
// twice and arrives brighter and greyer than authored: rgb(4,8,5) panels came
// out olive, which is exactly the wash the palette migration existed to
// remove. Decoded here, once per corner, rather than on the CPU, because an
// 8-bit *linear* value cannot hold the dark end -- rgb(4,8,5) linearises to
// under 1/255 and would quantise to black. The exact piecewise transfer, not
// the pow(2.2) approximation, so the round trip through the RTV's encode
// reproduces the authored byte.
float3 SrgbToLinear(float3 _srgb)
{
  const float3 low = _srgb / 12.92f;
  const float3 high = pow((_srgb + 0.055f) / 1.055f, 2.4f);
  return select(_srgb <= 0.04045f, low, high);
}

#endif // OUTPOST_UI_HLSLI
