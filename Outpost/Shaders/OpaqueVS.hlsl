// OpaqueVS.hlsl -- instance transform for the one opaque pass (ADR-006 §6).
//
// Instancing is a second vertex stream rather than a structured buffer, so a
// class is one DrawIndexedInstanced per material with no descriptor churn.
// Normals are per-face as authored, so `flat` shading needs no shader work --
// the exporter already duplicated the corners.

#include "Opaque.hlsli"

VertexOutput VertexMain(VertexInput _input)
{
  // The cosmetic bank first, in model space (S14): a roll about the forward
  // axis, which as authored is -Z, so it rotates the model's X-Y plane. The
  // model's +X is its port side, so positive bank -- which rotates +X toward
  // +Y -- lifts port and drops starboard, matching Game::CosmeticBankRadians's
  // stated sign. Applied before the yaw because a ship banks about its own
  // hull, not about north.
  const float sinBank = sin(_input.instanceBank);
  const float cosBank = cos(_input.instanceBank);

  float3 local = _input.position;
  local.x = _input.position.x * cosBank - _input.position.y * sinBank;
  local.y = _input.position.x * sinBank + _input.position.y * cosBank;

  float3 localNormal = _input.normal;
  localNormal.x = _input.normal.x * cosBank - _input.normal.y * sinBank;
  localNormal.y = _input.normal.x * sinBank + _input.normal.y * cosBank;

  // Heading is radians CCW from +x in sim space (ADR-001 §3) and the mesh
  // forward axis is -Z as authored, so the model's -Z has to end up pointing
  // along (cos h, 0, sin h). That is this rotation and no other; writing it out
  // is cheaper than a matrix upload and impossible to get subtly wrong later.
  const float sinHeading = sin(_input.instanceHeading);
  const float cosHeading = cos(_input.instanceHeading);

  float3 world;
  world.x = -local.x * sinHeading - local.z * cosHeading;
  world.y = local.y;
  world.z = local.x * cosHeading - local.z * sinHeading;
  world += _input.instancePosition;

  float3 normal;
  normal.x = -localNormal.x * sinHeading - localNormal.z * cosHeading;
  normal.y = localNormal.y;
  normal.z = localNormal.x * cosHeading - localNormal.z * sinHeading;

  VertexOutput output;
  output.clipPosition = mul(float4(world, 1.0f), g_viewProjection);
  output.worldNormal = normal;
  output.team = _input.instanceChannels.x;
  return output;
}
