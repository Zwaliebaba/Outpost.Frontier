// The fixed-function pipeline of Direct3D 8 as one vertex and one pixel
// shader that branch on the per-draw constants (decision record 0004,
// point 6; work package P4.2).
//
// The constant buffer is laid out by neuron-client's render::shader::
// constants::pack; every field has the register given there (a Rust test
// compares the packoffsets below with it). Matrices are row-major and the
// file is compiled with /Zpr, so mul(v, m) is Direct3D's v * M. Lighting is
// in camera space: the packer has already multiplied world by view, built
// the normal matrices and moved the lights into camera space.
//
// The formulas are Direct3D 9's documented fixed-function ones ("Mathematics
// of Lighting", "Texture Coordinate Processing", D3DTEXTUREOP, fog). Where
// the documentation is silent the choice is marked ASSUMPTION; image replay
// (record 0004, section 2) is what confirms or corrects them.
//
// Entry points: vs_main, and ps_main compiled twice, with FLAT defined for
// SHADEMODE FLAT (colours not interpolated: the first vertex's, the leading
// vertex of Direct3D 12 as of Direct3D 8).

struct Light
{
    float4 diffuse;
    float4 specular;
    float4 ambient;
    float4 position_range;    // camera space; range
    float4 direction_falloff; // camera space; spot falloff
    float4 attenuation_type;  // a0, a1, a2; 1 point, 2 spot, 3 directional
    float4 cone;              // cos(theta / 2), cos(phi / 2)
};

cbuffer Draw : register(b0)
{
    float4x4 worldview[4]      : packoffset(c0);
    float4x4 normal_matrix[4]  : packoffset(c16);
    float4x4 projection        : packoffset(c32);
    float4x4 tex_transform[2]  : packoffset(c36);
    float4 material_diffuse    : packoffset(c44);
    float4 material_ambient    : packoffset(c45);
    float4 material_specular   : packoffset(c46);
    float4 material_emissive   : packoffset(c47);
    float4 global_ambient      : packoffset(c48);
    float4 scalars             : packoffset(c49); // power, fog start, end, density
    float4 fog_color           : packoffset(c50);
    float4 texture_factor      : packoffset(c51);
    float4 viewport            : packoffset(c52); // x, y, width, height
    float4 offsets             : packoffset(c53); // pixel offset x, y; alpha ref
    uint4 flags                : packoffset(c54); // vertex flags, blend matrices, fog, alpha func
    uint4 sources              : packoffset(c55); // diffuse, specular, ambient, emissive
    uint4 counts               : packoffset(c56); // lights, coordinate sizes, samplers
    uint4 stage[6]             : packoffset(c57); // per stage: colour, alpha, misc
    Light lights[16]           : packoffset(c63);
};

// render::shader::constants::vertex_flags
#define PRETRANSFORMED    (1u << 0)
#define HAS_NORMAL        (1u << 1)
#define HAS_DIFFUSE       (1u << 2)
#define HAS_SPECULAR      (1u << 3)
#define LIGHTING          (1u << 4)
#define COLOR_VERTEX      (1u << 5)
#define SPECULAR_ENABLE   (1u << 6)
#define LOCAL_VIEWER      (1u << 7)
#define NORMALIZE_NORMALS (1u << 8)
#define TEXTURE0          (1u << 9)
#define CUBE0             (1u << 11)

// render::shader::constants::fog_bits
#define FOG_ENABLE (1u << 0)
#define FOG_RANGE  (1u << 12)

// render::shader::samplers::COUNT
#define SAMPLERS 1050

Texture2D textures[2] : register(t0);
// Cube maps (LoadCubemap) of stages 0 and 1; a null cube view otherwise.
TextureCube cubes[2] : register(t2);
SamplerState samplers[SAMPLERS] : register(s0);

struct VsIn
{
    float4 position : POSITION;
    float4 blend    : BLENDWEIGHT;
    float3 normal   : NORMAL;
    float4 diffuse  : COLOR0;
    float4 specular : COLOR1;
    float4 tc0 : TEXCOORD0;
    float4 tc1 : TEXCOORD1;
    float4 tc2 : TEXCOORD2;
    float4 tc3 : TEXCOORD3;
    float4 tc4 : TEXCOORD4;
    float4 tc5 : TEXCOORD5;
    float4 tc6 : TEXCOORD6;
    float4 tc7 : TEXCOORD7;
};

