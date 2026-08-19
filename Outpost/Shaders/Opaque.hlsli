#ifndef OUTPOST_OPAQUE_HLSLI
#define OUTPOST_OPAQUE_HLSLI

// What OpaqueVS and OpaquePS both have to agree about (ADR-006 §6).
//
// `VertexOutput` is the reason this file exists rather than the constants: the
// two stages are linked by that signature, and a field added to one copy and
// not the other is a link-time mismatch at PSO creation with a message that
// names neither file. One declaration cannot disagree with itself.

#include "FrameConstants.hlsli"
#include "PassConstants.hlsli"

cbuffer DrawConstants : register(b2)
{
  uint g_materialIndex;
};

struct VertexInput
{
  float3 position : POSITION;
  float3 normal : NORMAL;

  // Per instance, from InstanceRecord (RenderWorld.h). classId is not here: the
  // draw call already knows the class -- it is what chose the mesh.
  float3 instancePosition : INSTANCE_POSITION;
  float instanceHeading : INSTANCE_HEADING;
  uint2 instanceChannels : INSTANCE_CHANNELS; // x = teamColorId, y = selectionAndLodBias.
  float instanceBank : INSTANCE_BANK;         // Cosmetic roll, radians; positive drops starboard (S14).
};

struct VertexOutput
{
  float4 clipPosition : SV_Position;
  float3 worldNormal : NORMAL;
  nointerpolation uint team : TEAM;
};

#endif // OUTPOST_OPAQUE_HLSLI
