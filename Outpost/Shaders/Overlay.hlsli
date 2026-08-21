#ifndef OUTPOST_OVERLAY_HLSLI
#define OUTPOST_OVERLAY_HLSLI

// What OverlayVS and OverlayPS both have to agree about (ADR-006 §8).
//
// One quad per instanced mark, built from SV_VertexID. `kind` decides what the
// quad means: a ring is a distance test in the pixel shader and a bar is a fill
// fraction, and both are the same four vertices with the same signature between
// the stages.

#include "FrameConstants.hlsli"
#include "PassConstants.hlsli"

// Must match OverlayKind in OverlayMark.h. The shader branches on these rather
// than the pass binding a pipeline per kind.
//
// Plane-lying kinds are numbered below the screen-facing ones so that "which
// space is this quad built in" is one comparison rather than a list. The pass
// draws the two halves separately with different depth state, and this is the
// same boundary seen from inside the shader.
#define OVERLAY_SELECTION_RING 0
#define OVERLAY_ORDER_FOOTPRINT 1
#define OVERLAY_ORDER_STATION 2
#define OVERLAY_FIRST_SCREEN_FACING 3
#define OVERLAY_HULL_BAR 3
#define OVERLAY_SHIELD_BAR 4
#define OVERLAY_STALE_MARKER 5

// A game status bit, marked (ADR-014 4). Same shape as the stale marker -- a
// screen-facing dashed ring -- because it says the same kind of thing: this
// ship is temporarily in a state, and the state is about the ship rather than
// about the ground it is over. What the bit *means* is the game's, and neither
// this shader nor the pass that binds it knows.
#define OVERLAY_STATUS_MARKER 6

// A ship arriving in or leaving the world, for the second it takes (ADR-017 4).
// The same ring undashed and fading, because an arrival is a fact rather than a
// promise and the dash is this vocabulary's word for unresolved.
#define OVERLAY_TRANSIT_RING 7

// Per instance, from OverlayMark (OverlayMark.h). The size channel packs four
// floats whose meaning depends on the kind: a ring is sized in plane metres
// because it lies on the plane, a bar in pixels because it faces the screen.
struct MarkInput
{
  float2 anchorPlane : MARK_ANCHOR;
  float4 size : MARK_SIZE; // x = radius m, y = half width px, z = half height px, w = offset up px.
  float4 colour : MARK_COLOUR;
  uint2 kindAndFill : MARK_KINDFILL; // x = kind, y = fill 0-65535.
};

struct VertexOutput
{
  float4 clipPosition : SV_Position;
  float2 local : LOCAL;           // The quad corner, -1..1 on both axes.
  float4 colour : COLOUR;
  nointerpolation uint kind : KIND;

  /// `OverlayMark::fill`, raw: 0-65535 of a bar's width, or a footprint's dash
  /// count. Not normalised here, because the two readings want different
  /// scales and the stage that knows which is the pixel shader.
  nointerpolation float fill : FILL;
};

/// Quad corners from the vertex id, as a triangle strip: 0 (-1,-1), 1 (+1,-1),
/// 2 (-1,+1), 3 (+1,+1). No vertex buffer and no index buffer, the same trick
/// the nebula's full-screen triangle uses.
float2 OverlayCorner(uint _vertexId)
{
  return float2((_vertexId & 1u) != 0u ? 1.0f : -1.0f, (_vertexId & 2u) != 0u ? 1.0f : -1.0f);
}

#endif // OUTPOST_OVERLAY_HLSLI
