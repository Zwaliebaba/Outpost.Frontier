#ifndef OUTPOST_FRAME_CONSTANTS_HLSLI
#define OUTPOST_FRAME_CONSTANTS_HLSLI

// The per-frame constant block (b0), and the only declaration of it.
//
// Split out of Opaque.hlsli when the overlay arrived: OverlayVS needs
// g_viewProjection and nothing else in here, and a shader that declared its own
// copy of the block to reach one field would be a second copy of two array
// sizes, free to drift. The nebula still declares none of this, because it
// reads none of it.
//
// Must match FrameConstants in GpuPipelines.h, including both array sizes.

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
  float4 g_sunDirection;    // xyz: the direction the key light travels, normalised.
  float4 g_sunColour;       // rgb, w = intensity.
  float4 g_ambientSky;      // rgb, the fill arriving from above.
  float4 g_ambientGround;   // rgb, the fill arriving from below.
  float4 g_bounceDirection; // xyz: the direction the bounce light travels, normalised.
  float4 g_bounceColour;    // rgb, w = intensity.
  float4 g_materialAlbedo[MESH_MATERIAL_COUNT]; // rgb = albedo, w = emissive strength.
  float4 g_teamEmissive[TEAM_COLOUR_COUNT];     // rgb = the tint on emissive materials only.

  /*
   * x = seconds since the client started; yzw reserved.
   *
   * Appended after the arrays rather than inserted among the light terms, so a
   * reader diffing the compiled register listing against the last one sees one
   * new register at the end instead of nine that moved.
   *
   * **Deliberately not a simulation clock.** The signal lamps are the only
   * thing that reads it and they are cosmetic (ADR-006 §6a): an unsynced wall
   * clock is what makes two clients show the same fleet blinking differently,
   * which is correct, where a replicated one would be a per-frame float in the
   * snapshot for no gameplay at all.
   */
  float4 g_frameTime;
};

#endif // OUTPOST_FRAME_CONSTANTS_HLSLI
