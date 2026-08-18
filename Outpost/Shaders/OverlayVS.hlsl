// OverlayVS.hlsl -- one quad per instanced overlay mark (ADR-006 §8).
//
// Two spaces in one shader, chosen per kind. A ring, an order footprint and a
// station tick are circles *on the plane*, so their quads are built in plane
// metres and pushed through the view-projection -- which is what makes them
// foreshorten into the 2:1 ellipse ADR-006 §4 fixed the elevation to produce,
// with no ellipse maths anywhere, and it is why a ghost's footprint lies on the
// ground like the fleet it describes. A bar is screen-facing, so its quad is
// built by anchoring the ship's position in clip space and then offsetting in
// pixels, which keeps it the same size at every zoom.

#include "Overlay.hlsli"

VertexOutput VertexMain(uint _vertexId : SV_VertexID, MarkInput _mark)
{
  const float2 corner = OverlayCorner(_vertexId);

  VertexOutput output;
  output.local = corner;
  output.colour = _mark.colour;
  output.kind = _mark.kindAndFill.x;
  output.fill = float(_mark.kindAndFill.y);

  if (_mark.kindAndFill.x < OVERLAY_FIRST_SCREEN_FACING)
  {
    // The quad lies on the plane, so render space is (x, 0, y) -- ADR-001 §3's
    // basis, with the cosmetic height left at zero because a ring is on the
    // ground by definition. A selection ring, an order footprint and a station
    // tick differ only in radius and in what the pixel shader draws inside.
    const float2 plane = _mark.anchorPlane + corner * _mark.size.x;
    output.clipPosition = mul(float4(plane.x, 0.0f, plane.y, 1.0f), g_viewProjection);
    return output;
  }

  // A bar. The anchor goes through the projection and the quad is added
  // afterwards in pixels: NDC spans 2 across the viewport, so a pixel is
  // 2 / viewport of it, and multiplying by w keeps this correct if the
  // projection ever stops being orthographic.
  const float4 anchorClip = mul(float4(_mark.anchorPlane.x, 0.0f, _mark.anchorPlane.y, 1.0f), g_viewProjection);
  const float2 offsetPixels = float2(corner.x * _mark.size.y, corner.y * _mark.size.z + _mark.size.w);

  output.clipPosition = anchorClip;
  output.clipPosition.xy += offsetPixels * 2.0f * g_viewportSize.zw * anchorClip.w;
  return output;
}
