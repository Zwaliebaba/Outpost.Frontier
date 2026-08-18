// UiVS.hlsl -- one quad per HUD instance, in screen pixels (ADR-006 §10).
//
// The whole pass is a coordinate change: pixels with the origin top-left into
// clip space with the origin at the centre and y up. Doing it here rather than
// in the layout is what lets every constant in `UiTuning` be read straight off
// a print.

#include "Ui.hlsli"

UiVertexOutput VertexMain(uint _vertexId : SV_VertexID, UiInstanceInput _instance)
{
  const float2 corner = UiCorner(_vertexId);

  // An oriented quad is swept from its centre: half a length along the axis and
  // half a thickness across it. The axis-aligned branch is left exactly as it
  // was rather than expressed as a special case of the sweep -- every panel and
  // every glyph in the HUD goes through it, and "the general form reduces to
  // the old one" is a claim no test here can check.
  float2 pixel;
  if ((_instance.flags & UI_FLAG_ORIENTED) != 0u)
  {
    const float2 along = _instance.axis;
    const float2 across = float2(-along.y, along.x);
    pixel = _instance.rect.xy + along * ((corner.x - 0.5f) * _instance.rect.z) +
            across * ((corner.y - 0.5f) * _instance.rect.w);
  }
  else
  {
    pixel = _instance.rect.xy + corner * _instance.rect.zw;
  }

  UiVertexOutput output;
  // x: 0..width becomes -1..1. y: 0..height becomes 1..-1, because pixels count
  // down from the top and clip space counts up from the centre.
  output.clipPosition = float4(pixel.x * g_viewportSize.z * 2.0f - 1.0f, 1.0f - pixel.y * g_viewportSize.w * 2.0f, 0.0f, 1.0f);
  output.colour = _instance.colour;
  output.flags = _instance.flags;

  // Already normalised (Ui.hlsli). A panel's uv is zero and never read.
  output.uv = _instance.uv.xy + corner * _instance.uv.zw;
  return output;
}
