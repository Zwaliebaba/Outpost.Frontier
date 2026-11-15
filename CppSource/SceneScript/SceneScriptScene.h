// Author: Giperion.

#pragma once

// IScriptEngine scene: Lua foreach → C++ entities/events + playback clock.
// Layout: implementation notes and scenescript_thn_format.md.

#include "SceneScript.h"
#include "scenescript_controls.h"
#include <list>
#include <map>
#include <memory>
#include <string>
#include <optional>
#include <set>
#include <vector>

static const int kSceneScriptPlaybackLoaded = 2;
static const int kSceneScriptPlaybackReset = 3;
static const int kSceneScriptPlaybackDone = 4;
static const int kSceneScriptPlaybackPaused = 5;
static const int kSceneScriptPlaybackPlay = 6;
static const int kSceneScriptPlaybackLoop = 7;

// The entity / event type constants, FSceneScriptLightProps, FSceneScriptMotionPathDesc and
// FSceneScriptEntitySnap are public ABI now: src/include/scenescript_iface.h (through SceneScript.h).

static const int kSceneScriptTargetRoot = 0;
static const int kSceneScriptTargetHardpoint = 1;
static const int kSceneScriptTargetPart = 2;

static const int kSceneScriptFlagUseScriptDuration = 1;
static const int kSceneScriptFlagPosition = 2;
static const int kSceneScriptFlagOrientation = 4;
static const int kSceneScriptFlagLookAt = 8;
static const int kSceneScriptFlagQuatOrient = 0x10;
static const int kSceneScriptFlagEntityRelative = 0x20;
static const int kSceneScriptFlagParentChild = 0x40;
static const int kSceneScriptFlagOrientRelative = 0x80;

static const int kSceneScriptSpatialBitPos = 0x1;
static const int kSceneScriptSpatialBitOrient = 0x2;
static const int kSceneScriptSpatialBitAxisRot = 0x4;

// Type 5 load the field: fovh/aspect/near/far → bits 1/2/4/8 (not GET-order names).
static const int kSceneScriptCamBitFovh = 0x1;
static const int kSceneScriptCamBitAspect = 0x2;
static const int kSceneScriptCamBitNear = 0x4;
static const int kSceneScriptCamBitFar = 0x8;

static const int kSceneScriptLightBitColor = 0x1;
static const int kSceneScriptLightBitDirection = 0x2;
static const int kSceneScriptLightBitRange = 0x4;
static const int kSceneScriptLightBitCutoff = 0x8;
static const int kSceneScriptLightBitOn = 0x10;
static const int kSceneScriptLightBitInfinite = 0x20;
static const int kSceneScriptLightBitAtten = 0x40;
static const int kSceneScriptLightBitTheta = 0x80;
static const int kSceneScriptLightBitType = 0x100;
static const int kSceneScriptLightBitSpecular = 0x200;
static const int kSceneScriptLightBitAmbient = 0x400;
static const int kSceneScriptLightBitDiffuse = 0x800;

// baseline Csys object: orient 3×3 @ +0, pos @ +0x24 (the routine / event+0x4).
struct FSceneScriptCsys
{
    float Orient[9];
    float Pos[3];
};

struct FSceneScriptUserPropLess
{
    bool operator()(const std::string& Left, const std::string& Right) const;
};

typedef std::map<std::string, std::string, FSceneScriptUserPropLess> FSceneScriptUserProps;

// Native replacement for the baseline START_IK constructor payload.  The baseline
// object also carried VC6 string/vector state around this data; this descriptor
// keeps only the observed game fields and deliberately has no baseline ABI.
struct FSceneScriptIkEventDesc
{
    int Desc[11];
    int CountToRoot;
    int LookAtUpAxis;
    int LookAtFrontAxis;
    float Damping;
    unsigned char bPointAt;
    unsigned char bMoveTo;
    unsigned char Pad[2];
    float TransitionDuration;
    const char* EndEffector;
    const char* TargetPart;
    int TargetType;
    int IkId;
};

class FSceneScriptEntity;

// Private typed circular owner. Node identity survives append/other-node erase;
// callbacks observe Next only after returning, as in 41490/414d0/41540.
struct FSceneScriptMonitorLinkNode
{
    FSceneScriptMonitorLinkNode* Next;
    FSceneScriptMonitorLinkNode* Previous;
    FSceneScriptEntity* Entity;
};

