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

// Must match UiInstance in GpuPasses.cpp.
#define UI_FLAG_GLYPH 1u

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

#endif // OUTPOST_UI_HLSLI
