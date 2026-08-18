#ifndef OUTPOST_PASS_CONSTANTS_HLSLI
#define OUTPOST_PASS_CONSTANTS_HLSLI

// The per-pass constant block (b1), and the only declaration of it.
//
// Both passes bind this slot. Before the shaders were split there were two
// copies of the layout in two files, kept in step by a comment asking whoever
// edited one to remember the other -- and two shaders declaring one cbuffer
// differently is a class of bug with no error message. Splitting each shader
// into a vertex and a pixel file would have made four copies, so the shared
// declarations moved here instead and the comment stopped being load-bearing.
//
// One copy remains and cannot be shared: `PassConstants` in GpuPipelines.h is
// C++. That one still carries the warning, and it is now the only pairing to
// check rather than one of several.

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

#endif // OUTPOST_PASS_CONSTANTS_HLSLI
