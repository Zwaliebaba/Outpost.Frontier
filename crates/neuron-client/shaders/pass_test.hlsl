// A test program for the programmable passes (decision record 0005): the
// WARP tests of platform::d3d12::backend::ex draw with it. Shader model 6.1,
// HLSL 2021.
//
// The pass root signature: the vertex registers at b0 of the vertex stage,
// the pixel registers at b0 of the pixel stage, views t0 to t15 and
// samplers s0 to s15 for the pixel stage, slot n sampling with sampler n.
// The full-screen quad's vertices are XYZ | TEX1 in clip space; the vertex
// shader passes them through.

cbuffer PixelRegisters : register(b0)
{
    float4 c[224];
};

Texture2D<float4> scene_depth : register(t0);
SamplerState scene_depth_sampler : register(s0);
Texture3D<float4> volume : register(t1);
SamplerState volume_sampler : register(s1);

struct VsIn
{
    float3 position : POSITION;
    float2 uv : TEXCOORD0;
};

struct VsOut
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

VsOut vs_main(VsIn i)
{
    VsOut o;
    o.position = float4(i.position, 1.0);
    o.uv = i.uv;
    return o;
}

// Pixel register 0 as the colour.
float4 ps_constant(VsOut i) : SV_Target
{
    return c[0];
}

// The scene depth as grey.
float4 ps_depth(VsOut i) : SV_Target
{
    float d = scene_depth.Sample(scene_depth_sampler, i.uv).r;
    return float4(d, d, d, 1.0);
}

// The volume at the pixel's coordinates and depth c[0].x.
float4 ps_volume(VsOut i) : SV_Target
{
    return volume.Sample(volume_sampler, float3(i.uv, c[0].x));
}
