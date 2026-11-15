// Author: Giperion.

#pragma once

// Base in-flight camera. FSpaceCamera implements the common 20-method ICamera prefix;
// FGameCamera and the concrete camera modes derive from it.

#include "common_misc.h"

struct FSpaceCamera;

// Slot ids, in bytes, exactly as baseline indexes them.
// Most leaves are __stdcall (this on stack). Exceptions matching ICamera / listing:
//   +0x48 GetVelocity — __thiscall, one stack arg (the branch RET 0x4)
//   +0x4C SetProperty — __thiscall, two stack args (the routine RET 0x8)
enum ESpaceCameraSlot
{
    kCamSlotQuery0            = 0x00,  // (*out = 0; return -1)
    kCamSlotIsA               = 0x04,  // (return 1)
    kCamSlotIsA2              = 0x08,  // (same function, second slot)
    kCamSlotGetPosition       = 0x0c,  // (this + 0x28)
    kCamSlotGetOrientation    = 0x10,  // (this + 0x04)
    kCamSlotGetViewport       = 0x14,  // (this + 0xac -- the ViewRect)
    kCamSlotGetHalfFovXDeg    = 0x18,  // (float this + 0x98)
    kCamSlotGetFovYDeg        = 0x1c,  // (float this + 0x9c)
    kCamSlotGetZNear          = 0x20,  // (float this + 0xa4)
    kCamSlotGetZFar           = 0x24,  // (float this + 0xa8)
    kCamSlotGetAspect         = 0x28,  // (float this + 0xa0)
    kCamSlotGetXScaleOverNear = 0x2c,  // (this+0x78 / this+0xa4)
    kCamSlotGetYScaleOverNear = 0x30,  // (this+0x7c / this+0xa4)
    kCamSlotProject           = 0x34,  // (this, &x, &y, &z, worldPoint)
    kCamSlotUnproject         = 0x38,
    kCamSlotClassifySphere    = 0x3c,  // 1 = out, 3 = straddling, 4 = in
    kCamSlotGetViewTransform  = 0x40,  // (lazily rebuilds this + 0x38)
    kCamSlotProjectVec        = 0x44,  // (this, outVec, worldPoint)
    kCamSlotGetVelocity       = 0x48,  // (writes a zero vector)
    kCamSlotSetProperty       = 0x4c   // "fovx" / "fovy" / "znear" / "zfar"
};

// baseline object layout, byte for byte with the routine. Everything is a raw float array so
// this header stays independent of Common's Vector/Matrix (and of whatever is being changed
// there); the offsets are what matters for a faithful port.
struct FSpaceCamera : ICamera
{
    GENRESULT NEURONCOMPONENTS_API QueryInterface(char const* Iid, void** Out) override;
    unsigned long NEURONCOMPONENTS_API AddRef() override;
    unsigned long NEURONCOMPONENTS_API Release() override;
    float const* NEURONCOMPONENTS_API GetPosition() override;
    float const* NEURONCOMPONENTS_API GetOrientation() override;
    int const* NEURONCOMPONENTS_API GetViewport() override;
    float NEURONCOMPONENTS_API GetHalfFovXDeg() override;
    float NEURONCOMPONENTS_API GetFovYDeg() override;
    float NEURONCOMPONENTS_API GetZNear() override;
    float NEURONCOMPONENTS_API GetZFar() override;
    float NEURONCOMPONENTS_API GetAspect() override;
    double NEURONCOMPONENTS_API GetXScaleOverNear() override;
    double NEURONCOMPONENTS_API GetYScaleOverNear() override;
    int NEURONCOMPONENTS_API Project(float* OutX, float* OutY, float* OutZ, float const* WorldPoint) override;
    void NEURONCOMPONENTS_API Unproject(float* OutWorld3, float ScreenX, float ScreenY) override;
    int NEURONCOMPONENTS_API ClassifySphere(float const* Center, float Radius) override;
    float const* NEURONCOMPONENTS_API GetViewTransform() override;
    int NEURONCOMPONENTS_API ProjectVec(float* Out3, float const* WorldPoint) override;
    void GetVelocity(float* Out3) override;
    int SetProperty(char const* Key, char const* Value) override;