struct VsOut
{
    float4 position  : SV_Position;
    float4 diffuse   : COLOR0;
    float4 specular  : COLOR1;
    float4 coord0    : TEXCOORD0;
    float4 coord1    : TEXCOORD1;
    float2 fog_depth : TEXCOORD2; // vertex fog factor, depth for table fog
};

bool has(uint bit)
{
    return (flags.x & bit) != 0;
}

// D3DFOGMODE: 1 exp, 2 exp2, 3 linear.
float fog_factor(uint mode, float d)
{
    float start = scalars.y;
    float end = scalars.z;
    float density = scalars.w;
    float f = 1.0;
    if (mode == 1)
        f = exp(-(d * density));
    else if (mode == 2)
        f = exp(-((d * density) * (d * density)));
    else if (mode == 3)
        f = end == start ? 1.0 : (end - d) / (end - start);
    return saturate(f);
}

// A material colour from its D3DMATERIALCOLORSOURCE: a vertex colour that
// the vertex has, while COLORVERTEX is on, else the material's.
float4 material(uint source, float4 fallback, VsIn v)
{
    float4 result = fallback;
    if (has(COLOR_VERTEX))
    {
        if (source == 1 && has(HAS_DIFFUSE))
            result = v.diffuse;
        else if (source == 2 && has(HAS_SPECULAR))
            result = v.specular;
    }
    return result;
}

void light_vertex(VsIn v, float3 p, float3 n, out float4 diffuse, out float4 specular)
{
    float4 cd = material(sources.x, material_diffuse, v);
    float4 cs = material(sources.y, material_specular, v);
    float4 ca = material(sources.z, material_ambient, v);
    float4 ce = material(sources.w, material_emissive, v);
    float power = scalars.x;
    float3 ambient_sum = 0;
    float3 diffuse_sum = 0;
    float3 specular_sum = 0;
    for (uint i = 0; i < counts.x; i++)
    {
        Light l = lights[i];
        uint kind = (uint)l.attenuation_type.w;
        float3 to_light = float3(0, 0, 0);
        float atten = 1.0;
        float spot = 1.0;
        if (kind == 3)
        {
            to_light = -normalize(l.direction_falloff.xyz);
        }
        else if (kind == 1 || kind == 2)
        {
            float3 d = l.position_range.xyz - p;
            float dist = length(d);
            if (dist > l.position_range.w)
                continue;
            to_light = dist > 0.0 ? d / dist : float3(0, 0, 0);
            float denom = l.attenuation_type.x + l.attenuation_type.y * dist
                + l.attenuation_type.z * dist * dist;
            atten = denom > 0.0 ? 1.0 / denom : 1.0;
            if (kind == 2)
            {
                float rho = dot(-normalize(l.direction_falloff.xyz), to_light);
                float cos_theta = l.cone.x;
                float cos_phi = l.cone.y;
                if (rho <= cos_phi)
                    spot = 0.0;
                else if (rho <= cos_theta)
                    spot = pow(saturate((rho - cos_phi) / (cos_theta - cos_phi)),
                               l.direction_falloff.w);
            }
        }
        else
        {
            continue;
        }
        float k = atten * spot;
        ambient_sum += l.ambient.rgb * k;
        float ndl = dot(n, to_light);
        if (ndl > 0.0)
        {
            diffuse_sum += l.diffuse.rgb * ndl * k;
            if (has(SPECULAR_ENABLE))
            {
                // H = norm(norm(Cp - Vp) + Ldir); (0, 0, 1) for the viewer
                // without LOCALVIEWER.
                float3 eye = has(LOCAL_VIEWER) ? normalize(-p) : float3(0, 0, 1);
                float3 h = normalize(eye + to_light);
                specular_sum += l.specular.rgb * pow(max(dot(n, h), 0.0), power) * k;
            }
        }
    }
    diffuse.rgb = saturate(ce.rgb + ca.rgb * (global_ambient.rgb + ambient_sum)
                           + cd.rgb * diffuse_sum);
    diffuse.a = saturate(cd.a);
    specular.rgb = saturate(cs.rgb * specular_sum);
    // The specular alpha carries the application's fog factor.
    specular.a = v.specular.a;
}

