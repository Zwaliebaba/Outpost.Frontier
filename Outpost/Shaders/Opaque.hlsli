#ifndef OUTPOST_OPAQUE_HLSLI
#define OUTPOST_OPAQUE_HLSLI

// What OpaqueVS and OpaquePS both have to agree about (ADR-006 §6).
//
// `VertexOutput` is the reason this file exists rather than the constants: the
// two stages are linked by that signature, and a field added to one copy and
// not the other is a link-time mismatch at PSO creation with a message that
// names neither file. One declaration cannot disagree with itself.

#include "PassConstants.hlsli"

#define MESH_MATERIAL_COUNT 5
#define TEAM_COLOUR_COUNT 4

// row_major, matching how DirectXMath stores an XMFLOAT4X4, so the upload is a
// memcpy. The alternative -- HLSL's column-major default plus a transpose on
// the CPU -- is the same result with one more place to get the convention
// wrong.
//
// Must match FrameConstants in GpuPipelines.h, including both array sizes.
cbuffer FrameConstants : register(b0)
{
  row_major float4x4 g_viewProjection;
  float4 g_sunDirection;   // xyz: the direction the light travels, normalised.
  float4 g_sunColour;      // rgb, w = intensity.
  float4 g_ambientSky;     // rgb, the colour arriving from above.
  float4 g_ambientGround;  // rgb, the colour arriving from below.
  float4 g_materialAlbedo[MESH_MATERIAL_COUNT]; // rgb = albedo, w = emissive strength.
  float4 g_teamEmissive[TEAM_COLOUR_COUNT];     // rgb = the tint on emissive materials only.
};

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
};

struct VertexOutput
{
  float4 clipPosition : SV_Position;
  float3 worldNormal : NORMAL;
  nointerpolation uint team : TEAM;
};

#endif // OUTPOST_OPAQUE_HLSLI
