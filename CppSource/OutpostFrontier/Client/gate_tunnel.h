// Author: Giperion.

#pragma once

// baseline Client\Space\GateTunnel.cpp (path string .rdata 0x005da560) -- the jump tunnel a
// player ship flies through, loaded from the [Data] gate_tunnels INI and driven by the
// JumpShipEffect (Ship +0x188) that finds it by its jump_tunnel id.

// Class GateTunnel: operator new(0x2b8), constructor the routine, vtable 0x005da71c
//   slot 0 the routine  scalar deleting destructor (body the routine)
//   slot 1 the routine  EnterState0 -- reset the clocks and the ship
//   slot 2 the branch  EnterState1 -- rebuild every curve, create the tunnel light
//   slot 3 the branch  EnterState2 -- restart the speed clock
//   slot 4 the routine  EnterState3 -- release the tunnel light
// and its members SetState the routine, SetShip the routine, Update the routine, Draw
// (+ BuildFrame the routine, EmitLayer the routine), RebuildPath the routine,
// RebuildColorOffsets the routine. The nine 0x28-byte spline curves are FGateTunnelCurve
// (the routine / the routine / the routine / the routine / the routine, the solver
// and the cubic the routine). The list the field is filled by GateTunnelIniLoad
// (the routine, 0x0054ac92) and emptied by GateTunnelsClear (the routine, 0x0054afbb).

// Layout note: baseline's curves hold VC6 std::vectors (allocator byte, first, last, end); the
// port holds ours, so the class is not byte-compatible with baseline past +0xc8 and nothing may
// address it by raw offset. Offsets below are baseline's, for tracing.

#include "common_csys.h"
#include "common_id.h"
#include "texture_shapes.h" // FTextureRef
#include "x86math.h"

#include <vector>

class FClientShipGameobj; // client_gameobj.h
struct FGameLight;        // engine_light.h
struct FSpaceCamera;      // space_camera.h

// One cubic segment of a curve: value = ((c3 t + c2) t + c1) t + c0 (0x10 bytes).
struct FGateTunnelCoeff
{
    float C0; // +0x00
    float C1; // +0x04
    float C2; // +0x08
    float C3; // +0x0c
};

// (thiscall on the segment, RET 4) -- the cubic at T, left on the FPU.
double GateTunnelCoeffEval(FGateTunnelCoeff const* Coeff, float T);

// (cdecl) -- the natural-end cubic spline through Y[0..N] (tridiagonal solve for
// the tangents D, then Out[i] = {Y[i], D[i], 3(Y[i+1]-Y[i]) - 2D[i] - D[i+1],
// 2(Y[i]-Y[i+1]) + D[i] + D[i+1]}). Out is emptied and resized to N segments.
void GateTunnelSolveSpline(float const* Y, int N, std::vector<FGateTunnelCoeff>* Out);

// A random spline (0x28 bytes in baseline): the control values, their segments and the range new
// control values are drawn from.
struct FGateTunnelCurve
{
    // + the ctor's `[+0x20] = 0, [+0x24] = 1.0f`.
    FGateTunnelCurve();

    // (thiscall, RET 4) -- the curve at T in [0, 1]: T == 1 gives the last control
    // value, anything else the segment floor(Count * T) at the fraction. Left on the FPU.
    double Sample(float T) const;

    // (thiscall, RET 0x14) -- Count control values drawn uniformly from
    // [Minimum, Maximum] (which become the curve's range), the first ZeroHead and the last
    // ZeroTail forced to 0, then the spline.
    void Randomize(int Count, float Minimum, float Maximum, int ZeroHead, int ZeroTail);

    // (thiscall, RET 4) -- with Shift >= 8 and at least 8 control values: the
    // last Shift values move to the front, the rest are redrawn from the range, the spline is
    // rebuilt, and -((Count - Shift) / (Count - 1)) (integer division) comes back; otherwise 0.
    double Scroll(int Shift);

    // (thiscall, RET 0xc) -- Count control values read from Source with a Stride
    // of floats, then the spline.
    void SetFromSamples(int Count, float const* Source, int Stride);

    std::vector<float> Samples;              // +0x00 (VC6: first +0x04, last +0x08)
    std::vector<FGateTunnelCoeff> Coeffs;    // +0x10 (VC6: first +0x14, last +0x18)
    float Minimum;                           // +0x20
    float Maximum;                           // +0x24
};

// One render layer (0x34 bytes, two at +0xc). Constructor the branch and destructor
// are FTextureRef's; the rest is set by the loader's [layer] section.
struct FGateTunnelLayer
{
    FTextureRef Texture;  // +0x00 texture (a G_EffectShapeDB shape's record)
    FVector Color;        // +0x08 color (defaults 0.5)
    float NearAlpha;      // +0x14 near_alpha (1.0)
    float FarAlpha;       // +0x18 far_alpha (0.0)
    float RadiusFactor;   // +0x1c radius_factor (1.0)
    float UOffset;        // +0x20 u_offset (0.0), scrolled by Du
    float VOffset;        // +0x24 v_offset (0.0), scrolled by Dv
    float Du;             // +0x28 du (0.09)
    float Dv;             // +0x2c dv (0.07)
    float VScale;         // +0x30 v_scale (10.0)

    FGateTunnelLayer();
};

// A struct (not a class) because jump_effect.h forward-declares it as one.
struct FGateTunnel
{
    // .
    FGateTunnel();
    // slot 0: the routine / the routine.
    virtual ~FGateTunnel();
    // slots 1..4, reached through SetState.
    virtual void EnterState0();
    virtual void EnterState1();
    virtual void EnterState2();
    virtual void EnterState3();