class FSceneScriptMonitorLinks
{
private:
    FSceneScriptMonitorLinkNode InactiveSentinel;
    std::unique_ptr<FSceneScriptMonitorLinkNode> OwnedSentinel;

public:
    FSceneScriptMonitorLinks();
    ~FSceneScriptMonitorLinks();
    void Initialize();
    void Reset();
    void Erase(FSceneScriptMonitorLinkNode* Node);
    void Clear();
    FSceneScriptMonitorLinks(const FSceneScriptMonitorLinks&) = delete;
    FSceneScriptMonitorLinks& operator=(const FSceneScriptMonitorLinks&) = delete;

    FSceneScriptMonitorLinkNode* Sentinel;
    unsigned int Count;
};

// C++ inner entity (not NEURONCOMPONENTS). baseline: operator_new per-type + vtable
// / 06f5b084 / … . Slot 3 +0x0C / slot 4 +0x10 = thiscall RET 4
// (the branch / the routine / … → the routine / the routine).
class FSceneScriptEntity
{
public:
    FSceneScriptEntity();
    FSceneScriptEntity(int Type, const FSceneScriptEntitySnap* Desc, FSceneScriptScene* OwnerScene);
    virtual ~FSceneScriptEntity() noexcept(false);
    void ReleaseCommonEntityState();
    char* AssignEntityName(const char* Source);
    char* AssignEntityTemplate(const char* Source);
    virtual void InnerUnused1();
    virtual void InnerApplyDesc(const FSceneScriptEntitySnap* SnapIn);
    float* InnerGetPos();
    float* InnerGetOrient();
    virtual void InnerSetPos(const float* Pos);
    virtual void InnerSetOrient(const float* Mat9);

    // refresh the live audio payload, then copy all
    // eight words to the caller's output buffer.
    void CaptureAudioProps(float* Out8);

    // initializes SOUND ain/aout to 0x168 before the
    // descriptor payload is applied.  Keep that semantic default in the
    // native audio owner; it is not a baseline object-layout contract.
    void InitializeSoundDefaults();

    // releases the specialized light controls before
    // common entity cleanup.  Keep that ownership operation on the entity;
    // the scene chooses the remaining controls by bound entity type.
    void ReleaseLightControls();

    // Concrete derived controls precede common owned data and User/Geo teardown.
    void ReleaseDerivedControls();
    // Concrete entity virtual+0x24 is separate from deleting-destructor cleanup.
    void InnerUnbindControls();

    // Modern owner-side equivalent of baseline the routine. Scene unbind
    // intentionally leaves ReverbControl held until final destruction,
    // matching the baseline unbind body without reproducing its object layout.
    void ReleaseSceneBoundControls();

    // is the motion-path three-control teardown.
    void ReleaseMotionPathControls();

    // the MOTION_PATH constructor (the routine) calls
    // this once, right after ApplyDesc, with the entity's own
    // mutable Snap.MotionPath pair. It retains the descriptor's
    // path object a second time (+0xe8) and queries "1.11_IMotionPath" from
    // that copy into MotionPath (+0xd0).  UpdateEntityFromDesc never
    // re-enters this path; Attach later rebinds MotionPath from the engine
    // object.
    void BindMotionPathFromDescObject();

private:
    // String owners are distinct from the published descriptor pointers.
    // Construction's primitive-tail memset excludes all these RAII objects.
    std::optional<std::string> NameStorage;
    std::optional<std::string> TemplateStorage;
public:
    FSceneScriptMonitorLinks MonitorLinks;
    std::optional<FSceneScriptUserProps> UserProps;
    FSceneScriptEntitySnap Snap;
    // baseline entity +0x38 / +0x44 (the routine / the routine): the pose
    // as loaded by the last ApplyDesc, already converted into the parent
    // Csys when one exists.  InnerSetPos/InnerSetOrient and the slot-5 live
    // refresh update Snap only; IScriptEngine slots 10/11 (the branch /
    // ) and therefore the baseline serializer read these words.
    float LoadPos[3];
    float LoadOrient[9];
    void* EngineObj;
    FSceneScriptCsys* ParentCsys;
    int BindState;
    unsigned char bBound;
    // Type whose interface sequence is currently held by this aggregate.
    // Cleanup must use this value rather than a descriptor type that may
    // have been replaced after Attach and before Detach/Destroy.
    int BoundControlType;
    IGeoTransformControl* GeoControl;
    ICameraControl* CameraControl;
    ICompoundControl* CompoundControl;
    IIKControl* IkControl;
    ILightControl* LightControl;
    ID3DLightControl* D3dLightControl;
    ISoundControl* SoundControl;
    IUserEventControl* UserEventControl;
    ISubtitleControl* SubtitleControl;
    IFogControl* FogControl;
    IReverbControl* ReverbControl;
    IPSysControl* PsysControl;
    IMotionPath* MotionPath;
    // MOTION_PATH +e8 retains the construction-time path separately from
    // the mutable public owning descriptor reference (+cc); both release
    // in the routine before the sole PathType string and runtime +d0.
    // Parse-only temporary reference; actual MOTION_PATH owning +cc is
    // the mutable public Snap.MotionPath.MotionPath field itself.
    IPersistString* ParsedMotionPathDescObject;
    IPersistString* MotionPathBoundObject;
    // Typed construction identity differs from the Lua descriptor staging owner.
    bool bMotionPathDescriptorOwner;
    // Constructor unwind owns only the string, runtime interface and common base.
    bool bMotionPathConstructionComplete;
    IScriptControl* ScriptControl;