    float  World[9];            // +0x004  orientation, row-major; ctor writes identity
    float  WorldPos[3];         // +0x028
    unsigned char bViewDirty;   // +0x034  ctor sets 1; slot 0x40 clears it after rebuilding
    unsigned char Pad035[3];
    float  View[9];             // +0x038  cached transpose(World)
    float  ViewPos[3];          // +0x05c  cached -(World * WorldPos)
    float  ScreenCenterX;       // +0x068  NOT initialised by the base ctor
    float  ScreenCenterY;       // +0x06c  NOT initialised by the base ctor
    float  HalfWidth;           // +0x070  320.0f
    float  HalfHeight;          // +0x074  240.0f
    float  XScale;              // +0x078  HalfWidth / tan(HalfFovX)
    float  YScale;              // +0x07c  -(HalfHeight / (0.75 * tan(HalfFovX)))
    float  PlaneX[3];           // +0x080  normalise(1, 0, tan(HalfFovX))
    float  PlaneY[3];           // +0x08c  normalise(0, 1, 0.75 * tan(HalfFovX))
    float  HalfFovXDeg;         // +0x098  35.0f
    float  FovYDeg;             // +0x09c  atan(0.75 * tan(HalfFovX)) in degrees
    float  Aspect;              // +0x0a0  1.3333334f
    float  ZNear;               // +0x0a4  3.0f      (the field)
    float  ZFar;                // +0x0a8  250000.0f (the field)
    // +0x0ac  the viewport, as Common's ViewRect: left, top, right, bottom in pixels. Slot 0x14
    // hands the whole thing out and the base ctor leaves it alone -- it is the routine (and its
    // two-argument twin the routine) that fills it in, deriving the aspect, the half-extents,
    // the screen centre and both frustum planes from it. baseline stores four INTS here, which is
    // why 0x0040fd87 reads them back with FILD and not FLD.
    int    Viewport[4];
    unsigned char FlagBC;       // +0x0bc  ctor sets 0
    unsigned char Pad0BD[3];
};

// The base ctor's highest write is the byte at +0xbc, and the third-person camera derived from
// it puts its own first field at +0xc0 (the routine reads the watched object from there), so
// the base subobject is exactly 0xc0 bytes.
static_assert(sizeof(FSpaceCamera) == 0xc0, "FSpaceCamera must be 0xc0");

inline ICamera* AsCamera(FSpaceCamera* Cam) { return Cam; }

// explicit field initialization; the C++ object already owns its vptr.
void SpaceCameraConstruct(FSpaceCamera* Cam);

// a NON-virtual method, not a vtable slot. Takes the HALF horizontal angle in
// degrees, stores it at +0x98, derives the vertical half-angle at +0x9c through the live aspect
// and rebuilds both frustum planes and both screen scales. The ctor and the "fovx" branch of
// are inlined copies of exactly this; the GameCamera ctor (0x0051b2f0) calls it.
void SpaceCameraSetHalfFovX(FSpaceCamera* Cam, float HalfFovXDeg);

// the vertical twin of the one above (thiscall, one stack float, RET 4): stores
// FovYDeg at +0x9c, derives the horizontal half-angle at +0x98 through the live aspect, then
// rebuilds both frustum planes and both screen scales. The "fovy" branch of the routine is an
// inlined copy; ShipViewControl's draw (0x00584509) calls it.
void SpaceCameraSetFovY(FSpaceCamera* Cam, float FovYDeg);

// the base SetProperty ("fovx" / "fovy" / "znear" / "zfar"; nonzero when the key
// was one of them), called DIRECTLY, not through slot 0x4c: the GameCamera override
// (0x0051b3fe) calls the base body this way. Through the vtable it is ICamera::SetProperty.
int SpaceCameraSetProperty(FSpaceCamera* Cam, const char* Key, const char* Value);

#ifdef OUTPOST_FRONTIER_WIDESCREEN_SUPPORT
// Port-only (widescreen.h): the Hor+ factor for this camera's stored viewport. Under the define
// +0x98 is the horizontal half-angle on a 4:3 surface; the planes, XScale and +0x9c use
// tan(+0x98) times this factor.
double SpaceCameraHorPlusScale(const FSpaceCamera* Cam);
#endif

// the other non-virtual rebuild, and the mirror image of the one above: the
// HALF-FOV is held fixed and the VIEWPORT changes, so the aspect comes out of the rectangle
// rather than out of the ini. Stores the four edges at +0xac, derives the aspect, both
// half-extents, the screen centre and the vertical half-angle, then rebuilds the two frustum
// planes and both screen scales.

// ViewRect4 is Common's ViewRect: left, top, right, bottom, INCLUSIVE -- baseline's extents are
// `right - left + 1` and `bottom - top + 1` (0x0040fd91 and 0x0040fdac both INC the difference).

