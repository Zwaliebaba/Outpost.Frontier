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

// Must match OverlayKind in OverlayMark.h. Three values, and the shader
// branches on them rather than the pass binding three pipelines.
#define OVERLAY_SELECTION_RING 0
#define OVERLAY_HULL_BAR 1
#define OVERLAY_SHIELD_BAR 2

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
  nointerpolation float fill : FILL; // 0..1.
};

/// Quad corners from the vertex id, as a triangle strip: 0 (-1,-1), 1 (+1,-1),
/// 2 (-1,+1), 3 (+1,+1). No vertex buffer and no index buffer, the same trick
/// the nebula's full-screen triangle uses.
float2 OverlayCorner(uint _vertexId)
{
  return float2((_vertexId & 1u) != 0u ? 1.0f : -1.0f, (_vertexId & 2u) != 0u ? 1.0f : -1.0f);
}

#endif // OUTPOST_OVERLAY_HLSLI
