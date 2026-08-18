// OverlayPS.hlsl -- the annulus and the fill (ADR-006 §8).
//
// The quad is a canvas, not the shape. A ring is the set of pixels a fixed
// screen distance from the unit circle, which is why it stays the same
// thickness whether it is fourteen pixels across or four hundred; a bar is a
// fill test along the quad's x.

#include "Overlay.hlsli"

float4 PixelMain(VertexOutput _input) : SV_Target
{
  if (_input.kind == OVERLAY_SELECTION_RING)
  {
    // Distance from the centre, 1.0 exactly on the circle. fwidth gives how
    // much that changes per pixel, so a constant multiple of it is a constant
    // *screen* thickness -- and the floor keeps the ring from disappearing when
    // the derivative is tiny, which is what happens on a ring that fills the
    // screen.
    const float distance = length(_input.local);
    const float halfThickness = max(fwidth(distance) * 1.2f, 0.03f);

    const float alpha = 1.0f - smoothstep(halfThickness * 0.4f, halfThickness, abs(distance - 1.0f));
    clip(alpha - 0.004f); // Nothing to blend outside the ring, so do not blend it.
    return float4(_input.colour.rgb, _input.colour.a * alpha);
  }

  // A bar: filled from the left edge to `fill`, with the remainder drawn dark
  // rather than transparent. An empty bar has to be visible as an empty bar --
  // a shield at zero that draws nothing is indistinguishable from a ship that
  // has no shields at all.
  const float along = _input.local.x * 0.5f + 0.5f;
  if (along <= _input.fill)
  {
    return _input.colour;
  }
  return float4(_input.colour.rgb * 0.12f, _input.colour.a * 0.5f);
}
