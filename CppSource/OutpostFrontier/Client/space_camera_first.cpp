// Author: Giperion.

// baseline first-person (cockpit) camera (in-flight controller + 0x00c, vtable .rdata 0x005db944).

// Ported: the constructor (0x00517180), the name slot (0x00517330) and the ini loader
// (0x005173c0) -- the [Cockpit] walk, the head_turn globals, the obsolete-[Camera] warning, the
// cockpit mesh load on the holder at +0x190 (the routine / the routine / the routine), the
// camera-part search (CompoundInstanceAnalyze with the routine, then IEngine slots 0x98 and
// 0x90 into +0x13c and +0x130) and the closing aspect check. This replaced the cockpit_load.cpp
// twin of the routine (2026-09-25). Later the same day: the attach (the branch), the
// Event::Handler face (the routine), the mesh release (the routine) and the per-frame update
// (the branch, slot 0x6c).

#include "space_camera_first.h"

#include "NeuronRuntime.h"
#include "common_cobject.h"   // CompoundInstanceAnalyze
#include "common_csys.h"      // Acquire3DMathEngine (the routine)
#include "common_goods.h"     // FindHardpoint_PS, HardpointInfo
#include "common_iface.h"     // IObjRW / IObjInspect (thiscall getters)
#include "common_ini.h"
#include "neuroncomponents.h"
#include "engine/sceneruntime.h"
#include "client_gameobj.h"   // FClientShipGameobj (+0x158 / +0x188, slot 0x18c)
#include "engine_light.h"     // EngineLightClearRegisteredList / EngineLightSetAmbientRgbBytes
#include "game_options.h"     // GetGameOption (the routine)
#include "globals.h"          // CopyBoundedString (the routine)
#include "jump_effect.h"      // FJumpShipEffect::Ambient (the routine)
#include "player_behavior.h"  // GetPlayerObjRW (the routine)
#include "hud_target.h"       // HudIsMousePickBlocked (the routine)
#include "perf_options.h"     // FindPerfOption (the routine)
#include "space_scene_obj_draw.h" // SpaceObjDrawPass (the routine), the routine / the routine
#include "win3d_render.h"     // ClearWin3DBuffers (the routine)
#include "x86math.h"

#include <cstddef>

#include <math.h>
#include <string.h>

// .data ships 30.0f / 25.0f (0x41f00000 / 0x41c80000). The [Cockpit] head_turn loader
// (0x00517545 / 0x0051755a) is the only code that touches either; nothing in the PE reads them.
float G_CockpitHeadTurnX = 30.0f;
float G_CockpitHeadTurnY = 25.0f;

// this file's own lazily-filled math engine cache (the routine at 0x00517a26,
// , 0x00517aec and in the per-frame slot).
I3DMathEngine* G_FirstPersonMathEngine = nullptr;

namespace
{
    const float  kDegreesToRadians = 0.01745329238474369f;   // .rdata 0x005c77f4
    const double kExpectedAspect = 1.333;
    const double kAspectTolerance = 0.05;

    // 's capacity at both call sites in this loader (0x005174d1, 0x00517515).
    const unsigned kIniStringCapacity = 0x104;

    const char kCockpitCpp[] = "Client\\space\\Cockpit.cpp";


    // 's head-motion state and tuning. The two state floats live in .bss; the four
    // tuning values are .data, each read only by that update.
    float G_CockpitLastForwardSpeed = 0.0f;
    float G_CockpitHeadLag = 0.0f;
    float G_CockpitLagScale = 0.3f;           // (0x3e99999a)
    float G_CockpitHeadPivot = 1.5f;
    float G_CockpitRollPerSpin = -4.0f;
    float G_CockpitRollLimit = 4.0f;

    // The face's slot 0 is called __thiscall (face in ECX, id on the stack); __fastcall with an
    // unused EDX has the same shape.


    // .rdata 0x005db93c: one slot, the routine.

    // .rdata 0x005db994: FirstPerson's slot 0x50 is IDPMsgHandler::OnDisconnect (0x00401e90,
    // a bare RET), not GameCamera's the routine -- its scene slot runs the two list walks
    // itself around the cockpit draw.

