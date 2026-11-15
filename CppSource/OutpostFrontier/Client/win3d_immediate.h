// Author: Giperion.

#pragma once

// baseline `Client\cmn\OpenGL.cpp` -- an OpenGL immediate-mode emulator
// sitting on top of the D3D8-shaped IRenderPipeline. The source file names itself in the
// binary twice, in the glVertex overflow warning and in glEnd's invalid-mode warning.

// This is the layer the client draws all its flat geometry through: the fade quad, and anything
// else that would have been a glBegin/glVertex/glEnd block. Vertices are accumulated into two
// static buffers and handed to the pipeline in one draw call at glEnd.

// The vertex format is settled by the FVF baseline passes, 0x142 = XYZ | DIFFUSE | TEX1, which is
// twelve bytes of position, four of packed colour and eight of texture coordinate -- 24 bytes,
// exactly the stride the accumulator walks (0x18).

// Buffer sizes are baseline's: 0x3000 vertices and 0x3000 indices.

struct FSpaceCamera;

// The primitive modes glBegin takes. These are Win3D's own numbering, NOT OpenGL's -- glEnd
// maps them onto D3D primitive types, and two of them are emulated rather than mapped:
// LineLoop becomes a line strip plus one closing line, and Quads is turned into a triangle list
// through a generated index buffer.
enum EWin3DPrimitive
{
    kWin3DPoints        = 1,
    kWin3DLines         = 2,
    kWin3DLineStrip     = 3,
    kWin3DTriangles     = 4,
    kWin3DTriangleStrip = 5,
    kWin3DTriangleFan   = 6,
    kWin3DLineLoop      = 7,
    kWin3DQuads         = 8,
    kWin3DPolygon       = 9
};

// . Starts a primitive. Note the vertex counter is reset only when the buffer is
// not locked -- baseline's the field -- so a locked buffer accumulates across begins.
void Win3DBegin(int PrimitiveMode);

// . Adds a vertex at the current colour and texture coordinate. Silently drops the
// vertex when either buffer is full, and warns once a second while it is dropping them.
void Win3DVertex3f(float X, float Y, float Z);

// . Sets the current colour from four floats in 0..1. Each component is scaled,
// converted with ftol and CLAMPED to 0..255 -- baseline clamps after the conversion, not before,
// so an out-of-range float saturates rather than wrapping.
void Win3DColor4f(float R, float G, float B, float A);

// . The three-float form: each component scaled by 255 (the field), ftol, clamped
// to 0..255 afterwards like Win3DColor4f; the alpha byte is 0xff (`OR ECX,0xffffff00` on red).
void Win3DColor3f(float R, float G, float B);

// . The three-byte form; the alpha byte is left as 0xff.
void Win3DColor3ub(unsigned char R, unsigned char G, unsigned char B);

// . The pointer form of Win3DColor3ub: Rgb[0..2], alpha 0xff.
void Win3DColor3ubv(const unsigned char* Rgb);

// . The four-byte form: R, G, B, A packed straight into ARGB with no scale and no
// clamp -- the bytes are already in 0..255.
void Win3DColor4ub(unsigned char R, unsigned char G, unsigned char B, unsigned char A);

// . The pointer form of Win3DColor4ub: Rgba[0]=R, Rgba[1]=G, Rgba[2]=B, Rgba[3]=A.
void Win3DColor4ubv(const unsigned char* Rgba);

// . Sets the current texture coordinate that subsequent Win3DVertex3f calls stamp
// onto the vertex, the same way Win3DColor4f/3ub set the current colour.
void Win3DTexCoord2f(float U, float V);

// . Packs four 0..1 floats into a D3DCOLOR the same way Win3DColor4f does (scale by
// 255, ftol, clamp to a byte) and sets it as pipeline state 8 -- the swap-chain clear colour, not
// the immediate-mode "current vertex colour" -- so it takes effect independently of glColor4f.
// Called by the space-camera background-clear path right before Win3DSetViewport/ClearWin3DBuffers.
void Win3DSetPipelineClearColor(float R, float G, float B, float A);

