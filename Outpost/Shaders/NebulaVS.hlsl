// NebulaVS.hlsl -- one oversized triangle, no vertex buffer (ADR-006 §1).

#include "Nebula.hlsli"

VertexOutput VertexMain(uint _vertexId : SV_VertexID)
{
  // One oversized triangle rather than two: no diagonal seam, and no pixel
  // shaded twice along it. Ids 0,1,2 give (0,0), (2,0), (0,2), which becomes
  // (-1,-1), (3,-1), (-1,3) -- a triangle containing the whole NDC square.
  // The casts are explicit because the compile runs with warnings as errors.
  const float2 corner = float2(float((_vertexId << 1) & 2u), float(_vertexId & 2u));
  const float2 ndc = corner * 2.0f - 1.0f;

  VertexOutput output;
  // z = 0 and depth testing is off; the pass owns no depth opinion at all.
  output.clipPosition = float4(ndc, 0.0f, 1.0f);
  output.ndc = ndc;
  return output;
}