float4 texcoord_set(VsIn v, uint set)
{
    float4 sets[8] = { v.tc0, v.tc1, v.tc2, v.tc3, v.tc4, v.tc5, v.tc6, v.tc7 };
    uint size = set < 8 ? (counts.y >> (set * 4)) & 15 : 0;
    float4 c = set < 8 ? sets[min(set, 7)] : float4(0, 0, 0, 0);
    // A missing set reads (0, 0). ASSUMPTION: the coordinates go into the
    // transform with a 1 after the last one and zeros beyond, so a 2D set's
    // translation sits in row 3 (_31, _32).
    if (size == 0)
    {
        size = 2;
        c = 0;
    }
    float4 t = 0;
    [unroll] for (uint i = 0; i < 4; i++)
        t[i] = i < size ? c[i] : (i == size ? 1.0 : 0.0);
    return t;
}

float4 stage_coord(VsIn v, uint s, float3 p, float3 n)
{
    uint index = stage[s * 3 + 2].y;
    uint generate = index >> 16;
    uint set = index & 0xffff;
    float4 t = texcoord_set(v, set);
    // Pre-transformed vertices take their coordinates as they are.
    if (!has(PRETRANSFORMED))
    {
        if (generate == 1)
        {
            t = float4(n, 1);
        }
        else if (generate == 2)
        {
            t = float4(p, 1);
        }
        else if (generate == 3)
        {
            float3 eye = has(LOCAL_VIEWER) ? normalize(-p) : float3(0, 0, 1);
            t = float4(2.0 * dot(eye, n) * n - eye, 1);
        }
        if ((stage[s * 3 + 2].z & 0xff) != 0)
            t = mul(t, tex_transform[s]);
    }
    return t;
}

VsOut vs_main(VsIn v)
{
    VsOut o;
    float3 p = 0;
    float3 n = 0;
    if (has(PRETRANSFORMED))
    {
        float rhw = v.position.w;
        float w = rhw != 0.0 ? 1.0 / rhw : 1.0;
        float2 ndc = float2((v.position.x - viewport.x) / viewport.z * 2.0 - 1.0,
                            1.0 - (v.position.y - viewport.y) / viewport.w * 2.0);
        o.position = float4(ndc * w, v.position.z * w, w);
    }
    else
    {
        // Vertex blending: the first matrices take the vertex's weights,
        // the last one minus their sum.
        uint matrices = max(flags.y, 1u);
        float sum = 0.0;
        [unroll] for (uint i = 0; i < 4; i++)
        {
            float weight = 0.0;
            if (i + 1 < matrices)
            {
                weight = v.blend[i];
                sum += weight;
            }
            else if (i + 1 == matrices)
            {
                weight = 1.0 - sum;
            }
            if (weight != 0.0)
            {
                p += weight * mul(float4(v.position.xyz, 1), worldview[i]).xyz;
                n += weight * mul(float4(v.normal, 0), normal_matrix[i]).xyz;
            }
        }
        if (has(NORMALIZE_NORMALS) && dot(n, n) > 0.0)
            n = normalize(n);
        o.position = mul(float4(p, 1), projection);
    }
    // Direct3D 8 samples pixels at their integer coordinates, Direct3D 12
    // at their centres: half a pixel right and down (record 0004, point 7).
    o.position.xy += offsets.xy * o.position.w;

    if (has(LIGHTING) && !has(PRETRANSFORMED))
    {
        light_vertex(v, p, n, o.diffuse, o.specular);
    }
    else
    {
        o.diffuse = v.diffuse;
        o.specular = v.specular;
    }

    uint fog = flags.z;
    uint table = (fog >> 4) & 15;
    uint vertex_mode = (fog >> 8) & 15;
    float f = o.specular.a;
    if ((fog & FOG_ENABLE) != 0 && table == 0 && vertex_mode != 0 && !has(PRETRANSFORMED))
        f = fog_factor(vertex_mode, (fog & FOG_RANGE) != 0 ? length(p) : p.z);
    o.fog_depth = float2(f, o.position.w);

    o.coord0 = stage_coord(v, 0, p, n);
    o.coord1 = stage_coord(v, 1, p, n);
    return o;
}