// . Ends the primitive and issues the draw.
void Win3DEnd();

// The GL capability names glEnable/glDisable understand: five for glEnable, and GL_FOG as a
// sixth for glDisable only. Anything else warns and does nothing.
enum EWin3DCapability
{
    kWin3DCullFace  = 0xb44,
    kWin3DLightingCap = 0xb50,
    kWin3DFog       = 0xb60,   // glDisable only (0x00421f02); glEnable warns on it
    kWin3DDepthTest = 0xb71,
    kWin3DBlend     = 0xbe2,
    kWin3DTexture2D = 0xde1
};

// D3DBLEND factors, for the blend-func setter below. Five and six are the pair the fade uses.
enum EWin3DBlendFactor
{
    kWin3DBlendOne         = 2,
    kWin3DBlendSrcAlpha    = 5,
    kWin3DBlendInvSrcAlpha = 6
};

// / the routine. Note both always write ALL THREE depth states when handed
// GL_DEPTH_TEST, not just the enable -- which is why the depth cache lives in this module and
// the two setters below re-apply from it.
void Win3DEnable(int Capability);
void Win3DDisable(int Capability);

// . Binds a texture handle for the next glEnd's texture-stage apply and marks a
// texture as bound -- the counterpart of Win3DEnable(kWin3DTexture2D), which only raises the
// flag. baseline's glBindTexture.
void Win3DBindTexture(void* TextureHandle);

// / the routine. Texture stage 0's D3DTSS_MAGFILTER/MINFILTER, linear or point.
// is baseline's __fastcall form that hands its single argument straight back --
// kept so a caller can chain it inline the way baseline's call sites do.
void* Win3DSetTextureFilterLinear(void* PassThrough);
void Win3DSetTextureFilterPoint();

// . The depth compare function (a D3DCMPFUNC), plus the same re-apply.
void Win3DSetDepthFunc(int CompareFunc);
// . D3DRS_SRCBLEND / D3DRS_DESTBLEND, and re-applies D3DRS_ALPHABLENDENABLE from
// the cached flag. The arguments are D3D blend factors: 5 is SRCALPHA and 6 is INVSRCALPHA,
// which is the pair the fade uses.
void Win3DSetBlendFunc(int SourceFactor, int DestFactor);

// . D3DRS_ZWRITEENABLE, plus a re-apply of the cached ZFUNC and ZENABLE.
void Win3DSetDepthWrite(int bEnabled);

// . Sets the pipeline viewport from a camera's ViewRect (slot 0x14), or to the whole
// surface when Cam is null. The file and line are baseline's own diagnostic arguments, carried
// through so the call sites keep reading the way they do in the binary.
struct ICamera;
void Win3DSetViewport(ICamera* Cam, const char* SourceFile, int SourceLine);

// . Answers baseline's own GL_VIEWPORT (0xba2) query out of the state Win3DSetViewport
// last installed. Any other query id is silently ignored -- baseline's glGetIntegerv only ever
// understood this one. Out4[1] is the flipped Y origin Win3DSetViewport's comment describes, not
// Top itself; that is what baseline stored and what baseline hands back.
void Win3DGetViewport(int Query, int* Out4);

// (cdecl X, Y, Width, Height, SourceFile, SourceLine) -- baseline's glViewport:
// stores the four values where Win3DGetViewport reads them and hands the pipeline the rectangle
// with Y flipped back, (X, SurfaceHeight - Y - Height, Width, Height). Y is the flipped origin a
// GL_VIEWPORT query returned, so a query / set pair restores a viewport exactly (the
// ShipViewControl draw does that, 0x00584518 / 0x00584667).
void Win3DSetViewportRect(int X, int Y, int Width, int Height, const char* SourceFile,
                          int SourceLine);