    I3DMathEngine* FirstPersonMathEngine()
    {
        if (G_FirstPersonMathEngine == nullptr)
            G_FirstPersonMathEngine = Acquire3DMathEngine();
        return G_FirstPersonMathEngine;
    }

    FMatrix const& AsMatrix(const float* Nine)
    {
        return *reinterpret_cast<FMatrix const*>(Nine);
    }

    FVector const& AsVector(const float* Three)
    {
        return *reinterpret_cast<FVector const*>(Three);
    }

    // the CompoundInstanceAnalyze callback. Asks the engine for the part's name
    // (IEngine slot 0x68, GetInstanceName, into a stack FEngineName), treats a null name as ""
    // (.rdata 0x005c865c), and records the part in the context when the name contains "cam"
    // (.rdata 0x005db934). No early out: a later match overwrites an earlier one.
    void __cdecl FindCockpitCameraPart(long Instance, void* Context)
    {
        FEngineName Name;
        const char* Text =
            NeuronRuntime::Engine->GetInstanceName(&Name, reinterpret_cast<void*>(Instance))->Text;
        if (Text == nullptr)
            Text = "";
        if (strstr(Text, "cam") != nullptr)
            *static_cast<long*>(Context) = Instance;
    }

    void SetIdentity(float* M)
    {
        for (int i = 0; i < 9; ++i)
            M[i] = 0.0f;
        M[0] = 1.0f;
        M[4] = 1.0f;
        M[8] = 1.0f;
    }
}

void FirstPersonCameraConstruct(FFirstPersonCamera* Cam)
{
    // .
    HudCameraConstruct((FHudCamera*)Cam);

    // the routine constructs the cockpit mesh holder in place at +0x190.
    MeshHolderConstruct(&Cam->Mesh);

    // the face table 0x005db93c, one slot the routine.

    // three transforms, each a zero position followed by an identity matrix. baseline
    // writes them out of order -- the matrices' off-diagonals first, then the three 1.0f -- but
    // the result is exactly this.
    Cam->PositionA[0] = 0.0f;
    Cam->PositionA[1] = 0.0f;
    Cam->PositionA[2] = 0.0f;
    SetIdentity(Cam->OrientationA);

    Cam->HeadPosition[0] = 0.0f;
    Cam->HeadPosition[1] = 0.0f;
    Cam->HeadPosition[2] = 0.0f;
    SetIdentity(Cam->HeadOrientation);

    Cam->PositionC[0] = 0.0f;
    Cam->PositionC[1] = 0.0f;
    Cam->PositionC[2] = 0.0f;
    SetIdentity(Cam->OrientationC);

    // this class clears BOTH of the HUDCamera bytes at +0xfc and +0xfd -- the only
    // evidence that +0xfd is a field at all rather than padding. It leaves +0xfe at the 1 the
    // HUDCamera ctor put there.
    Cam->bFlagFC = 0;
    Cam->bFlagFD = 0;

    Cam->bFlag1A0 = 1;
    Cam->Pad1A1[0] = 0;
    Cam->Pad1A1[1] = 0;
    Cam->Pad1A1[2] = 0;
}

const char* FirstPersonCameraGetName()
{
    return "FirstPerson";       // -> .rdata
}

bool FirstPersonCameraOnEvent(FFirstPersonCamera* Cam, unsigned MsgId)
{
    // (thiscall on the face, RET 4): the routine -> false; otherwise HUDCamera's
    // on the same face, its answer normalised (SETNZ).
    if (HudIsMousePickBlocked())
        return false;
    return HudCameraOnEvent((FHudCamera*)Cam, MsgId) != false;
}

void FirstPersonCameraReleaseMesh(FFirstPersonCamera* Cam)
{
    // ADD ECX,0x190 / JMP the routine.
    ClientMeshHolderRelease(&Cam->Mesh);
}

