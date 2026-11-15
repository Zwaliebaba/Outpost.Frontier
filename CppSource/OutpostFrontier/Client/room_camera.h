// Author: Giperion.

#pragma once

// The room camera: the ICamera every RoomObject owns at +0x500 (the routine), and the one the
// SceneScript camera pool (gf_script.h, the field) pushes the active script camera into.

// baseline class tree, three vtables stamped in turn by the ctor the routine:
//   .rdata 0x005e1354  abstract base (the routine / dtor the routine): slots 0..4 and 16 are
//                      _purecall (the branch), slot 17 the branch its deleting dtor;
//   .rdata 0x005e1304  intermediate class: QI/AddRef/Release filled in, slots 3/4 still pure,
//                      slot 17 the routine;
//   .rdata 0x005e12a4  this class, 22 slots.
// The common ICamera prefix has 20 methods. baseline Room's tail differs after slot16;
// its position/orientation/transform operations use the named typed members below.
// This derived class's appended virtual methods do not reproduce the baseline tail indices.

#include "common_misc.h"

struct Rect;
struct ViewRect;

class FRoomCamera : public ICamera
{
public:
    // body after operator_new(0xbc): the three vtable stamps, RefCount = 1,
    // identity orientation, zero position, the routine identity view, then
    // (&Viewport) -- the ViewRect the ctor built with Rect_to_ViewRect first.
    explicit FRoomCamera(ViewRect const& Viewport);
    // only restamps the base vtable; no member teardown.
    virtual ~FRoomCamera();

    // Slots 0..2 -- the branch / the branch / the branch.
    GENRESULT NEURONCOMPONENTS_API QueryInterface(char const* Iid, void** Out) override;
    unsigned long NEURONCOMPONENTS_API AddRef() override;
    unsigned long NEURONCOMPONENTS_API Release() override;

    // Slots 3..16.
    float const* NEURONCOMPONENTS_API GetPosition() override;          // this + 0x80
    float const* NEURONCOMPONENTS_API GetOrientation() override;       // this + 0x5c
    int const* NEURONCOMPONENTS_API GetViewport() override;            // this + 0x04
    float NEURONCOMPONENTS_API GetHalfFovXDeg() override;              // +0x14
    float NEURONCOMPONENTS_API GetFovYDeg() override;                  // +0x18
    float NEURONCOMPONENTS_API GetZNear() override;                    // +0x1c
    float NEURONCOMPONENTS_API GetZFar() override;                     // +0x20
    float NEURONCOMPONENTS_API GetAspect() override;                   // +0x2c
    double NEURONCOMPONENTS_API GetXScaleOverNear() override;          // +0x38 / +0x1c
    double NEURONCOMPONENTS_API GetYScaleOverNear() override;          // +0x3c / +0x1c
    int NEURONCOMPONENTS_API Project(float* OutX, float* OutY, float* OutZ,
                          float const* World) override;
    void NEURONCOMPONENTS_API Unproject(float* OutWorld3, float ScreenX,
                             float ScreenY) override;
    int NEURONCOMPONENTS_API ClassifySphere(float const* Center,
                                 float Radius) override;
    float const* NEURONCOMPONENTS_API GetViewTransform() override;

    // Complete the common prefix; the two legacy placeholders are local extensions.
    int NEURONCOMPONENTS_API ProjectVec(float* Out3, float const* World) override;
    void GetVelocity(float* Out3) override;
    int SetProperty(char const* Key, char const* Value) override;
    virtual void NEURONCOMPONENTS_API Slot20();
    virtual void NEURONCOMPONENTS_API Slot21();

    // baseline slot 19 (the routine): copy three floats to +0x80.
    void SetPosition(float const* Pos3);
    // baseline slot 20 (the routine): copy nine floats to +0x5c.
    void SetOrientation(float const* Orient9);
    // baseline slot 21 (the branch): copy twelve floats -- orientation then position -- to +0x5c.
    void SetTransform(float const* Transform12);

    // non-virtual init from a ViewRect (null selects 0,0,639,479).
    void InitFromViewRect(int const* ViewRect4);
    // rebuild the X side plane and XScale from TanHalfFovX.
    void RebuildPlaneX();
    // rebuild the Y side plane and YScale from TanHalfFovY.
    void RebuildPlaneY();

    int Viewport[4];             // +0x04  ViewRect: left, top, right, bottom
    float HalfFovXDeg;           // +0x14
    float HalfFovYDeg;           // +0x18
    float ZNear;                 // +0x1c
    float ZFar;                  // +0x20
    float TanHalfFovX;           // +0x24
    float TanHalfFovY;           // +0x28
    float Aspect;                // +0x2c
    float HalfWidth;             // +0x30
    float HalfHeight;            // +0x34
    float XScale;                // +0x38
    float YScale;                // +0x3c
    float PlaneX[3];             // +0x40
    float PlaneY[3];             // +0x4c
    unsigned long RefCount;      // +0x58
    float Orient[9];             // +0x5c  row-major; followed by Pos -- together an FTransform
    float Pos[3];                // +0x80
    float View[12];              // +0x8c  GetViewTransform's output
};

// cdecl (Transform12, Rect by value, HalfFovX, ZNear, ZFar). Transform12 is
// position (3) then orientation (9), as 556be0 lays it out on its stack.
FRoomCamera* RoomCameraConstruct(float const* PosOrient12, Rect const& Viewport,
                                 float HalfFovXDeg, float ZNear, float ZFar);

// origin, identity, Rect {0, 0, the field (640), the field (480)},
// 35 / 0.25 / 1e6.
FRoomCamera* RoomCameraCreate();

// slot 17 with 1 (delete) on a non-null camera, bypassing the ref count.
void RoomCameraDestroy(FRoomCamera* Camera);

// / 556cc0 / 556cd0 -- slots 19 / 20 / 21.
void RoomCameraSetPosition(FRoomCamera* Camera, float const* Pos3);
void RoomCameraSetOrientation(FRoomCamera* Camera, float const* Orient9);
void RoomCameraSetTransform(FRoomCamera* Camera, float const* Transform12);

// set the horizontal half-angle; the aspect comes from the live viewport.
void RoomCameraSetHalfFovX(FRoomCamera* Camera, float HalfFovXDeg);
// Typed raw-word transport for SceneScript control callbacks.
void RoomCameraSetHalfFovXValue(FRoomCamera* Camera, const float* HalfFovXDeg);
// store ZNear, then rebuild both planes from the stored half-angles.
void RoomCameraSetZNear(FRoomCamera* Camera, float ZNear);
// Typed raw-word transport for SceneScript control callbacks.
void RoomCameraSetZNearValue(FRoomCamera* Camera, const float* ZNear);
// store ZFar.
void RoomCameraSetZFar(FRoomCamera* Camera, float ZFar);
// Typed raw-word transport for SceneScript control callbacks.
void RoomCameraSetZFarValue(FRoomCamera* Camera, const float* ZFar);

// (thiscall on a float[3]) -- scale by 1 / |v|, no zero test.
void RoomVec3Normalize(float* V);

// the lazily acquired 3DMathEngine this TU (0x00556xxx) shares: every use
// tests it and fills it from the routine when null -- FRoomCamera::Project the branch
// (0x005568c1), FRoomCamera::GetViewTransform the branch (0x00556aa0) and the light factory
// (0x00557094). A raw scan finds no other reference; nothing releases it.
struct I3DMathEngine;
extern I3DMathEngine* G_LightsCameraMath;
I3DMathEngine* LightsCameraMath();