// . Win3DSetOrtho2D's shape, but the box comes from an arbitrary object's own
// viewport rect (the same Vtable[5] accessor Win3DSetViewport uses) instead of the display size.
void Win3DSetOrtho2DForCamera(FSpaceCamera* Cam);

// . Sets the projection, and resets the modelview to the identity on the way.

// The six arguments are glOrtho's, in glOrtho's order. Two things about it are worth knowing.
// The modelview reset happens BEFORE the cache test, so it runs on every call whether or not the
// projection itself changes. And the cache stores the new parameters even on the path where it
// decides nothing changed -- harmless, since on that path they are equal anyway, but it means
// the store is unconditional and the port keeps it that way.
void Win3DSetProjection(float Left, float Right, float Bottom, float Top, float NearPlane,
                        float FarPlane);

// . The 2D projection every flat overlay draws through: an orthographic box from
// (0, 0) at the top left to (Width, Height) at the bottom right, with near -1 and far 1. Top and
// bottom are the way round they are because screen Y runs downward.
void Win3DSetOrtho2D();

// . Perspective's sibling to Win3DSetProjection: same change-cache, sharing the same
// backing store (only the first four of its six slots), under cache "kind" 1 instead of ortho's
// kind 2. Unlike the ortho setter it does NOT touch the modelview.
void Win3DSetPerspective(float FovDeg, float Aspect, float NearPlane, float FarPlane);

// (cdecl Camera) -- Win3DSetPerspective(GetFovYDeg, GetAspect, GetZNear, GetZFar)
// of Camera (slots 0x1c / 0x28 / 0x20 / 0x24, called in the order far, near, aspect, fov).
// NOTE: space_scene_obj_draw.cpp still keeps a private copy of this function (with null
// guards baseline lacks); cli_nebula.cpp's copy was folded into this one.
void Win3DApplyCameraPerspective(ICamera* Camera);

// / the routine. Save and restore the whole projection -- cache included when the
// cache is on (kind + the four or six cached params, replayed through Win3DSetPerspective or
// Win3DSetProjection on pop) or the raw 4x4 matrix via the pipeline's own Get/SetProjection when
// the cache is off. baseline's glPushAttrib(GL_TRANSFORM_BIT) / glPopAttrib pair for this emulator.
void Win3DPushProjection();
void Win3DPopProjection();

// How many times the projection was actually pushed to the pipeline, and how many times the
// cache spared it (baseline .data 0x00664990 and 0x0066498c). baseline keeps both counters and never
// reads them; they are exposed here because they are the only way to see the cache working.
int Win3DGetProjectionSetCount();
int Win3DGetProjectionSkipCount();

// the render-state defaults RenderMgrCreateBuffers (the routine, 0x00424bdf)
// installs after the device is made: blend 2/2, depth test/write on, depth func 4, no alpha
// blend / texture / lighting, an empty accumulator, identity modelview, no cached projection.
void Win3DResetState();

// — called from the routine just before SwapBuffers: zeroes the projection
// set/skip counters and latches the field from game option LIKE_PETAL (0x3e).
void Win3DOnPresentFrame();

// The modelview the emulator hands to the pipeline at glEnd (baseline's .data 0x006648dc, twelve
// floats). Exposed because the projection setter is what writes it and glEnd is what reads it.
extern float GWin3DModelview[12];

// A read-only window on the accumulator. baseline has no such thing -- its buffers are plain .data
// and anything that wanted to look simply read them. These exist so the state can be checked
// without a live pipeline, which is the only way the packing and the counters can be pinned down
// offline; nothing in the port's own code path calls them.
int Win3DGetVertexCount();
int Win3DGetIndexCount();
unsigned long Win3DGetCurrentColour();

// Reads back one accumulated vertex. Returns false for an index outside what has been written.
bool Win3DGetVertex(int Index, float* OutXyz3, unsigned long* OutColour);