void FirstPersonCameraAttach(FFirstPersonCamera* Cam, void* Object)
{
    // (slot 0x58, __thiscall, RET 4).
    // the HUDCamera attach first (watcher swap + HUD retarget).
    HudCameraAttach((FHudCamera*)Cam, Object);

    FSpaceCamera* const Space = SpaceCameraOf((FHudCamera*)Cam);

    IObjRW* const Watched = static_cast<IObjRW*>(GameCameraGetTarget((FGameCamera*)Cam));
    if (Watched != nullptr)
    {
        IObjInspect* const Inspect = Watched;

        // slot 4 (get_orientation), whose body is folded with slot 3 and hands back
        // the whole transform -- twelve dwords go into the camera's world rotation + position.
        Transform const& ObjectXform = Inspect->get_transform();
        Space->bViewDirty = 1;
        memcpy(Space->World, &ObjectXform, 12 * sizeof(float));

        // FindHardpoint_PS(get_index(), "HpCockpit", handle, info).
        long Handle = 0;
        HardpointInfo Info;
        if (FindHardpoint_PS(Inspect->get_index(), "HpCockpit", Handle, Info))
        {
            Cam->PositionA[0] = Info.Position.x;                           // +0x100
            Cam->PositionA[1] = Info.Position.y;
            Cam->PositionA[2] = Info.Position.z;
            memcpy(Cam->OrientationA, &Info.Orient, 9 * sizeof(float));   // +0x10c
        }
        else
        {
            // no cockpit hardpoint -- four units behind the origin, identity.
            Cam->PositionA[0] = 0.0f;
            Cam->PositionA[1] = 0.0f;
            Cam->PositionA[2] = -4.0f;
            Cam->OrientationA[8] = 1.0f;
            Cam->OrientationA[4] = 1.0f;
            Cam->OrientationA[0] = 1.0f;
            Cam->OrientationA[7] = 0.0f;
            Cam->OrientationA[6] = 0.0f;
            Cam->OrientationA[5] = 0.0f;
            Cam->OrientationA[3] = 0.0f;
            Cam->OrientationA[2] = 0.0f;
            Cam->OrientationA[1] = 0.0f;
        }
    }

    // the watcher is tested again.
    IObjRW* const Again = static_cast<IObjRW*>(GameCameraGetTarget((FGameCamera*)Cam));
    if (Again != nullptr)
    {
        I3DMathEngine* const Math = FirstPersonMathEngine();

        // MulMatrix(stack, world rotation, hardpoint orientation) -- the head's
        // base orientation in world space; kept in a second stack copy.
        FMatrix Product;
        Math->MulMatrix(Product, AsMatrix(Space->World), AsMatrix(Cam->OrientationA));
        FMatrix const Head = Product;

        // slot 0, get_position.
        Vector const& ObjectPosition = static_cast<IObjInspect*>(Again)->get_position();
        float Position[3] = {ObjectPosition.x, ObjectPosition.y, ObjectPosition.z};

        // + world rotation × hardpoint position (TransformMatrix, vt+0x2c).
        FVector Offset;
        FirstPersonMathEngine()->TransformMatrix(Offset, AsMatrix(Space->World),
                                                 AsVector(Cam->PositionA));
        Position[0] = Offset.X + Position[0];
        Position[1] = Offset.Y + Position[1];
        Position[2] = Offset.Z + Position[2];

        // + head orientation × the cockpit mesh's camera-part position (+0x130).
        FirstPersonMathEngine()->TransformMatrix(Offset, Head, AsVector(Cam->HeadPosition));
        Position[0] = Offset.X + Position[0];
        Position[1] = Offset.Y + Position[1];
        Position[2] = Offset.Z + Position[2];

        // +0x160 keeps the eye point; the view is marked dirty; the camera's world
        // transform becomes {head orientation, eye point}.
        Cam->PositionC[0] = Position[0];
        Cam->PositionC[1] = Position[1];
        Cam->PositionC[2] = Position[2];
        Space->bViewDirty = 1;
        memcpy(Space->World, &Head, 9 * sizeof(float));
        Space->WorldPos[0] = Position[0];
        Space->WorldPos[1] = Position[1];
        Space->WorldPos[2] = Position[2];
    }

    Cam->bFlag1A0 = 1;
}

