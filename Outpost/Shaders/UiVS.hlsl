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
  const float2 pixel = _instance.rect.xy + corner * _instance.rect.zw;

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
