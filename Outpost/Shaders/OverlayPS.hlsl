// OverlayPS.hlsl -- the annulus, the tick and the fill (ADR-006 §8).
//
// The quad is a canvas, not the shape. A ring is the set of pixels a fixed
// screen distance from the unit circle, which is why it stays the same
// thickness whether it is fourteen pixels across or four hundred; a bar is a
// fill test along the quad's x; a station tick is a disc.
//
// A footprint is a ring that may be dashed, and the dashing is what separates
// a promise from a fact (`puck-and-wheel.png` §4): PENDING and REJECTED ghosts
// are dashed, an accepted one is solid, and the CPU says which by sending a
// dash count or a zero.

#include "Overlay.hlsli"

/// The annulus test, shared by every ring-shaped kind. `_distance` is the
/// distance from the quad's centre with 1.0 exactly on the circle; fwidth gives
/// how much that changes per pixel, so a constant multiple of it is a constant
/// *screen* thickness -- and the floor keeps the ring from disappearing when
/// the derivative is tiny, which is what happens on a ring that fills the
/// screen.
float RingAlpha(float _distance)
{
  const float halfThickness = max(fwidth(_distance) * 1.2f, 0.03f);
  return 1.0f - smoothstep(halfThickness * 0.4f, halfThickness, abs(_distance - 1.0f));
}

float4 PixelMain(VertexOutput _input) : SV_Target
{
  if (_input.kind == OVERLAY_SELECTION_RING)
  {
    const float alpha = RingAlpha(length(_input.local));
    clip(alpha - 0.004f); // Nothing to blend outside the ring, so do not blend it.
    return float4(_input.colour.rgb, _input.colour.a * alpha);
  }

  if (_input.kind == OVERLAY_ORDER_FOOTPRINT)
  {
    const float distance = length(_input.local);
    float alpha = RingAlpha(distance);

    // Dashed while the order is a promise. The dash runs on the *angle*, so the
    // gaps are evenly spaced around the circle on the plane and therefore
    // unevenly spaced on screen -- which is correct: the ring is foreshortened,
    // and dashes that stayed even on screen would be uneven on the ground the
    // ring is lying on.
    if (_input.fill > 0.5f)
    {
      const float turns = atan2(_input.local.y, _input.local.x) * (0.5f / 3.14159265f) + 0.5f;
      alpha *= step(frac(turns * _input.fill), 0.55f);
    }

    clip(alpha - 0.004f);
    return float4(_input.colour.rgb, _input.colour.a * alpha);
  }

  if (_input.kind == OVERLAY_ORDER_STATION)
  {
    // A filled disc rather than a ring: a station tick is a handful of pixels
    // across and an annulus that small is an aliased speck. One per ship, so
    // the shape has to survive being tiny.
    const float distance = length(_input.local);
    const float edge = max(fwidth(distance), 0.02f);
    const float alpha = 1.0f - smoothstep(1.0f - edge, 1.0f, distance);
    clip(alpha - 0.004f);
    return float4(_input.colour.rgb, _input.colour.a * alpha);
  }

  // A bar: filled from the left edge to `fill`, with the remainder drawn dark
  // rather than transparent. An empty bar has to be visible as an empty bar --
  // a shield at zero that draws nothing is indistinguishable from a ship that
  // has no shields at all.
  const float along = _input.local.x * 0.5f + 0.5f;
  if (along <= _input.fill * (1.0f / 65535.0f))
  {
    return _input.colour;
  }
  return float4(_input.colour.rgb * 0.12f, _input.colour.a * 0.5f);
}
