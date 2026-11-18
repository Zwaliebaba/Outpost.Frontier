// The final blit (decision record 0004, point 8): the scene target to the
// swap chain's back buffer through the gamma ramp, which Direct3D 8 applied
// in hardware. Screenshots read the scene before this pass.
//
// One full-screen triangle from the vertex id, no vertex buffer. The pixel
// shader reads the scene texel at its own position (scene and target have
// the same size) and looks each channel up in the ramp: a 256 by 1 texture
// whose red, green and blue are the ramp's three channels as UNORM16.

Texture2D<float4> scene : register(t0);
Texture2D<float4> ramp : register(t1);

struct VsOut
{
    float4 position : SV_Position;
};

VsOut vs_main(uint id : SV_VertexID)
{
    VsOut o;
    float2 corner = float2((id << 1) & 2, id & 2);
    o.position = float4(corner * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return o;
}

float4 ps_main(VsOut i) : SV_Target
{
    float4 c = scene.Load(int3(i.position.xy, 0));
    int3 index = int3(round(saturate(c.rgb) * 255.0));
    return float4(ramp.Load(int3(index.r, 0, 0)).r,
                  ramp.Load(int3(index.g, 0, 0)).g,
                  ramp.Load(int3(index.b, 0, 0)).b,
                  1.0);
}