    // Concrete owner selected by the entity factory; descriptor Type can change on updates.
    int DescriptorOwnerType;
    // / 06f460f0: runtime +0xec..+0xf8, separate from Snap +0xd4..+0xe0.
    float CameraRuntimeProps[4];

    // Type-specific dump/load (the routine / the routine). Not in Common snapshot 0x54.
    float FloorHeight;
    int LightMask;
    int LightOn;
    int LightInfinite;
    int LightType;
    float LightColor[3];
    float LightDiffuse[3];
    float LightSpecular[3];
    float LightAmbient[3];
    float LightDirection[3];
    float LightRange;
    float LightCutoff;
    float LightTheta;
    float LightAtten[3];
    // clears the two light-entity runtime words at baseline +0x158/+0x15c.
    int LightRuntimeState[2];
    // baseline initializes light payload in the constructor, not on every
    // descriptor update.
    unsigned char bLightStateInitialized;
    // copies the 0x24-dword descriptor to baseline this+0x160.
    FSceneScriptLightProps LightEntityDescriptor;
    float Audio[8];
    int AudioMask;
    float PsysRuntimeSparam; // baseline +0xd0; retained input lives in Snap.PsysSparam.
    char* PathType;
    std::size_t PathTypeCapacity;
    const char* MotionPathBoundType; // Constructor cache corresponding to baseline +e4.
    char* PathData;
    int SceneUpAxis;
    int SceneFrontAxis;
    int SceneAmbient[3];
    int SceneFogMask;
    // Entity+0x11c: 15 dword blob. Fog 8-dword lives at packed[7] (= +0x138).
    unsigned int SceneFogPacked[15];
    int SceneStartTime;
    float SceneReverb[4];
    int SceneReverbMask;
    // mirrors packed words +0x130/+0x134 through these
    // optional caller-owned outputs.  Keep the pointers as native state;
    // their presence does not expose the baseline object layout or ABI.
    unsigned int* SceneFogOutputA;
    unsigned int* SceneFogOutputB;
};

class FSceneScriptScene;

// Private CONNECT string ownership; published descriptor pointers are separate.
// This is a current-CRT owner, without a VC6 basic_string representation.
struct FSceneScriptConnectString
{
    std::unique_ptr<char[]> Data;
    size_t Length = 0;
    size_t Capacity = 0;
    FSceneScriptConnectString() = default;
    FSceneScriptConnectString(const FSceneScriptConnectString&) = delete;
    FSceneScriptConnectString& operator=(const FSceneScriptConnectString&) = delete;
    bool Assign(const char* Source);
    char* GetPublishedData();
    void Reset();
};

