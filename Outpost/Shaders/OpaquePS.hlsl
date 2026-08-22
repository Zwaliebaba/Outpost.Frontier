// OpaquePS.hlsl -- flat-shaded hulls, five shared materials, emissive accents
// (ADR-006 §6).
//
// Three terms and nothing else -- key, hemispherical fill, bounce. The Darwinia
// look is silhouette and colour, not shading detail, but a *flat*-shaded hull
// only reads as three-dimensional when adjacent facets are lit differently, so
// each of the three earns its constant:
//
//  - the **key** is one hard directional, which is what gives two neighbouring
//    faces distinct tones at all;
//  - the **fill** is hemispherical, so a face turned away from the key is a
//    shape rather than a hole;
//  - the **bounce** is a weak green directional from the camera's side, so a
//    hull's shadow side separates from a near-black background instead of
//    dissolving into it.
//
// Everything not in that list is a feature to argue for later.

#include "Opaque.hlsli"

float4 PixelMain(VertexOutput _input) : SV_Target
{
  const float3 normal = normalize(_input.worldNormal);
  const float4 material = g_materialAlbedo[g_materialIndex];

  /*
   * An emissive material takes no key.
   *
   * A stripe or an engine bell is a light source, not a lit panel: keying it
   * would make it brightest on the faces that already face the sun, so a hull's
   * accents would flicker in and out as it turned. Unlit, they are the same
   * green whichever way the ship is pointing, which is what makes them read as
   * markings rather than as gloss. Fill and bounce still apply -- they are what
   * keeps the *facets* of a bell distinguishable under the glow.
   */
  const float keyLit = material.w > 0.0f ? 0.0f : 1.0f;
  const float diffuse = saturate(dot(normal, -g_sunDirection.xyz)) * keyLit;

  // Hemispherical fill: sky above, the dark of the plane below. On a
  // flat-shaded hull this is what keeps an unlit face readable instead of
  // black, which is the difference between a silhouette and a hole.
  const float hemisphere = saturate(normal.y * 0.5f + 0.5f);
  const float3 ambient = lerp(g_ambientGround.rgb, g_ambientSky.rgb, hemisphere);

  const float bounce = saturate(dot(normal, -g_bounceDirection.xyz));

  const float3 lit = material.rgb * (ambient + g_sunColour.rgb * g_sunColour.w * diffuse +
                                     g_bounceColour.rgb * g_bounceColour.w * bounce);

  // Emissive is the only channel team colour touches. Hulls are never tinted by
  // relationship or selection -- those are the overlay pass's channels
  // (ADR-006 §7), and mixing them here is how an icon system stops reading.
  const float3 glow = material.rgb * material.w * g_teamEmissive[_input.team % TEAM_COLOUR_COUNT].rgb;

  return float4(lit + glow, 1.0f);
}