// . A small GLU-style sphere helper living in the same baseline source file as the
// rest of this emulator (OpenGL.cpp), adjacent in the binary to Win3DBegin/Vertex/End. Fills
// OutPoints (Cols*Rows*3 floats, one XYZ triplet per grid point) with a UV-sphere point grid:
// Rows rings from pole to pole (phi 0..pi) of Cols points each (theta 0..2pi), at Radius.

// baseline builds the per-column cosine/sine table into two 256-float stack arrays indexed 1..Cols
// (index 0 unused) and reads table[Cols] for the last column rather than wrapping back to
// table[0] -- so the last column is not bit-identical to the first even though both are at
// theta==0 mod 2pi. The port keeps that quirk and the same 256-column ceiling.
void Win3DBuildSpherePoints(float* OutPoints, float Radius, int Cols, int Rows);

// . Draws the grid Win3DBuildSpherePoints produces: every interior latitude ring
// (all rows except the first and last) as a closed line loop, then every longitude line (all
// rows, one column) as a line strip. Disables kWin3DTexture2D first (the routine(0xde1)).
void Win3DDrawSphereGrid(const float* Points, int Cols, int Rows);

// (cdecl Camera, Xform12, Radius, Cols, Rows) -- a wireframe sphere: the points into
// the function-static 0x3000-byte buffer the field (Win3DBuildSpherePoints), the modelview
// from Camera and Xform (the routine, Win3DBindModelview), then Win3DDrawSphereGrid. Caller:
// HUD_Target's target view 0x004e3e63 (16 x 16).
void Win3DDrawWireSphere(void* Camera, float const* Xform12, float Radius, int Cols, int Rows);

// / the routine (cdecl Out, A, B): Out = A + B / Out = A - B, per component, each
// sum rounded to float on its FSTP. Out may alias an input (all three loads precede the stores).
void Win3DVec3Add(float* Out, float const* A, float const* B);
void Win3DVec3Sub(float* Out, float const* A, float const* B);

// The argument of the fuzzy-line drawers, 0x50 bytes. baseline has no name for it; the only
// builder is the tractor beam draw (the branch), which fills it on its own stack. Offsets are
// the ones both drawers read.
struct FWin3DFuzzyLine
{
    unsigned char bCurved;   // +0x00 0 = the routine draws a straight line, else the routine's arc
    unsigned char Pad01[3];  // +0x01
    void* Texture;           // +0x04 ITextureLibrary2 record (a shape's FTextureRef::Texture); null
                             //       = an untextured line strip
    float Start[3];          // +0x08
    float End[3];            // +0x14 the straight line's end; the arc ends at Control + (End -
                             //       Control) rescaled to |Start - Control|
    float Control[3];        // +0x20 the arc's centre (read only when bCurved)
    float EyePos[3];         // +0x2c the camera position the textured quads turn towards
    float Color[3];          // +0x38 Win3DColor3f
    float Width;             // +0x44 segment length, and the arc's chord for the step angle
    float JitterScale;       // +0x48 RandomVector radius = JitterScale * Width
    float SideScale;         // +0x4c straight line only: the quad's side-vector scale
};
static_assert(sizeof(FWin3DFuzzyLine) == 0x50, "module state argument is 0x50 bytes");

// (cdecl Line). A jittered line from Start to End in segments of about Width: a
// line strip when Texture is null, otherwise one camera-facing textured quad per segment
// (additive blend 2/2). Hands the whole record to Win3DDrawFuzzyArc when bCurved is set.
void Win3DDrawFuzzyLine(FWin3DFuzzyLine const* Line);

// (cdecl Line). The curved sibling: the arc about Control from Start through the
// angle to End, stepped by quaternion rotations of (Start - Control), jittered the same way.
void Win3DDrawFuzzyArc(FWin3DFuzzyLine const* Line);
