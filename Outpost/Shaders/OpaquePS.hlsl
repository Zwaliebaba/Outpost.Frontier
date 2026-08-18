// OpaquePS.hlsl -- flat-shaded hulls, five shared materials, emissive accents
// (ADR-006 §6).
//
// One fixed directional light and a hemispherical ambient, and nothing else:
// the Darwinia look is silhouette and colour, not shading detail, and every
// feature not in that sentence is a feature to argue for later.

#include "Opaque.hlsli"

float4 PixelMain(VertexOutput _input) : SV_Target
{
  const float3 normal = normalize(_input.worldNormal);
  const float4 material = g_materialAlbedo[g_materialIndex];

  const float diffuse = saturate(dot(normal, -g_sunDirection.xyz));

  // Hemispherical ambient: sky above, the dark of the plane below. On a
  // flat-shaded hull this is what keeps an unlit face readable instead of
  // black, which is the difference between a silhouette and a hole.
  const float hemisphere = saturate(normal.y * 0.5f + 0.5f);
  const float3 ambient = lerp(g_ambientGround.rgb, g_ambientSky.rgb, hemisphere);

  const float3 lit = material.rgb * (ambient + g_sunColour.rgb * g_sunColour.w * diffuse);

  // Emissive is the only channel team colour touches. Hulls are never tinted by
  // relationship or selection -- those are the overlay pass's channels
  // (ADR-006 §7), and mixing them here is how an icon system stops reading.
  const float3 glow = material.rgb * material.w * g_teamEmissive[_input.team % TEAM_COLOUR_COUNT].rgb;

  return float4(lit + glow, 1.0f);
}