// C++ event vtable (not NEURONCOMPONENTS). Slot 2 = GetDesc → this+0x28 (the branch).
// Member order matches the routine + desc copy @ +0x28.
class FSceneScriptEvent
{
public:
    FSceneScriptEvent();
    explicit FSceneScriptEvent(int DescriptorStorageType); // storage selection only; no descriptor callbacks
    FSceneScriptEvent(int Type, const void* Desc);
    virtual ~FSceneScriptEvent() noexcept(false);
    virtual void ApplyDesc(const void* Desc);
    void ApplyCommonDesc(const void* Desc);
    void FinalizeDesc();
    void ApplySetCameraDesc(const void* Desc);
    void ApplyCameraPropAnimDesc(const void* Desc);
    void ApplyStartSoundDesc(const void* Desc);
    void ApplyFloorHeightDesc(const void* Desc);
    void ApplyStartPsysDesc(const void* Desc);
    void ApplyAudioPropAnimDesc(const void* Desc);
    void ApplyPsysPropAnimDesc(const void* Desc);
    void ApplyUserEventDesc(const void* Desc);
    virtual void* GetDesc();
    virtual unsigned char Start(FSceneScriptScene* Scene, int TimeMs);
    void DispatchSubtitleControl(FSceneScriptScene* Scene, bool bShow);
    virtual unsigned char Stop(FSceneScriptScene* Scene);
    virtual unsigned char StillRunning(int TimeMs, FSceneScriptScene* Scene);

    // Modern owner-side equivalent of the deleting-destructor branch used by
    // the baseline event vtables.  Heap ownership is explicit at scene call
    // sites; no VC6 wrapper or cross-module delete ABI is exposed.
    static void DestroyNative(FSceneScriptEvent*& Event);

    // is the typed descriptor path for subtitle events.
    // It deliberately remains separate from the common descriptor operation
    // so the mismatch behavior for dword 0 and strid is observable.
    void ApplySubtitleDesc(const void* Desc);

    // is the event progress accessor used by the
    // type-18 vtable. Keep its arithmetic on the modern event owner; no
    // baseline vtable or aggregate layout crosses this boundary.
    float EvaluateProgress(int TimeMs) const;

    // REVERB never takes the period branch and
    // always rounds its ratio before either the plain curve call or return.
    float EvaluateProgressNoPeriodWrap(int TimeMs) const;
    void EvaluateProgressExtended(int TimeMs, FSceneScriptExtendedProgress* Result,
                                  bool bNoPeriodWrap) const;

    // baseline per-type event vtable slot 7 (base+0x1c): constant type-name
    // string getter ("MOTION", "IK", ... "SUBTITLE"). The port keeps one
    // Type-switched event class, so the 19 baseline getters collapse here.
    const char* TypeName() const;

    // resolve both event endpoints, connect camera
    // monitor links, copy camera properties, then copy the source live pose.
    unsigned char StartSetCamera(FSceneScriptScene* Scene);
    unsigned char StartCameraPropAnim(FSceneScriptScene* Scene);
    unsigned char StartFogPropAnim(FSceneScriptScene* Scene);
    unsigned char StartPsysPropAnim(FSceneScriptScene* Scene);
    unsigned char StartReverbPropAnim(FSceneScriptScene* Scene);
    unsigned char TickReverbPropAnim(int TimeMs, FSceneScriptScene* Scene);
    unsigned char StartAudioPropAnim(FSceneScriptScene* Scene);
    unsigned char TickAudioPropAnim(int TimeMs, FSceneScriptScene* Scene);
    unsigned char TickFogPropAnim(int TimeMs, FSceneScriptScene* Scene);
    unsigned char StartAttachEntity(FSceneScriptScene* Scene);
    unsigned char StartSpatialPropAnim(FSceneScriptScene* Scene);
    unsigned char TickCameraPropAnim(int TimeMs, FSceneScriptScene* Scene);
    unsigned char TickPsysPropAnim(int TimeMs, FSceneScriptScene* Scene);
    unsigned char TickLightPropAnim(int TimeMs, FSceneScriptScene* Scene);
    unsigned char TickSpatialPropAnim(int TimeMs, FSceneScriptScene* Scene);
    unsigned char TickAttachEntity(int TimeMs, FSceneScriptScene* Scene);
    unsigned char TickPathAnim(int TimeMs, FSceneScriptScene* Scene);
    unsigned char StartFloorHeightAnim(FSceneScriptScene* Scene);
    unsigned char TickFloorHeightAnim(int TimeMs, FSceneScriptScene* Scene);

    void ApplyIkExtendedDesc(const FSceneScriptIkEventDesc* Desc);
    void ApplyStartIkDesc(const void* Desc);
    void SetIkEndEffector(const char* Name);
    void SetIkTargetPart(const char* Name);

    // Modern owner-side equivalent of the routine.  Type 12 has one
    // descriptor word beyond the common eleven-word event record; keep that
    // payload typed instead of reproducing the baseline subclass layout.
    void ApplySubSceneDesc(const void* Desc, float StartTimeValue);

