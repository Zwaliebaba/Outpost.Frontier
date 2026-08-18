// Opaque.hlsl -- the one opaque pass of the MVP frame (ADR-006 §6).
//
// Flat-shaded hulls, five shared materials, emissive accents. One fixed
// directional light and a hemispherical ambient, and nothing else: the
// Darwinia look is silhouette and colour, not shading detail, and every
// feature not in that sentence is a feature to argue for later.
//
// Instancing is a second vertex stream rather than a structured buffer, so a
// class is one DrawIndexedInstanced per material with no descriptor churn.
// Normals are per-face as authored, so `flat` shading needs no shader work --
// the exporter already duplicated the corners.

#define MESH_MATERIAL_COUNT 5
#define TEAM_COLOUR_COUNT 4

// row_major, matching how DirectXMath stores an XMFLOAT4X4, so the upload is a
// memcpy. The alternative -- HLSL's column-major default plus a transpose on
// the CPU -- is the same result with one more place to get the convention
// wrong.
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

// Per-pass, and deliberately bound even though the opaque pass reads nothing
// from it: the slot is ADR-006 §12's, and OverlayWorld and Ui are written
// against these numbers (S8, S11).
//
// Must match PassConstants in GpuPipelines.h and the declaration in Nebula.hlsl.
// Two shaders declaring one cbuffer differently is a class of bug with no error
// message, so the three are edited together or not at all.
cbuffer PassConstants : register(b1)
{
  float4 g_viewportSize;      // xy = pixels, zw = 1/pixels.
  float4 g_planeAxes;         // xy = screen-right on the plane, zw = screen-up.
  float4 g_planeOrigin;       // xy = the plane point at the centre of the view.
  float4 g_planeRightPerNdc;  // xy = plane metres per unit of NDC x.
  float4 g_planeUpPerNdc;     // xy = plane metres per unit of NDC y.
  float4 g_nebulaTint;        // rgb = linear tint, w = peak intensity.
  float4 g_nebulaTile;        // x = 1 / tile metres.
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