#ifdef FLAT
#define COLOUR_INTERPOLATION nointerpolation
#else
#define COLOUR_INTERPOLATION
#endif

struct PsIn
{
    float4 position : SV_Position;
    COLOUR_INTERPOLATION float4 diffuse  : COLOR0;
    COLOUR_INTERPOLATION float4 specular : COLOR1;
    float4 coord0    : TEXCOORD0;
    float4 coord1    : TEXCOORD1;
    float2 fog_depth : TEXCOORD2;
};

// D3DTA_*: the source in the low four bits, then COMPLEMENT and
// ALPHAREPLICATE. ASSUMPTION: replicate before complement.
float4 argument(uint a, float4 diffuse, float4 current, float4 tex, float4 temp, float4 specular)
{
    uint source = a & 15;
    float4 v = current;
    if (source == 0)
        v = diffuse;
    else if (source == 2)
        v = tex;
    else if (source == 3)
        v = texture_factor;
    else if (source == 4)
        v = specular;
    else if (source == 5)
        v = temp;
    if ((a & 0x20) != 0)
        v = v.aaaa;
    if ((a & 0x10) != 0)
        v = 1.0 - v;
    return v;
}

// D3DTEXTUREOP on arguments 0, 1 and 2.
float4 combine(uint op, float4 a0, float4 a1, float4 a2, float4 diffuse, float4 tex, float4 current)
{
    // PREMODULATE (17) and unknown operations: the first argument.
    float4 r = a1;
    if (op == 3)
        r = a2;
    else if (op == 4)
        r = a1 * a2;
    else if (op == 5)
        r = 2.0 * a1 * a2;
    else if (op == 6)
        r = 4.0 * a1 * a2;
    else if (op == 7)
        r = a1 + a2;
    else if (op == 8)
        r = a1 + a2 - 0.5;
    else if (op == 9)
        r = 2.0 * (a1 + a2 - 0.5);
    else if (op == 10)
        r = a1 - a2;
    else if (op == 11)
        r = a1 + a2 - a1 * a2;
    else if (op == 12)
        r = lerp(a2, a1, diffuse.a);
    else if (op == 13)
        r = lerp(a2, a1, tex.a);
    else if (op == 14)
        r = lerp(a2, a1, texture_factor.a);
    else if (op == 15)
        r = a1 + a2 * (1.0 - tex.a);
    else if (op == 16)
        r = lerp(a2, a1, current.a);
    else if (op == 18)
        r = float4(a1.rgb + a1.a * a2.rgb, a1.a);
    else if (op == 19)
        r = float4(a1.rgb * a2.rgb + a1.a, a1.a);
    else if (op == 20)
        r = float4((1.0 - a1.a) * a2.rgb + a1.rgb, a1.a);
    else if (op == 21)
        r = float4((1.0 - a1.rgb) * a2.rgb + a1.a, a1.a);
    // Bump mapping changes the next stage's coordinates, which the shader
    // does not emulate; the colour passes through.
    else if (op == 22 || op == 23)
        r = current;
    else if (op == 24)
        r = saturate(dot((a1.rgb - 0.5) * 2.0, (a2.rgb - 0.5) * 2.0)).xxxx;
    else if (op == 25)
        r = a0 + a1 * a2;
    else if (op == 26)
        r = a0 * a1 + (1.0 - a0) * a2;
    return r;
}

// The coordinates a stage samples at: the projective divide by the last
// coordinate the transform produced, with D3DTTFF_PROJECTED.
float2 stage_uv(uint s, float4 coord)
{
    uint tflags = stage[s * 3 + 2].z;
    uint count = tflags & 0xff;
    float2 uv = coord.xy;
    if ((tflags & 0x100) != 0 && count > 0 && !has(PRETRANSFORMED))
    {
        float q = coord[min(count, 4u) - 1];
        uv = q != 0.0 ? coord.xy / q : coord.xy;
    }
    return uv;
}

bool bound(uint s)
{
    return (flags.x & (TEXTURE0 << s)) != 0;
}