    // Modern owner-side equivalent of baseline the routine: type-8 (attach)
    // carries six extra descriptor words (TargetType, PointAt xyz,
    // LookAtUpAxis, LookAtFrontAxis) plus one ref-counted target-part string
    // beyond the common eleven-word record.  A mismatched input type resets
    // the six words to their construction-time defaults instead of copying.
    void ApplyAttachDesc(const void* Desc);
    void ApplyConnectDesc(const void* Desc);
    // two constructor setter calls, then descriptor event ID.
    void InitializeConnectDesc(const void* Desc);
    void SetConnectHardpoint(const char* Name);
    void SetConnectParentHardpoint(const char* Name);

    // Modern owner-side equivalent of baseline the routine: type-6 (path
    // animation) carries eight extra descriptor words (StartPercent,
    // StopPercent, PercentDelta, PathOffset xyz, LookAtUpAxis,
    // LookAtFrontAxis) beyond the common eleven-word record.  A mismatched
    // input type resets them to their construction-time defaults instead of
    // copying.
    void ApplyPathAnimDesc(const void* Desc);

    // Native owner for the baseline type-15 audio-property descriptor.  The
    // baseline subclass carried the same semantic state behind a VC6 layout;
    // this adapter keeps the payload and mask in the current event owner.

    // Portable owner-side equivalent of the baseline audio subclass cleanup.
    // baseline +0x4c is the optional descriptor curve, not a separate audio
    // ABI object; the current event owns it as ParamCurve.
    void ReleaseAudioOwnedState();

    // Portable owner-side equivalent of the camera subclass cleanup.  baseline
    // releases only the optional curve before common cleanup.
    void ReleaseCameraOwnedState();

    // Portable owner-side equivalent of the spatial-property subclass
    // cleanup.  baseline the routine releases the optional curve and its
    // ref-counted target-part string before common cleanup.
    void ReleaseSpatialOwnedState();

    // Portable owner-side equivalent of the baseline attach subclass cleanup.
    // TargetPart is the modern owned form of the baseline ref-counted string
    // stored by the attach descriptor.
    void ReleaseAttachOwnedState();

    // releases the two owned IK endpoint strings.
    void ReleaseIkOwnedState();

    // releases the two owned motion descriptor strings.
    void ReleaseMotionOwnedState();

    // Native path for the baseline control-backed curve evaluator.  The
    // multiplier and additive term are the path event's stop/start percent
    // pair; the curve itself remains owned by the event.
    float EvaluateControlBackedCurve(float Scale, int TimeMs);

    // type-4 event descriptor.  The payload is kept in
    // the modern light-property structure, without reproducing the baseline
    // aggregate's VC6 object layout.
    void ApplyLightEventDesc(const void* Desc);

    // type-7 (SPATIAL PROP ANIM) descriptor.  Beyond
    // the common eleven-word record it carries the spatial mask, the target
    // orientation/position, an optional quaternion (used only with
    // kSceneScriptFlagQuatOrient, otherwise derived from the matrix), the
    // axis-rotation angle/axis pair and the target part/type.
    void ApplySpatialPropAnimDesc(const void* Desc);

    // raw START_MOTION descriptor loading.
    void ApplyMotionDesc(const void* Desc);

    // Native payload owners for the specialized camera, PSYS and fog event
    // descriptors.  DescriptorType preserves the baseline type-mismatch branch.
    void ApplyCameraPropAnimPayload(int DescriptorType, int Mask, const float* Target4);
    void ApplyPsysPropAnimPayload(int DescriptorType, int Mask, float Target);
    void ApplyFogPropAnimDesc(const void* DescIn);
    void ApplyReverbPropAnimDesc(const void* DescIn);
    void ApplyFogPropAnimPayload(int DescriptorType, int Mask, const FSceneScriptFog8* Target);

    // Modern owner-side equivalent of the routine.  baseline type 17 carries
    // one mask dword and four payload words after the common 11-word record;
    // keep the payload in FSceneScriptEvent instead of exposing the baseline
    // subclass/subobject or VC6 deleting-destructor ABI.
    void ApplyReverbPropAnimPayload(int DescriptorType, int Mask, const FSceneScriptReverb4* Target4);