    FGateTunnel(FGateTunnel const&) = delete;
    FGateTunnel& operator=(FGateTunnel const&) = delete;

    // (thiscall, RET 4): State = NewState, then slot 1 + NewState for 0..3.
    // Callers: the JumpShipEffect methods 0x00503eb4 / 0x00504221 / 0x00504481 / 0x00504652.
    void SetState(int NewState);
    // (thiscall, RET 4). Caller: the routine (0x00503cf8).
    void Update(float Dt);
    // (thiscall, RET 4). Caller: the routine, the JumpShipEffect's tunnel draw.
    void Draw(FSpaceCamera* Camera);
    // (thiscall, RET 8): the path frame at T -- columns (left, up, forward), the
    // point on the path as translation.
    void BuildFrame(FTransform* Out, float T) const;
    // (thiscall, RET 0x10): one layer's ring strips from T0 to T1.
    void EmitLayer(float T0, float T1, FGateTunnelLayer* Layer, int LayerIndex);
    // (thiscall, RET 4).
    void RebuildPath(int Shift);
    // (thiscall, RET 4).
    void RebuildColorOffsets(int Shift);

    ID_String Id;                  // +0x004 nickname ("default")
    int LayerCount;                // +0x008
    FGateTunnelLayer Layers[2];    // +0x00c
    unsigned char bWriteDepthBuffer; // +0x074 write_depth_buffer (1)
    int NumSplineControlPoints;    // +0x078 num_spline_control_points (11)
    float XRange;                  // +0x07c x_range (0)
    float YRange;                  // +0x080 y_range (0)
    float ZRange;                  // +0x084 z_range (0)
    float MinRadius;               // +0x088 min_radius (50)
    float MaxRadius;               // +0x08c max_radius (50)
    float FarRadiusFactor;         // +0x090 far_radius_factor (1.0)
    float MinSpeed;                // +0x094 min_speed (3e-5)
    float MaxSpeed;                // +0x098 max_speed (0.06)
    float TimeToMaxSpeed;          // +0x09c time_to_max_speed (8)
    float NumTSteps;               // +0x0a0 num_t_steps (200); Draw negates it around a layer
    float NumSSteps;               // +0x0a4 num_s_steps (12)
    float MinRotation;             // +0x0a8 min_rotation (-pi)
    float MaxRotation;             // +0x0ac max_rotation (pi)
    FVector MinRgb;                // +0x0b0 min_rgb (0)
    FVector MaxRgb;                // +0x0bc max_rgb (0)
    FGateTunnelCurve PathX;        // +0x0c8
    FGateTunnelCurve PathY;        // +0x0f0
    FGateTunnelCurve PathZ;        // +0x118
    FGateTunnelCurve Radius;       // +0x140 [min_radius, max_radius]
    FGateTunnelCurve Rotation;     // +0x168 [min_rotation, max_rotation]; no reader traced
    FGateTunnelCurve Brightness;   // +0x190 [0.5, 1.0], scales the layer colour
    FGateTunnelCurve ColorOffsetR; // +0x1b8 [min_rgb.x, max_rgb.x]
    FGateTunnelCurve ColorOffsetG; // +0x1e0 [min_rgb.y, max_rgb.y]
    FGateTunnelCurve ColorOffsetB; // +0x208 [min_rgb.z, max_rgb.z]; EmitLayer reads R for B
    Transform Frame;               // +0x230 the path's world frame (the routine sets it)
    FClientShipGameobj* Ship;      // +0x260 the routine
    float SpeedClock;              // +0x264
    unsigned Field_0x268;          // +0x268 TODO: no reader or writer found yet
    int SegmentIndex;              // +0x26c
    float Phase;                   // +0x270 the path position (0..1) the ship is at
    Vector ShipPosition;           // +0x274
    Matrix ShipOrientation;        // +0x280
    int State;                     // +0x2a4
    FGameLight* Light;             // +0x2a8
    Vector LightDirection;         // +0x2ac
};

// (allocator byte; first the field, last the field, end the field) -- the
// loaded tunnels, a VC6 vector of pointers built by the static initializer the branch
// (the routine). Its atexit destructor (the branch -> the routine) owns the elements: every
// non-null tunnel still listed is deleted through slot 0 before the storage is freed.
struct FGateTunnelList : std::vector<FGateTunnel*>
{
    FGateTunnelList() = default;
    FGateTunnelList(FGateTunnelList const&) = delete;
    FGateTunnelList& operator=(FGateTunnelList const&) = delete;
    // .
    ~FGateTunnelList();
};
extern FGateTunnelList G_GateTunnels;

// (cdecl) -- the tunnel whose +4 id equals *Id, else FDUMP 0x100002 (line 0xfc)
// and null. Caller: the routine (0x0050377f).
FGateTunnel* GateTunnelFind(ID_String const* Id);

// (thiscall, RET 4) -- +0x260 = Ship. Caller: the routine (0x005037c0).
void GateTunnelSetShip(FGateTunnel* Tunnel, FClientShipGameobj* Ship);

// (cdecl, the resolved path) -- [gate_tunnel] / [layer] sections into
// G_GateTunnels. Caller: the space init at 0x0054ac92.
void GateTunnelIniLoad(char const* Path);

// (cdecl) -- every tunnel deleted through its slot 0, then end = begin. Caller:
// the space teardown at 0x0054afbb.
void GateTunnelsClear();