// Always returns true. That is not an interpretation: 0x0040ff29 loads AL with 1 outright and
// the only thing between there and the RET is an FPU compare that cannot touch AL. baseline's
// callers still branch on it -- FGameCamera's activate slot does -- so the port keeps the
// return value rather than quietly making it void.
bool SpaceCameraSetViewport(FSpaceCamera* Cam, const int* ViewRect4);

// byte for byte the same body as the routine with two differences: it takes a
// second argument and it returns nothing. The second argument is dead. baseline never reads it:
// the only stack slots the body touches are the ViewRect pointer and three locals, and the one
// write that lands on an argument slot (0x0040fbb2) is the compiler reusing the FIRST
// argument's slot as scratch. Both overloads survived into the shipped build, so both are here.
void SpaceCameraSetViewportEx(FSpaceCamera* Cam, const int* ViewRect4, int UnusedFlag);

// The eight camera slots the routine selects between, at their baseline offsets inside the
// in-flight controller (the `this` of the routine).
enum ESpaceCameraMode
{
    kSpaceCameraFirstPerson = 0,   // controller + 0x00c
    kSpaceCameraThirdPerson = 1,   // controller + 0x1b0
    kSpaceCameraTurret      = 2,   // controller + 0x3c8
    kSpaceCameraRearview    = 3,   // controller + 0x500
    kSpaceCameraDeath       = 4,   // controller + 0x60c
    kSpaceCameraScript      = 5,   // controller + 0x760
    kSpaceCameraStatic      = 6,   // controller + 0x9a4
    kSpaceCameraDocking     = 7,   // controller + 0xa9c
    kSpaceCameraCount       = 8
};

// the in-flight init opens the cockpit ini once and runs all four cockpit camera
// loaders over it, in the order FirstPerson, ThirdPerson, Turret, RearView. Returns false when
// any of them complained (a missing file, or an aspect that is not 4:3).
bool SpaceCameraLoadCockpit(const char* CockpitIniPath);

// . ModeName is one of "player", "first_person", "third_person", "turret",
// "rearview", "death", "script", "static", "docking"; anything else selects nothing and
// returns null, exactly as baseline does. Returns the selected camera.
FSpaceCamera* SwitchSpaceCameraMode(const char* ModeName, void* AttachTarget);

// The camera the routine last selected, or null before the first successful switch.
FSpaceCamera* GetActiveSpaceCamera();

// / the routine / the routine (cdecl, `MOV EAX,imm / RET`) -- the addresses of
// the first-person (0x00678ba4), third-person (0x00678d48) and turret (0x00678f60) cameras
// inside the in-flight controller. Caller: the tunnel particle's visibility slot 0x005134c0.
FSpaceCamera* GetFirstPersonSpaceCamera();
FSpaceCamera* GetThirdPersonSpaceCamera();
FSpaceCamera* GetTurretSpaceCamera();

// (cdecl, no arguments) -- `if (the field) the field->+0xbc = 1`: the active
// camera's FlagBC raised, the same flag a hard cut raises (the routine). Callers: the jump-in
// (0x0054368f), the asteroid and nebula detail options (0x005267fb, 0x00509b83).
void RaiseActiveSpaceCameraCut();

// (SpaceApp + 0x08), the "controller exists" byte the routine (SpaceSendCommand)
// tests first. Writes: 1 by the space init (0x0054ab8e), 0 by the space teardown (0x0054af8d).
bool IsSpaceCameraControllerReady();

// MOV AL,[the field]: in-flight controller 0x00678b98 + 0x0b, raised at the
// head of the space init (0x0054ab68) and dropped on each of its exits (0x0054abf2, 0x0054ac09,
// ). The HUD draws test it first (HUD_Core 0x004d56d9, 0x004d8698, HUD_Status
// , 0x004e1bb3) and skip the frame while the space init is running.
bool IsSpaceInitInProgress();

// is the camera the routine hands out (the port's GetClientSoundListenerObject)
// the first-person camera, i.e. .data 0x00678ba4 = in-flight controller 0x00678b98 + 0x0c?
bool IsFirstPersonCameraCurrent();

// the same test against the docking camera (.data 0x00679634). Caller: the Ship's
// land request (0x00543c11).
bool IsDockingCameraCurrent();

// The three-call tail the script and static cameras run when they take the view and when they
// give it back (0x0051e615 / 0x0051e6f1 and 0x0051e93f / 0x0051e985). All three are identical
// between the two cameras, so they live here rather than being written out twice:

//   1. take the player's IObjRW (the routine: the watcher at .data 0x00679744, minus eight);
//   2. its IObjInspect slot 8 (+0x20) get_id;
//   3. IServer (the field) slot 57 (+0xe4) SPRequestInvincibility(id, bActive, 5,
//      the connection id at .data 0x00673344) -- the ship is invincible while the view is
//      cinematic.

// Does nothing when there is no player object, which is baseline's only guard (the server
// pointer is not tested).
void NotifyCinematicView(bool bActive);

// the guarded front door to the switch above, and what the cameras themselves
// call when they want to hand the view back. baseline's guard is the byte at .data 0x00678ba0,
// which is SpaceApp + 0x08 (raised by the space init, dropped by the teardown).
void RequestSpaceCameraMode(const char* ModeName);

// the ship's docking code hands the docking camera its fixed Csys (twelve
// floats: orientation then position), raises its view-dirty byte and, when the controller
// exists, switches to "docking". Callers are in Ship (0x00543cbf / 0x00543ec9).
void SetDockingCameraTransform(const float* Csys12);

// arm the docking fade countdown (the routine) on the docking camera;
// FDUMPs an ERROR (space.cpp:0x8ad) when that camera is not the active one, then arms it
// regardless. Caller: 0x00543cce.
void BeginDockingCameraSequence(float Seconds);

// point the script camera (the routine) at Object under ScriptName from
// Transform12 (nine orientation floats then three of position; null keeps the stored one), switch
// to "script" when the controller exists, and raise the latch below. Caller: the IClient
// REQUESTSPACESCRIPT handler the branch (0x0053d9ff / 0x0053da7f).
void SetupSpaceScriptCamera(void* Object, const float* Transform12, const char* ScriptName);

// the field = SpaceApp + 0xba8: set to 1 by the routine, cleared by the
// space frame (0x0054b1fa), the teardown (0x0054af2e) and the ctor. Reader: FCommChannel::
// TickVoice (0x004cbc8b).
bool GetSpaceScriptCameraLatch();

// .data 0x0061402c = 0.95f, read at exactly one instruction in the whole PE (0x00549deb).
// It is a cosine: the view direction may rotate by up to ~18.2 degrees across a camera switch
// before the switch counts as a cut.
extern const float kSpaceCameraCutCosine;

// 's closing test (0x00549da8 .. 0x00549e18). Both orientations are nine floats,
// row-major; the axis compared is COLUMN 2 -- elements 2, 5 and 8 -- which is why baseline reads
// them at a stride of 0x0c starting eight bytes into each matrix.

// The comparison is FCOMP + TEST AH,5 + JNP, which fires only on a strict, ordered "less than":
// a NaN anywhere leaves the dot unordered and does NOT count as a cut on its own. Having had no
// previous camera does count, unconditionally.
bool SpaceCameraIsHardCut(const float* OldOrientation, const float* NewOrientation,
                          bool bHadNoPreviousCamera);

// (RET 8, returns nothing): deactivate the outgoing camera (slot 0x68) and drop the
// sound listener; for a new camera, attach it to the player (slot 0x58), activate it with the
// window's client rect or the surface size (slot 0x64), take the fog colour at its position as
// its clear colour and clear (the routine); make it active and the sound listener
// (the routine); raise its +0xbc when the view jumps. Passing null deactivates. bForce
// reinstalls even when the camera is already the active one.
void InstallSpaceCamera(FSpaceCamera* NewCam, bool bForce);

// 0x0054aef4..0x0054af18 -- the space teardown's camera block: the routine on the
// first-person camera (controller + 0x00c, releases its cockpit mesh), then the non-virtual
// IDPMsgHandler::OnDisconnect (0x00401e90, a bare RET) on the third-person (+0x1b0), turret
// (+0x3c8) and rear-view (+0x500) cameras of G_SpaceApp.
void ReleaseSpaceCameraControllerParts();

// baseline keeps the player's first/third-person preference in controller + 0xba4 and reuses it
// whenever the mode is spelled "player".
int GetSpaceCameraPlayerPreference();
// The space init's write to controller + 0xba4 (0x0054ae06, with 0 under session flag 8).
void SetSpaceCameraPlayerPreference(int Preference);

// 's local_28: true when the last selected mode was "script".
bool WasLastSpaceCameraModeScript();

// raised by every SwitchSpaceCameraMode call, even the ones that select nothing.
extern unsigned char G_SpaceCameraSwitched;
