// OpaqueVS.hlsl -- instance transform for the one opaque pass (ADR-006 §6).
//
// Instancing is a second vertex stream rather than a structured buffer, so a
// class is one DrawIndexedInstanced per material with no descriptor churn.
// Normals are per-face as authored, so `flat` shading needs no shader work --
// the exporter already duplicated the corners.

#include "Opaque.hlsli"

VertexOutput VertexMain(VertexInput _input)
{
  // Heading is radians CCW from +x in sim space (ADR-001 §3) and the mesh
  // forward axis is -Z as authored, so the model's -Z has to end up pointing
  // along (cos h, 0, sin h). That is this rotation and no other; writing it out
  // is cheaper than a matrix upload and impossible to get subtly wrong later.
  const float sinHeading = sin(_input.instanceHeading);
  const float cosHeading = cos(_input.instanceHeading);

  float3 world;
  world.x = -_input.position.x * sinHeading - _input.position.z * cosHeading;
  world.y = _input.position.y;
  world.z = _input.position.x * cosHeading - _input.position.z * sinHeading;
  world += _input.instancePosition;

  float3 normal;
  normal.x = -_input.normal.x * sinHeading - _input.normal.z * cosHeading;
  normal.y = _input.normal.y;
  normal.z = _input.normal.x * cosHeading - _input.normal.z * sinHeading;

  VertexOutput output;
  output.clipPosition = mul(float4(world, 1.0f), g_viewProjection);
  output.worldNormal = normal;
  output.team = _input.instanceChannels.x;
  return output;
}