    FSceneScriptCsys* ParentCsys;
    unsigned char bOwnsParentCsys;
    const unsigned int* SceneUpAxis;
    const unsigned int* SceneFrontAxis;
    int StartTimeMs;
    int UniqueId;
    int Type;
    int PrimaryIndex;
    int SecondaryIndex;
    int EndTimeMs;
    int Unused20;
    const bool bFloorEventOwner;
    // Concrete event identity survives descriptor callbacks and type mutation.
    const int NativeEventType;
    // Canonical typed storage; reference views are the exact same member types.
    FSceneScriptUserEventDesc UserEventDesc;
    FSceneScriptFogEventDesc FogDesc;
    FSceneScriptPsysPropEventDesc PsysPropDesc;
    FSceneScriptAudioPropEventDesc AudioPropDesc;
    FSceneScriptLightEventDesc LightDesc;
    FSceneScriptReverbEventDesc ReverbDesc;
    FSceneScriptCameraEventDesc CameraDesc;
    FSceneScriptEventDesc SetCameraDesc; // actual type-2 common-only owner (44e60).
    FSceneScriptFloorEventDesc FloorDesc; // actual type-18 live 56-byte owner (49570).
    FSceneScriptPathEventDesc PathDesc; // actual type-6 live 76-byte owner (450a0).
    IParamCurve*& ParamCurve;
    FSceneScriptEventDesc& Desc;
    int& PcurvePeriod;
    float TargetPos[3];
    float TargetOrient[9];
    float CapturePos[3];
    float CaptureOrient[9];
    FQuaternion TargetQuat;
    FQuaternion CaptureQuat;
    int SpatialMask;
    FSceneScriptSpatialEventDesc SpatialDesc;
    float AxisRotAngle;
    int AxisRotAxis;
    float AxisRotApplied;
    float SpatialWrapLast;
    bool bHasTargetPos;
    bool bHasTargetOrient;
    bool bCaptured;

    char* UserEventString;
    int& CamMask;
    // Load +0x58: fovh/aspect/near/far. Start capture +0x68 GET 8/11/12/10: fovh/near/far/aspect.
    float (&CamTarget)[4];
    float CamCapture[4];
    int& LightMask;
    FSceneScriptLightProps& LightTarget;
    FSceneScriptLightProps LightCapture;
    float& StartPercent;
    float& StopPercent;
    float& PercentDelta;
    float (&PathOffset)[3];
    char* Animation;
    char* LockedBone;
    float TransTime;
    float TimeScale;
    float Weight;
    float Heading;
    float TransScale;
    float MotionStartTime;
    unsigned char Field_0x40; // TODO: recover semantic name; descriptor +0x40.
    int Field_0x50; // TODO: recover semantic name; descriptor +0x50.
    float SubSceneStartTime;
    char* Hardpoint;
    char* ParentHardpoint;
    FSceneScriptConnectString ConnectHardpointStorage;
    FSceneScriptConnectString ConnectParentHardpointStorage;
    bool bConnectStorageActive = false;
    void AdoptConnectStringStorage();
    void ReleaseConnectStringStorage();
    char* EndEffector;
    const char* LegacyTargetPart;
    const char*& TargetPart;
    FSceneScriptConnectString IkEndEffectorStorage;
    FSceneScriptConnectString IkTargetPartStorage;
    bool bIkStorageActive = false;
    FSceneScriptConnectString MotionAnimationStorage;
    FSceneScriptConnectString MotionLockedBoneStorage;
    bool bMotionStorageActive = false;
    FSceneScriptConnectString AttachTargetPartStorage;
    bool bAttachStorageActive = false;
    FSceneScriptConnectString SpatialTargetPartStorage;
    bool bSpatialStorageActive = false;
    void AdoptSpatialStringStorage();
    void SetSpatialTargetPart(const char* Name);
    FSceneScriptConnectString FloorTargetPartStorage;
    bool bFloorStorageActive = false;
    void AdoptFloorStringStorage();
    void SetFloorTargetPart(const char* Name);
    void ReleaseFloorOwnedState();
    int LegacyTargetType;
    int& TargetType;
    int& LookAtUpAxis;
    int& LookAtFrontAxis;
    float PointAt[3];
    unsigned char bPointAt;
    unsigned char bMoveTo;
    float AttachXform[12];
    // Written first by the primary hardpoint callback; retained between IK ticks.
    float IkEffectorXform[12];
    int IkId;
    int CountToRoot;
    float Damping;
    float TransitionDuration;
    float& SparamTarget;
    float SparamCapture;
    int& PsysMask;
    int& AudioMask;
    FSceneScriptAudio8& AudioTarget;
    FSceneScriptAudio8 AudioCapture;
    int& FogMask;
    FSceneScriptFog8& FogTarget;
    int FogCaptureOn;
    float FogCaptureColor[3];
    float FogCaptureStart;
    float FogCaptureEnd;
    float FogCaptureDensity;
    FSceneScriptFog8 FogCapturePacked;
    int& ReverbMask;
    FSceneScriptReverb4& ReverbTarget;
    float ReverbCapture[4];
    float& FloorTarget;
    float FloorCapture;
    float FloorWrapLast;
    int Strid;
    FSceneScriptSubtitleEventDesc SubtitleDesc;
    FSceneScriptAttachEventDesc AttachDesc;
    void PackAttachDesc();
    bool bUserDescriptorOwner = false; // USER typed lifetime: 44be0/439a0 has no curve Release.
    int SoundStartTime;
    FSceneScriptSubSceneEventDesc SubSceneDesc;
    FSceneScriptSoundEventDesc SoundDesc;
    void PackSoundDesc();