void FirstPersonCameraUpdate(FFirstPersonCamera* Cam, float DeltaSeconds)
{
    // (slot 0x6c, thiscall RET 4).
    FHudCamera* const Hud = (FHudCamera*)Cam;
    FSpaceCamera* const Space = SpaceCameraOf(Hud);

    // only with a watched client ship -- watcher - 8, its +0xc'd
    // pointer's +0x4, CObject mask 0x503.
    bool bShip = false;
    {
        unsigned char* const Held = const_cast<unsigned char*>(
            reinterpret_cast<const unsigned char*>(Hud->Watched.target()));
        if (Held != nullptr)
        {
            unsigned char* const Object = Held - 8;
            if (Object != nullptr)
            {
                unsigned char* const Plus0c = Object + 0xc;
                if (Plus0c != nullptr)
                {
                    CEqObj* const EqObj = *reinterpret_cast<CEqObj**>(Plus0c + 4);
                    if (EqObj != nullptr &&
                        (static_cast<unsigned>(EqObj->ObjectClass) & 0x503u) == 0x503u)
                        bShip = true;
                }
            }
        }
    }

    if (bShip)
    {
        IObjRW* const Object = static_cast<IObjRW*>(GameCameraGetTarget((FGameCamera*)Hud));

        // the ship's transform (slot 0xc); its rotation is read through this pointer
        // for the rest of the function.
        Transform const& ShipXform = Object->get_transform();
        FMatrix const& ShipRot = AsMatrix(&ShipXform.Rotation.E[0][0]);

        // ship rotation x cockpit hardpoint orientation (+0x10c), copied to the
        // frame's mesh orientation.
        FMatrix CockpitRot;
        FirstPersonMathEngine()->MulMatrix(CockpitRot, ShipRot, AsMatrix(Cam->OrientationA));
        FMatrix MeshRot = CockpitRot;

        // slot 0, the ship's position.
        Vector const& ShipPos = Object->get_position();
        float Eye[3] = {ShipPos.x, ShipPos.y, ShipPos.z};

        // on the first update after an attach (+0x1a0), the ship's
        // behavior manager goes back to the normal camera mode.
        IBehaviorManager* const Manager = GetBehaviorManager(Object);
        if (Cam->bFlag1A0 != 0 && Manager != nullptr)
            Manager->set_camera_mode(IBehaviorManager::CameraModeNormal);

        // + ship rotation x cockpit hardpoint position (+0x100); the
        // point is kept in +0x160.
        FVector Offset;
        FirstPersonMathEngine()->TransformMatrix(Offset, ShipRot, AsVector(Cam->PositionA));
        Eye[0] = Offset.X + Eye[0];
        Cam->PositionC[0] = Eye[0];
        Eye[1] = Offset.Y + Eye[1];
        Cam->PositionC[1] = Eye[1];
        Eye[2] = Offset.Z + Eye[2];
        Cam->PositionC[2] = Eye[2];

        // the cockpit mesh follows (the routine / the routine /
        // ).
        ClientMeshHolderSetPosition(&Cam->Mesh, Eye);
        ClientMeshHolderSetOrientation(&Cam->Mesh, &MeshRot.D[0][0]);
        ClientMeshHolderUpdate(&Cam->Mesh);

        // the velocity (slot 1) into ship space (vt+0x38).
        const Vector Velocity = Object->get_velocity();
        FVector LocalVelocity;
        FirstPersonMathEngine()->TransposeTransform(LocalVelocity, ShipRot,
                                                    AsVector(&Velocity.x));

        // forward acceleration, 0 unless dt > 0.
        float Accel = 0.0f;
        if (DeltaSeconds > 0.0)
        {
            Accel = static_cast<float>(
                (static_cast<double>(LocalVelocity.Z) - G_CockpitLastForwardSpeed) / DeltaSeconds);
        }

        // the lag the head wants, (1 - 1/(|a| + 1)) * 0.3f with a's
        // sign, less the current lag, times 0.01; the forward speed is remembered.
        double Step = (1.0 - 1.0 / (fabs(static_cast<double>(Accel)) + 1.0)) * G_CockpitLagScale;
        if (Accel < 0.0f)
            Step = -Step;
        Step = Step - G_CockpitHeadLag;
        G_CockpitLastForwardSpeed = LocalVelocity.Z;
        Step = Step * 0.01;

        // at most 3 * dt either way, then added to the lag.
        const float Limit = static_cast<float>(DeltaSeconds * 3.0);
        const float NegLimit = -Limit;
        if (Step < NegLimit)
            Step = NegLimit;
        else if (Step > Limit)
            Step = Limit;
        G_CockpitHeadLag = static_cast<float>(Step + G_CockpitHeadLag);
        const float Lag = G_CockpitHeadLag;

        // the mesh's orientation back from the engine.
        memcpy(&MeshRot, ClientMeshHolderGetOrientation(&Cam->Mesh), sizeof(MeshRot));

        // the rigid eye -- mesh position + mesh rotation x the camera
        // part's position (+0x130), the routine.
        FVector HeadOffset;
        FirstPersonMathEngine()->TransformMatrix(HeadOffset, MeshRot, AsVector(Cam->HeadPosition));
        {
            float const* const MeshPos = ClientMeshHolderGetPosition(&Cam->Mesh);
            Eye[0] = MeshPos[0] + HeadOffset.X;
            Eye[1] = MeshPos[1] + HeadOffset.Y;
            Eye[2] = MeshPos[2] + HeadOffset.Z;
        }
        // an identity written to the stack matrix the roll overwrites
        // before any read -- dead in baseline too.

        // a function-local static (guard the field bit 0, empty atexit
        // destructor) takes the angular velocity (slot 2) and then its ship-space version.
        static Vector LocalSpin = {0.0f, 0.0f, 0.0f};   // ..the field
        LocalSpin = Object->get_angular_velocity();
        FVector Spin;
        FirstPersonMathEngine()->TransposeTransform(Spin, ShipRot, AsVector(&LocalSpin.x));
        LocalSpin.x = Spin.X;
        LocalSpin.y = Spin.Y;
        LocalSpin.z = Spin.Z;

        // roll = spin.y * , clamped to +/-the field,
        // turned into a matrix by RotateMatrix((0, 0, roll)).
        double Roll = static_cast<double>(LocalSpin.y) * G_CockpitRollPerSpin;
        const float NegRollLimit = -G_CockpitRollLimit;
        if (Roll < NegRollLimit)
            Roll = NegRollLimit;
        else if (Roll > G_CockpitRollLimit)
            Roll = G_CockpitRollLimit;
        Vector RollAngles;
        RollAngles.z = static_cast<float>(Roll);
        RollAngles.x = 0.0f;
        RollAngles.y = 0.0f;
        const Matrix RollMatrix = RotateMatrix(RollAngles);

        // the head pivot (0, the field, 0) through the camera part's
        // orientation (+0x13c), taken off the part's position (+0x130).
        Vector Pivot;
        Pivot.x = 0.0f;
        Pivot.y = G_CockpitHeadPivot;
        Pivot.z = 0.0f;
        FVector PivotOffset;
        FirstPersonMathEngine()->TransformMatrix(PivotOffset, AsMatrix(Cam->HeadOrientation),
                                                 AsVector(&Pivot.x));
        float Base[3];
        Base[0] = Cam->HeadPosition[0] - PivotOffset.X;
        Base[1] = Cam->HeadPosition[1] - PivotOffset.Y;
        Base[2] = Cam->HeadPosition[2] - PivotOffset.Z;

        // the pivot pushed back by the lag, rolled, put back on the base.
        Vector Swing;
        Swing.x = Pivot.x;
        Swing.y = Pivot.y;
        Swing.z = static_cast<float>(static_cast<double>(Pivot.z) + -static_cast<double>(Lag));
        FVector Swung;
        FirstPersonMathEngine()->TransformMatrix(Swung, AsMatrix(&RollMatrix.E[0][0]),
                                                 AsVector(&Swing.x));
        float Local[3];
        Local[0] = Swung.X + Base[0];
        Local[1] = Swung.Y + Base[1];
        Local[2] = Swung.Z + Base[2];

        // into the mesh's frame, onto the mesh position -> +0x160.
        FVector Moved;
        FirstPersonMathEngine()->TransformMatrix(Moved, MeshRot, AsVector(Local));
        {
            float const* const MeshPos = ClientMeshHolderGetPosition(&Cam->Mesh);
            Cam->PositionC[0] = Moved.X + MeshPos[0];
            Cam->PositionC[1] = Moved.Y + MeshPos[1];
            Cam->PositionC[2] = Moved.Z + MeshPos[2];
        }

        // the "cockpit" perf option (the routine, applied value +0x14,
        // no null test) under 0.999 keeps the head rigid; otherwise the lag and roll stay.
        FMatrix Final;
        if (FindPerfOption("cockpit")->Applied < 0.999)
        {
            // the eye back to the rigid point; mesh x part (+0x13c) x
            // +0x16c.
            Cam->PositionC[0] = Eye[0];
            Cam->PositionC[1] = Eye[1];
            Cam->PositionC[2] = Eye[2];
            FMatrix Head;
            FirstPersonMathEngine()->MulMatrix(Head, MeshRot, AsMatrix(Cam->HeadOrientation));
            const FMatrix HeadCopy = Head;
            FirstPersonMathEngine()->MulMatrix(Final, HeadCopy, AsMatrix(Cam->OrientationC));
        }
        else
        {
            // the same product, then x the roll.
            FMatrix Head;
            FirstPersonMathEngine()->MulMatrix(Head, MeshRot, AsMatrix(Cam->HeadOrientation));
            const FMatrix HeadCopy = Head;
            FMatrix Offsetted;
            FirstPersonMathEngine()->MulMatrix(Offsetted, HeadCopy, AsMatrix(Cam->OrientationC));
            const FMatrix OffsettedCopy = Offsetted;
            FirstPersonMathEngine()->MulMatrix(Final, OffsettedCopy, AsMatrix(&RollMatrix.E[0][0]));
        }

        // the routine shakes the orientation.
        HudCameraApplyShake(Hud, &Final.D[0][0]);

        // world = {orientation, +0x160}, view dirty.
        memcpy(Space->World, &Final, 9 * sizeof(float));
        Space->WorldPos[0] = Cam->PositionC[0];
        Space->WorldPos[1] = Cam->PositionC[1];
        Space->WorldPos[2] = Cam->PositionC[2];
        Space->bViewDirty = 1;
    }

    // HUDCamera's update, then the attach flag drops.
    HudCameraUpdate(Hud, DeltaSeconds);
    Cam->bFlag1A0 = 0;
}

