// LampVS.hlsl -- one screen-facing glow billboard per lamp per ship
// (ADR-006 §6a).
//
// The lamp is hull-local, so it goes through the same bank-then-yaw-then-
// translate the opaque vertex shader applies to a vertex: a navigation light
// banks into a turn with the wing it is bolted to, because it *is* bolted to
// it. Then the quad is added in world space along the screen's own axes, which
// keeps the billboard facing the viewer while leaving it a real quad at a real
// depth -- so the hull it sits on occludes it honestly, and a lamp on the far
// side of a Battleship is hidden by the Battleship.
//
// The animation is six ALU ops from `g_frameTime`. Nothing about a lamp is
// computed on the CPU after boot.

#include "Lamp.hlsli"

VertexOutput VertexMain(uint _vertexId : SV_VertexID, LampInstanceInput _instance)
{
  const LampDefinition lamp = g_lamps[g_lampIndex];

  const float level = LampAnimatedLevel(lamp, _instance.instanceLampPhase, g_frameTime.x);
  const float opacity = lerp(LAMP_OPACITY_MIN, LAMP_OPACITY_MAX, level);
  const float swell = 1.0f - LAMP_SCALE_SWELL + 2.0f * LAMP_SCALE_SWELL * level;

  // Bank first, in model space, about the forward axis -- which as authored is
  // -Z, so it rotates the model's X-Y plane. Identical to OpaqueVS, and
  // deliberately spelled the same way: a lamp that banked differently from the
  // hull would slide across it through a turn.
  const float sinBank = sin(_instance.instanceBank);
  const float cosBank = cos(_instance.instanceBank);

  float3 local = lamp.positionAndSize.xyz;
  local.x = lamp.positionAndSize.x * cosBank - lamp.positionAndSize.y * sinBank;
  local.y = lamp.positionAndSize.x * sinBank + lamp.positionAndSize.y * cosBank;

  // Then the heading, again exactly as OpaqueVS writes it: heading is radians
  // CCW from +x in sim space and the mesh forward axis is -Z, so the model's -Z
  // ends up along (cos h, 0, sin h).
  const float sinHeading = sin(_instance.instanceHeading);
  const float cosHeading = cos(_instance.instanceHeading);

  float3 world;
  world.x = -local.x * sinHeading - local.z * cosHeading;
  world.y = local.y;
  world.z = local.x * cosHeading - local.z * sinHeading;
  world += _instance.instancePosition;

  /*
   * The screen's own axes, in world space, read out of the view-projection.
   *
   * Column 0 is the world direction that increases clip x, and column 1 the one
   * that increases clip y -- which is screen-right and screen-up by definition,
   * whatever the camera's yaw and elevation are. Taking them from the matrix
   * rather than from the camera's yaw is what keeps this correct without a
   * second copy of the camera model in a shader (ADR-006 §4 owns that model).
   *
   * `g_viewProjection` is row_major, and HLSL's `m[i]` indexes rows whatever
   * the storage layout, so a column is one component from each of three rows.
   */
  const float3 screenRight = normalize(float3(g_viewProjection[0][0], g_viewProjection[1][0], g_viewProjection[2][0]));
  const float3 screenUp = normalize(float3(g_viewProjection[0][1], g_viewProjection[1][1], g_viewProjection[2][1]));

  // Sized in metres, so a lamp scales with zoom exactly as the hull under it
  // does -- an honest fitting rather than a screen-space dot that grows into a
  // dinner plate when the camera pulls back.
  const float2 corner = float2((_vertexId & 1u) != 0u ? 1.0f : -1.0f, (_vertexId & 2u) != 0u ? 1.0f : -1.0f);
  const float halfSize = 0.5f * lamp.positionAndSize.w * swell;
  world += screenRight * (corner.x * halfSize) + screenUp * (corner.y * halfSize);

  VertexOutput output;
  output.clipPosition = mul(float4(world, 1.0f), g_viewProjection);
  output.local = corner;
  output.colour = lamp.colourAndMode.rgb;
  output.opacity = opacity;
  return output;
}