    // START_MOTION's published descriptor (scenescript_iface.h FSceneScriptMotionEventDesc): baseline keeps
    // the common words and the motion fields contiguous at event +0x28 and hands that to
    // ICompoundControl slots 3 / 4 and IScriptEngine slots 15 / 16. Refreshed from Desc and
    // the named motion fields by PackMotionDesc before each of those uses.
    FSceneScriptMotionEventDesc MotionDesc;
    void PackMotionDesc();

    // START_IK's published descriptor (scenescript_iface.h FSceneScriptStartIkDesc), the IK twin of
    // MotionDesc: baseline the routine lays the IK fields out after the common words at event
    // +0x28, with the event's own address at +0x2c, and IIKControl slots 3 / 4 / 5 and
    // IScriptEngine slots 15 / 16 receive that. Refreshed by PackIkDesc before each use.
    FSceneScriptStartIkDesc IkDesc;
    void PackIkDesc();

    // CONNECT_HARDPOINTS' published descriptor (scenescript_iface.h FSceneScriptConnectEventDesc): baseline
    // keeps the common words and the two hardpoint name pointers contiguous at event +0x28
    // (+0x54 / +0x58 = desc +0x2c / +0x30) and hands event +0x28 to ICompoundControl slot 7
    // (the branch.., 0x06f48384) and slot 8 (the branch, 0x06f4840e). Refreshed by
    // PackConnectDesc before each use.
    FSceneScriptConnectEventDesc ConnectDesc;
    void PackConnectDesc();
};

class FSceneScriptScene
{
public:
    FSceneScriptScene();
    ~FSceneScriptScene();

    void Clear();
    FSceneScriptEntity* AddEntity();
    FSceneScriptEvent* AddEvent(int DescriptorStorageType = 0);
    void BindEntitySceneAxes(FSceneScriptEntity* Entity);
    void RegisterFrameEntity(FSceneScriptEntity* Entity);
    void BindEventSceneAxes(FSceneScriptEvent* Event);
    FSceneScriptCsys& ForwardCsys() { return SceneForwardCsys_; }
    void BuildInverseCsys(float* Out12) const;
    void MarkLoaded() { PlaybackState_ = kSceneScriptPlaybackLoaded; }
    void FinalizeLoad(int DurationMs);

    int FindEntityByName(const char* Name) const;
    FSceneScriptEntity** FindEntityEntry(int Index);
    FSceneScriptEntity* EntityAt(int Index);
    const FSceneScriptEntity* EntityAt(int Index) const;

    unsigned long EntityCount() const;
    unsigned long LiveEntityCount() const;
    void* EntityHandle(int Index);
    FSceneScriptEntitySnap* EntitySnap(int Index);
    FSceneScriptEntitySnap* EntitySnapInorder(int Index);
    unsigned long UserPropCount(int Index) const;
    const char* UserPropKey(int Index, int N) const;
    GENRESULT GetEntityPos(int Index, float* OutVec3);
    GENRESULT GetEntityOrient(int Index, float* OutMat9);
    GENRESULT Attach(int Index, void* EngineObj);
    GENRESULT Detach(int Index);
    void PushBoundGeoFromScene(const FTransform& OldInverse, bool bOrient);
    GENRESULT UserProp(void* Handle, const char* Key, char** Out);
    GENRESULT SetUserProp(int Index, const char* Key, const char* Value);
    GENRESULT AddEntityFromDesc(void* Desc, int* OutIndex);
    GENRESULT RemoveEntityAt(int Index);
    GENRESULT UpdateEntityFromDesc(int Index, void* Desc);
    GENRESULT AddEventFromDesc(const void* Desc, int* OutId);
    GENRESULT RemoveEventById(int Id);
    GENRESULT SaveToFileSystem(void* FileSystem, ISceneScriptComContext* ComContext,
                               unsigned int SavedDurationMs);