void FirstPersonCameraSceneDraw(FFirstPersonCamera* Cam, unsigned LayerFlags, unsigned Param2)
{
    // (slot 0x78, thiscall RET 8).
    FHudCamera* const Hud = (FHudCamera*)Cam;
    FSpaceCamera* const Space = SpaceCameraOf(Hud);

    // the watcher at +0xc0 set, and the object (watcher - 8) non-null.
    const unsigned char* const Held =
        reinterpret_cast<const unsigned char*>(Hud->Watched.target());
    if (Held == nullptr)
        return;
    if (Held - 8 == nullptr)
        return;

    // the routine(LayerFlags, Param2).
    SpaceObjDrawPass(Space, LayerFlags, Param2);

    // the "cockpit" perf option's applied value (+0x14, no null test),
    // kept in the first argument's stack slot for the second compare below.
    const float Applied = FindPerfOption("cockpit")->Applied;

    // FCOMP 0.999 / TEST AH,0x41 / JZ -- strictly greater (ordered)
    // clears depth outright; otherwise depth is cleared only with the HUD option (0xa) on.
    if (static_cast<double>(Applied) > 0.999 || GetGameOption(kGameOptHud) != 0)
        ClearWin3DBuffers(kWin3DClearDepth);

    // the routine.
    SpaceEnvFxBeginCockpitPass(Space);

    // the same compare, JNZ past the cockpit when not strictly greater.
    if (static_cast<double>(Applied) > 0.999)
    {
        // the cockpit mesh pass. The camera's near / far (slots 0x20
        // / 0x24) are saved, the routine(0.05f, 500.0f) (.rdata 0x005c8914 / 0x005c8918), and
        // (this).
        ICamera* const Camera = AsCamera(Space);
        const float SavedNear = Camera->GetZNear();
        const float SavedFar = Camera->GetZFar();
        SpaceCameraSetNearFar(Space, 0.05f, 500.0f);
        Win3DApplyCamera(Camera);

        // the player (the routine) in the jump arm (+0x158 == 2 or 3,
        // read without a class test) -- the routine, then the jump effect's ambient
        // (the routine = its +0x1c) through the routine when the effect and the ambient are
        // both set.
        FClientShipGameobj* const Ship =
            static_cast<FClientShipGameobj*>(static_cast<IObjRW*>(GetPlayerObjRW()));
        if (Ship != nullptr && (Ship->Field_0x158 == 2 || Ship->Field_0x158 == 3))
        {
            EngineLightClearRegisteredList();
            FJumpShipEffect* const JumpFx = static_cast<FJumpShipEffect*>(Ship->Field_0x188);
            if (JumpFx != nullptr && JumpFx->Ambient != nullptr)
                EngineLightSetAmbientRgbBytes(JumpFx->Ambient);
        }
        else
        {
            // the cockpit mesh's world bounding sphere (the routine),
            // the flag 1 unless the routine is below 0.999 (FCOMP double the field,
            // TEST AH,1: C0 -> 0), the ship's slot 0x18c draw state (OutB zeroed first), then
            // (centre, radius, this, A, B, flag). A missing player reaches the slot
            // call through null, as in baseline.
            float Center[3];
            const float Radius = ClientMeshHolderGetBoundingSphere(&Cam->Mesh, Center);
            unsigned char Flag = 1;
            if (SpaceEffectPerfOneMinus() < 0.999)
                Flag = 0;
            unsigned DrawStateB = 0;
            const unsigned DrawStateA = Ship->Slot18c(&DrawStateB);
            SpaceApplyObjectLighting(Center, Radius, Space, DrawStateA, DrawStateB, Flag);
        }

        // depth range [0.75, 1] (Pipeline slot 23), the cockpit mesh
        // (the routine with flags 0x100) when the holder has an instance (the routine),
        // depth range [0, 0.75], and the saved near / far back through the routine.
        NeuronRuntime::Pipeline->SetDepthRange(0.75f, 1.0f);
        if (ClientMeshHolderHasInstance(&Cam->Mesh))
            ClientMeshHolderRender(&Cam->Mesh, Space, 0x100);
        NeuronRuntime::Pipeline->SetDepthRange(0.0f, 0.75f);
        SpaceCameraSetNearFar(Space, SavedNear, SavedFar);
    }

    // HUDCamera's slot 0x78 body, called directly.
    HudCameraDraw((FGameCamera*)Hud);

    // the routine.
    SpaceEnvFxEndCockpitPass(Space);
}

