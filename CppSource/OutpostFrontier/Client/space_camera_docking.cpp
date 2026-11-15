// Author: Giperion.

// The docking camera: ctor the routine, slots the routine (0x58), the branch (0x64),
// (0x68), the branch (0x6c), and the non-virtual arm the routine.

#include "space_camera_docking.h"

#include "fade_to_black.h"
#include "player_behavior.h"
#include "player_spawn.h"
#include "space_camera.h"
#include "space_letterbox.h"

#include "common_csys.h"
#include "common_iface.h"   // IObjInspect -- the ship's thiscall getters

#include <string.h>

namespace
{
    // / 0x0051ea88: the same third of a second every cinematic camera uses.
    const float kLetterboxSpeed = 3.0f;

    // the elapsed counter is seeded with a tenth of a second, not with zero.
    const float kDockingElapsedSeed = 0.1f;

    // Same `- 8` as everywhere else: the watcher holds the Watchable, which is eight bytes into
    // the IObjRW.
    const int kWatchableOffset = 8;

    // the player's ship as an IObjRW, or null. baseline reads the Watched pointer out
    // of the BaseWatcher at .data 0x00679744 and subtracts eight; the port's watcher accessor
    // hands back that same pointer.
    void* GetPlayerShipObjRW()
    {
        void* const Watched = GetPlayerShipWatcher();
        if (Watched == nullptr)
            return nullptr;
        return static_cast<unsigned char*>(Watched) - kWatchableOffset;
    }

    // the GameCamera table with 0x58 the routine, 0x60 the routine (the
    // shared `XOR AL,AL / RET` -- "never finished"), 0x64 the branch, 0x68 the routine and
    // 0x6c the branch. Slot 0x54 stays GameCamera's name (the branch). Leaves take `this` on
    // the stack, the port's slot convention.

}

void DockingCameraConstruct(FDockingCamera* Cam)
{
    // the FGameCamera ctor, then the three own fields, then the vptr swap. No null
    // test on `this`.
    GameCameraConstruct((FGameCamera*)Cam);

    Cam->bFadePending = 0;          // , a BYTE
    Cam->TotalSeconds = 0.0f;       // , written as a zero dword
    Cam->ElapsedSeconds = 0.0f;

    // vptr = the branch.
}

void DockingCameraAttach(FDockingCamera* Cam, void* Object)
{
    // and 0x0051eaaa: before the watcher is touched at all, the player ship is told
    // to stop steering -- its current behaviour direction is reset, and its slide/strafe burst
    // is cancelled with StrafeNone. Both go through IBehaviorManager on the player IObjRW.
    ResetPlayerBehaviorDirection();
    SetPlayerStrafeBurst(0);   // , PUSH 0 == StrafeNone

    // the usual watcher swap, and the usual short-circuit when it already holds
    // exactly that target.
    Watchable const* const Target =
        (Object != nullptr)
            ? reinterpret_cast<Watchable const*>(static_cast<unsigned char*>(Object) +
                                                 kWatchableOffset)
            : nullptr;

    if (Target != Cam->Watched.target())
        Cam->Watched.set(Target);
}

bool DockingCameraActivate(FDockingCamera* Cam, const int* ViewRect4)
{
    // FGameCamera's activate, and its result is what comes back.
    const bool bBaseResult = GameCameraActivate((FGameCamera*)Cam, ViewRect4);

    // bars in.
    SetLetterboxTarget(1.0f, kLetterboxSpeed);

    // the countdown is disarmed on activate. Note it clears the FLAG only -- the two
    // floats keep whatever the last sequence left in them, and the arm below rewrites both.
    Cam->bFadePending = 0;

    // Unlike the script and static cameras, this one does NOT notify the ship. Worth saying out
    // loud: the three calls are simply absent from 0x0051ea50, not deferred.

    return bBaseResult;
}