bool cube(uint s)
{
    return (flags.x & (CUBE0 << s)) != 0;
}

// A stage's sample: its 2D texture at the stage's two coordinates, or its
// cube map in the direction of the first three (D3DTSS_TEXTURETRANSFORMFLAGS
// count 3, no projective divide). Both are sampled outside flow control;
// the unbound one reads its null view.
float4 stage_sample(uint s, float4 flat, float4 cubed)
{
    return cube(s) ? cubed : flat;
}

// A stage's texture, sampled outside flow control (an unbound stage has a
// null descriptor). Without a texture, D3DTA_TEXTURE reads the diffuse
// colour: documented for ALPHAARG1 (D3DTEXTURESTAGESTATETYPE), and the
// colour case where it matters ends the cascade (below).
float4 texel_or_diffuse(uint s, float4 sampled, float4 diffuse)
{
    return bound(s) ? sampled : diffuse;
}

// D3DCMPFUNC: 1 never to 8 always.
bool compare(uint func, float a, float b)
{
    bool passes = true;
    if (func == 1)
        passes = false;
    else if (func == 2)
        passes = a < b;
    else if (func == 3)
        passes = a == b;
    else if (func == 4)
        passes = a <= b;
    else if (func == 5)
        passes = a > b;
    else if (func == 6)
        passes = a != b;
    else if (func == 7)
        passes = a >= b;
    return passes;
}

float4 ps_main(PsIn i) : SV_Target
{
    float4 diffuse = i.diffuse;
    float4 specular = i.specular;
    float4 current = diffuse;
    float4 temp = 0;
    float4 texels[2] = {
        texel_or_diffuse(0, stage_sample(0, textures[0].Sample(samplers[counts.z], stage_uv(0, i.coord0)),
                                         cubes[0].Sample(samplers[counts.z], i.coord0.xyz)), diffuse),
        texel_or_diffuse(1, stage_sample(1, textures[1].Sample(samplers[counts.w], stage_uv(1, i.coord1)),
                                         cubes[1].Sample(samplers[counts.w], i.coord1.xyz)), diffuse)
    };
    bool active = true;
    [unroll] for (uint s = 0; s < 2; s++)
    {
        uint4 colour = stage[s * 3];
        uint4 alpha = stage[s * 3 + 1];
        uint4 misc = stage[s * 3 + 2];
        // DISABLE ends the cascade, and so does a stage whose COLORARG1 is
        // D3DTA_TEXTURE without a texture ("Texture Blending", Direct3D 9).
        if (colour.x == 1 || (colour.y == 2 && !bound(s)))
            active = false;
        if (!active)
            continue;
        float4 tex = texels[s];
        float4 c = combine(colour.x,
                           argument(colour.y, diffuse, current, tex, temp, specular),
                           argument(colour.z, diffuse, current, tex, temp, specular),
                           argument(colour.w, diffuse, current, tex, temp, specular),
                           diffuse, tex, current);
        float a = current.a;
        if (colour.x == 24)
        {
            a = c.x; // DOTPRODUCT3 goes to every channel
        }
        else if (alpha.x != 1)
        {
            a = combine(alpha.x,
                        argument(alpha.y, diffuse, current, tex, temp, specular),
                        argument(alpha.z, diffuse, current, tex, temp, specular),
                        argument(alpha.w, diffuse, current, tex, temp, specular),
                        diffuse, tex, current).a;
        }
        float4 result = saturate(float4(c.rgb, a));
        if ((misc.x & 15) == 5)
            temp = result;
        else
            current = result;
    }
    if (has(SPECULAR_ENABLE))
        current.rgb = saturate(current.rgb + specular.rgb);
    uint fog = flags.z;
    if ((fog & FOG_ENABLE) != 0)
    {
        uint table = (fog >> 4) & 15;
        float f = table != 0 ? fog_factor(table, i.fog_depth.y) : saturate(i.fog_depth.x);
        current.rgb = lerp(fog_color.rgb, current.rgb, f);
    }
    if (flags.w != 0)
    {
        // The alpha test compares 8-bit values.
        float a8 = round(saturate(current.a) * 255.0);
        if (!compare(flags.w, a8, offsets.z))
            discard;
    }
    return current;
}