void FirstPersonCameraLoad(FFirstPersonCamera* Cam, const char* CockpitIniPath)
{
    // (thiscall, the path), in baseline's order. Returns nothing.

    // [ESP+0x28]/[ESP+0x2c] and [ESP+0x130]/[ESP+0x134]: two length-prefixed 0x104-byte
    // strings, both {0, ""} before the reader opens (0x00517419..0x00517425).
    unsigned MeshLength = 0;
    char MeshName[kIniStringCapacity];
    MeshName[0] = 0;
    unsigned MaterialLibraryLength = 0;
    char MaterialLibrary[kIniStringCapacity];
    MaterialLibrary[0] = 0;

    INI_Reader Reader;
    if (!Reader.open(CockpitIniPath, false))
    {
        // Client\space\Cockpit.cpp:0xe4 -- a warning only; the rest of the loader still runs.
        FDUMP(0x100002, "%s(%d) : *** WARNING: cockpit file not found '%s'\n",
              kCockpitCpp, 0xe4, CockpitIniPath);
    }

    while (Reader.read_header())
    {
        if (Reader.is_header("Cockpit"))
        {
            while (Reader.read_value())
            {
                if (Reader.is_value("mesh"))
                {
                    // a null value clears the string; otherwise the routine copies
                    // at most 0x103 characters and its return (the length) is stored.
                    const char* const Value = Reader.get_value_string();
                    if (Value == nullptr)
                    {
                        MeshLength = 0;
                        MeshName[0] = 0;
                    }
                    else
                    {
                        MeshLength = CopyBoundedString(MeshName, kIniStringCapacity, Value);
                    }
                }
                else if (Reader.is_value("material_library"))
                {
                    // same shape, into the second 0x104-byte string.
                    const char* const Value = Reader.get_value_string();
                    if (Value == nullptr)
                    {
                        MaterialLibraryLength = 0;
                        MaterialLibrary[0] = 0;
                    }
                    else
                    {
                        MaterialLibraryLength =
                            CopyBoundedString(MaterialLibrary, kIniStringCapacity, Value);
                    }
                }
                else if (Reader.is_value("head_turn"))
                {
                    // the only two values in the whole cockpit file that land in
                    // globals rather than on an object.
                    G_CockpitHeadTurnX = Reader.get_value_float(0);
                    G_CockpitHeadTurnY = Reader.get_value_float(1);
                }
            }
            continue;
        }

        if (Reader.is_header("Camera"))
        {
            // Client\space\Cockpit.cpp:0xf9
            FDUMP(0x100002,
                  "%s(%d) : *** WARNING: header [Camera] is no longer supported, found in file (%s)\n",
                  kCockpitCpp, 0xf9, CockpitIniPath);
        }
    }
    (void)MaterialLibraryLength;

    // CMP [ESP+0x28],0 -- the mesh is only loaded when its stored LENGTH is
    // non-zero (an empty `mesh =` loads nothing), and the material-library buffer is passed
    // alongside it whether or not a value was given -- an empty string, never null.
    if (MeshLength != 0)
        ClientMeshHolderLoad(&Cam->Mesh, MeshName, MaterialLibrary);

    // the instance is placed at the origin and updated, unconditionally.
    const float Origin[3] = {0.0f, 0.0f, 0.0f};
    ClientMeshHolderSetPosition(&Cam->Mesh, Origin);
    ClientMeshHolderUpdate(&Cam->Mesh);

    // the compound is walked with the routine to find the part whose name
    // contains "cam"; only when one was found does the head transform come out of the engine
    // -- nine floats from IEngine slot 0x98 into +0x13c and three from slot 0x90 into +0x130.
    long CameraPart = -1;
    CompoundInstanceAnalyze(reinterpret_cast<long>(Cam->Mesh.EngineInstance),
                            &FindCockpitCameraPart, &CameraPart);
    if (CameraPart != -1)
    {
        void* const Part = reinterpret_cast<void*>(CameraPart);
        memcpy(Cam->HeadOrientation, NeuronRuntime::Engine->GetInstanceOrientation(Part),
               9 * sizeof(float));
        const float* const Position = NeuronRuntime::Engine->GetInstancePosition(Part);
        Cam->HeadPosition[0] = Position[0];
        Cam->HeadPosition[1] = Position[1];
        Cam->HeadPosition[2] = Position[2];
    }

    // the closing 4:3 check, read through the vtable exactly as the third-person
    // loader reads it.
    ICamera* const Space = AsCamera(SpaceCameraOf((FHudCamera*)Cam));

    // Each slot's return is multiplied by the float 0.0174532924f (the field) on the x87
    // stack, FPTAN; the first tangent is parked as a double, the second divides it (FDIVR).
    const double TanX = tan(static_cast<double>(Space->GetHalfFovXDeg()) *
                            static_cast<double>(kDegreesToRadians));
    const double TanY = tan(static_cast<double>(Space->GetFovYDeg()) *
                            static_cast<double>(kDegreesToRadians));
    const double Aspect = TanX / TanY;

    // |Aspect - 1.333| FCOMP 0.05, TEST AH,1 / JNZ -> skip. C0 is set
    // for "less" AND for unordered, so the warning is given only for an ordered >= (a NaN
    // ratio stays silent).
    if (fabs(Aspect - kExpectedAspect) >= kAspectTolerance)
    {
        // Client\space\Cockpit.cpp:0x113
        FDUMP(0x100002, "%s(%d) : *** WARNING: cockpit aspect (%f) is not PC-compatible?\n",
              kCockpitCpp, 0x113, Aspect);
    }
}

char const* FFirstPersonCamera::GetName()
{
    return FirstPersonCameraGetName();
}

void FFirstPersonCamera::ScaleEnvironmentEffects()
{
}

void FFirstPersonCamera::Attach(void* Object)
{
    FirstPersonCameraAttach(this, Object);
}

void FFirstPersonCamera::Update(float DeltaSeconds)
{
    FirstPersonCameraUpdate(this, DeltaSeconds);
}

void FFirstPersonCamera::DrawScene(unsigned LayerFlags, unsigned Param2)
{
    FirstPersonCameraSceneDraw(this, LayerFlags, Param2);
}

bool FFirstPersonCamera::OnEvent(unsigned MsgId)
{
    return FirstPersonCameraOnEvent(this, MsgId);
}

void FFirstPersonCamera::Deactivate()
{
    HudCameraDerivedDeactivate(this);
}
