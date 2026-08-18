#ifndef OUTPOST_NEBULA_HLSLI
#define OUTPOST_NEBULA_HLSLI

// What NebulaVS and NebulaPS both have to agree about (ADR-006 §1).
//
// FrameConstants (b0) is deliberately absent. This pass reads nothing from it,
// and declaring a cbuffer it does not use would mean a second copy of that
// layout -- including both array sizes -- free to drift from Opaque.hlsli's.
// The root signature is a superset; an unused slot is not an error, and
// NebulaPass::Record does not bind one.

struct VertexOutput
{
  float4 clipPosition : SV_Position;
  float2 ndc : NDC;
};

#endif // OUTPOST_NEBULA_HLSLI
