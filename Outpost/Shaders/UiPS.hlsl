// UiPS.hlsl -- a panel is its colour; a glyph is its colour through the atlas.
//
// The atlas is R8 coverage rather than colour (ADR-006 §9), so a glyph is the
// run's colour with the coverage as alpha. That is what makes one atlas serve
// every colour of text on the HUD without a second bake.

#include "Ui.hlsli"

float4 PixelMain(UiVertexOutput _input) : SV_Target
{
  if ((_input.flags & UI_FLAG_GLYPH) == 0u)
  {
    return _input.colour;
  }

  // Load rather than Sample would be one texel and no filtering; the atlas is
  // baked at the sizes it is drawn at, so sampling costs nothing and survives a
  // fractional pen position, which a scaled layout produces constantly.
  const float coverage = g_glyphAtlas.Sample(g_clampSampler, _input.uv).r;
  clip(coverage - 0.002f); // Most of a glyph's quad is empty; do not blend it.
  return float4(_input.colour.rgb, _input.colour.a * coverage);
}