void DockingCameraDeactivate(FDockingCamera* Cam)
{
    // the base deactivate is the empty IDPMsgHandler::OnDisconnect.
    // bars out.
    SetLetterboxTarget(0.0f, kLetterboxSpeed);

    // and the countdown is disarmed again.
    Cam->bFadePending = 0;
}

void DockingCameraUpdate(FDockingCamera* Cam, float DeltaSeconds)
{
    // the aim half runs only while there is a ship to aim at. The countdown half
    // below runs either way.
    void* const Ship = GetPlayerShipObjRW();
    if (Ship != nullptr)
    {
        // `MOV ECX,ship / CALL [EDX]` -- IObjInspect::get_position (slot 0,
        // __thiscall).
        const Vector& ShipPosition = static_cast<IObjInspect*>(Ship)->get_position();

        const float Ship3[3] = {ShipPosition.x, ShipPosition.y, ShipPosition.z};

        // and the camera's own position comes out of its slot 0x0c -- read through
        // the interface, not off the field, because a derived class could override it.
        FSpaceCamera* const Base = (FSpaceCamera*)Cam;
        const float* const CameraPosition = AsCamera(Base)->GetPosition();

        // the direction is ship minus camera, computed Z first because
        // that is the order the FPU stack unwinds in.
        Vector Direction;
        Direction.x = Ship3[0] - CameraPosition[0];
        Direction.y = Ship3[1] - CameraPosition[1];
        Direction.z = Ship3[2] - CameraPosition[2];

        // Common's LookMatrixYup, the same export baseline calls.
        const Matrix Look = LookMatrixYup(Direction);

        // the position is re-read after the call, and it is the camera's OWN -- the
        // docking camera never moves, it only turns.
        const float* const HoldPosition = AsCamera(Base)->GetPosition();

        // twelve dwords in one run into the world Csys, then the view cache is
        // invalidated.
        memcpy(Base->World, &Look.E[0][0], 9 * sizeof(float));
        Base->WorldPos[0] = HoldPosition[0];
        Base->WorldPos[1] = HoldPosition[1];
        Base->WorldPos[2] = HoldPosition[2];
        Base->bViewDirty = 1;
    }

    // the docking fade. Nothing happens until the sequence has been armed.
    if (!Cam->bFadePending)
        return;

    // the elapsed counter takes the frame time. FST stores the float
    // but leaves the sum on the FPU, so the subtraction below uses the unrounded sum.
    const double ElapsedSum =
        static_cast<double>(DeltaSeconds) + static_cast<double>(Cam->ElapsedSeconds);
    Cam->ElapsedSeconds = static_cast<float>(ElapsedSum);

    // and once the REMAINING time has dropped below the fade's own
    // duration, the fade starts -- so it finishes exactly as the docking sequence does.
    // FCOMP / TEST AH,0x5 / JP: skip unless C0 alone is set, i.e. only on an ordered "less".
    const double Remaining = static_cast<double>(Cam->TotalSeconds) - ElapsedSum;
    if (!(Remaining < static_cast<double>(GetFadeDuration())))
        return;

    // disarmed first, so the fade is started exactly once.
    Cam->bFadePending = 0;
    FadeOut();
}

void DockingCameraBeginSequence(FDockingCamera* Cam, float Seconds)
{
    // .
    Cam->bFadePending = 1;
    Cam->TotalSeconds = Seconds;
    Cam->ElapsedSeconds = kDockingElapsedSeed;
}

void FDockingCamera::Attach(void* Object)
{
    DockingCameraAttach(this, Object);
}

bool FDockingCamera::IsFinished()
{
    return false;
}

bool FDockingCamera::Activate(int const* ViewRect4)
{
    return DockingCameraActivate(this, ViewRect4);
}

void FDockingCamera::Deactivate()
{
    DockingCameraDeactivate(this);
}

void FDockingCamera::Update(float DeltaSeconds)
{
    DockingCameraUpdate(this, DeltaSeconds);
}