    unsigned long EventCount() const { return static_cast<unsigned long>(MasterCount_); }
    void* EventDescByIndex(int Index);
    void* EventDescById(int Id);
    bool HasEventType(int Type) const;

    int DurationMs() const { return DurationMs_; }
    void SetDuration(int Ms) { DurationMs_ = Ms; }
    int ClockMs() const { return ClockMs_; }
    int PlaybackState() const { return PlaybackState_; }
    unsigned char LoopFlag() const { return bLoop_; }
    // MOV [ECX+0x30], AL — raw byte, not normalized 0/1.
    void SetLoop(unsigned char Value) { bLoop_ = Value; }

    GENRESULT StartPlayback(int Arg);
    GENRESULT StopPlayback();
    GENRESULT PausePlayback();
    GENRESULT Tick(int DtMs);

private:
    friend class FSceneScriptEntity;
    void GrowMaster();
    void GrowPending();
    void GrowActive();
    void RebuildPending();
    void StopAll();
    void DispatchWindow(int OldTime, int NewTime);
    struct FActiveGroup
    {
        std::vector<FSceneScriptEvent*> Events;
        bool bVisited = false;
    };
    void RebuildActiveGroups();
    void EmitActiveGroup(std::map<int, FActiveGroup>& Groups,
                         std::map<int, FActiveGroup>::iterator Group);
    void AppendActiveGroupEvent(FSceneScriptEvent* Event);
    void FrameUpdateEntities(int TimeMs);
    void UpdateActive(int TimeMs);
    void DestroyEntity(FSceneScriptEntity* Entity);
    void ApplyEntitySnap(FSceneScriptEntity* Entity, const FSceneScriptEntitySnap* Snap);
    bool EntityExists(int Index) const;
    void CompactPendingFrom(FSceneScriptEvent** SelectedBegin, FSceneScriptEvent** SelectedEnd);
    void DrainTimedStops(const int* TimeMs, bool bBeforeStartTime, std::set<int>* UpdatedTimes);
    FSceneScriptEvent** EraseMasterEventAt(FSceneScriptEvent** Slot);
    static int CompareEventStart(const void* Left, const void* Right);
    static bool EventAllowsMissingSecondary(int Type);

    std::map<int, FSceneScriptEntity*> Entities_;
    int EntityCount_;
    FSceneScriptEvent** Master_;
    int MasterCount_;
    int MasterCap_;

    FSceneScriptEvent** Pending_;
    int PendingCount_;
    int PendingCap_;
    int PendingBegin_;

    FSceneScriptEvent** Active_;
    int ActiveCount_;
    int ActiveCap_;

    std::optional<std::list<float>> StopTimes_;
    // baseline +f8/+fc: alias nodes; sentinel is destroyed before StopTimes_.
    std::optional<std::list<FSceneScriptEntity*>> FrameEntities_;

    int DurationMs_;
    int ClockMs_;
    int ClockAfterDt_;
    int PlaybackState_;
    unsigned char bLoop_;
    int LoopCarryMs_;
    unsigned int SceneUpAxis_;
    unsigned int SceneFrontAxis_;
    FSceneScriptCsys SceneForwardCsys_;
};

// compact+D3D defaults; the routine convert when type not in mask.
void SceneScriptInitLightEntityDefaults(FSceneScriptLightProps* Props);
void SceneScriptConvertCompactLight(FSceneScriptLightProps* Blob);
void SceneScriptCopyLightPropsToEntity(FSceneScriptEntity* Entity, const FSceneScriptLightProps* Props);
// Fill the published descriptor tail (Snap past +0x44) from the entity's native fields.
// Called once by the Lua entity loader, after every per-type field is read.
void SceneScriptPublishEntityDescTail(FSceneScriptEntity* Entity);
