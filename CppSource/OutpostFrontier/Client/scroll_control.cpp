// Author: Giperion.

#include "scroll_control.h"

#include "game_options.h"      // GetGameOption (the routine)
#include "globals.h"           // GetClientElapsedTime (the routine)
#include "hud_element_list.h"  // HudVec3ScaleByInverse (the routine)
#include "hud_target.h"        // HudTransformPoint (the routine)
#include "input_manager.h"     // GetClientSoftwareCursor (the field / the field)
#include "nn_ui_pulse.h"       // G_NnPaletteArgb_679bb0
#include "space_camera.h"      // AsCamera, FSpaceCamera
#include "star_sphere.h"       // Win3DIdentityRotation3x3 (the routine)
#include "win3d.h"
#include "win3d_engine_submit.h" // SubmitWin3DEngineInstance, Win3DRayHitsAabbHalfExtents
#include "win3d_immediate.h"   // Win3DDisable (the routine)
#include "win_factory.h"

#include "common_csys.h"       // Acquire3DMathEngine (the routine)
#include "common_ini.h"
#include "NeuronRuntime.h"
#include "engine/sceneruntime.h"
#include "x86math.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

// the branch builds it, the branch (atexit) runs the routine on it.
WINFACTORY<ScrollControl> G_ScrollControlFactory("ScrollControl");

unsigned char G_ScrollControlDragging = 0;

namespace
{
    // this TU's 3DMathEngine, the routine on first use (0x00580a33, 0x00580a8d,
    // , 0x00581235, 0x00582df5).
    I3DMathEngine* G_ScrollControlMath = nullptr;

    I3DMathEngine* ScrollControlMath()
    {
        if (G_ScrollControlMath == nullptr)
            G_ScrollControlMath = Acquire3DMathEngine();
        return G_ScrollControlMath;
    }

    // on the rotation, then the three translation dwords cleared -- the transform
    // every ScrollControl field starts as, and what a "<part>_hp" key resets before asking the
    // parent for the hardpoint.
    void SetIdentityTransform(FTransform* Xform)
    {
        Win3DIdentityRotation3x3(&Xform->Rotation.D[0][0]);
        Xform->Translation.Z = 0.0f;
        Xform->Translation.Y = 0.0f;
        Xform->Translation.X = 0.0f;
    }

    // The `CMP [holder+4],-1` pair every slot inlines: NeuronRuntime::Engine vt+0x9c SetInstanceMatrix,
    // then vt+0x6c UpdateInstance(instance, 0, 0).
    void PushInstanceMatrix(FClientMeshHolder* Holder, FTransform* Xform)
    {
        if (!ClientMeshHolderHasInstance(Holder))
            return;
        NeuronRuntime::Engine->SetInstanceMatrix(Holder->EngineInstance, Xform);
        NeuronRuntime::Engine->UpdateInstance(Holder->EngineInstance, 0, nullptr);
    }

    // The same with vt+0x8c SetInstancePosition (the "<part>_offset" keys).
    void PushInstancePosition(FClientMeshHolder* Holder, FVector* Position)
    {
        if (!ClientMeshHolderHasInstance(Holder))
            return;
        NeuronRuntime::Engine->SetInstancePosition(Holder->EngineInstance, Position);
        NeuronRuntime::Engine->UpdateInstance(Holder->EngineInstance, 0, nullptr);
    }

    // The "<part>_mesh" sequence: release the holder (the routine), load Path when it is not
    // empty (the routine(Path, 0, 0)), push Xform to a bound instance, then slot 35.
    void LoadPartMesh(ScrollControl* Control, FClientMeshHolder* Holder, char const* Path,
                      FTransform* Xform)
    {
        ClientUiMeshHolderRelease(Holder);
        if (Path[0] != '\0')
            ClientMeshLoadFromPath(Holder, Path, 0, false);
        PushInstanceMatrix(Holder, Xform);
        Control->RefreshAttachment();
    }

    // The "<part>_hp" sequence with a non-null Parent: Parent->AsWinControl() (slot 2), and when
    // that is a WinControl, its slot 36 GetHardpointTransform(Name, Xform).
    void AskHardpoint(Win3D* Parent, char const* Name, FTransform* Xform)
    {
        WinControl* const Owner = Parent->AsWinControl();
        if (Owner == nullptr)
            return;
        Owner->GetHardpointTransform(Name, reinterpret_cast<float*>(Xform));
    }

    // The three floats of a vector key, read in baseline's order (index 1, 0, 2).
    void ReadVector(INI_Reader* Reader, FVector* Out)
    {
        float const Y = Reader->get_value_float(1);
        float const X = Reader->get_value_float(0);
        float const Z = Reader->get_value_float(2);
        Out->X = X;
        Out->Y = Y;
        Out->Z = Z;
    }

    // A flag key: get_value_float(0) != 0.0 (FCOMP against the field, TEST AH,0x44 / JNP).
    unsigned char ReadFlag(INI_Reader* Reader)
    {
        return (static_cast<double>(Reader->get_value_float(0)) != 0.0) ? 1 : 0;
    }

    // (cdecl X) -- the bit-trick reciprocal square root with one Newton step,
    // (1.5 - Y * Y * X * 0.5) * Y, left in ST0 (the port's double).
    double FastInvSqrt(float X)
    {
        int Bits = 0;
        memcpy(&Bits, &X, sizeof(Bits));
        Bits = 0x5f3759df - (Bits >> 1);
        float Y = 0.0f;
        memcpy(&Y, &Bits, sizeof(Y));
        return (1.5 - static_cast<double>(Y) * Y * X * 0.5) * Y;
    }

    // The same step inlined with the squared length kept on the FPU stack (FST float for the
    // bits only): ((1.5 - Y * Y * Len2 * 0.5) * Y) * Len2, stored as a float.
    float FastLengthExtended(double Len2)
    {
        float const Len2Float = static_cast<float>(Len2);
        int Bits = 0;
        memcpy(&Bits, &Len2Float, sizeof(Bits));
        Bits = 0x5f3759df - (Bits >> 1);
        float Y = 0.0f;
        memcpy(&Y, &Bits, sizeof(Y));
        return static_cast<float>((1.5 - static_cast<double>(Y) * Y * Len2 * 0.5) * Y * Len2);
    }

    // The the routine form: the squared length stored as a float first, the result times that
    // float, stored as a float.
    float FastLengthStored(double Len2)
    {
        float const Len2Float = static_cast<float>(Len2);
        return static_cast<float>(FastInvSqrt(Len2Float) * Len2Float);
    }

    // z * z + y * y + x * x in that order (every call site here sums from z).
    double SquaredLengthZyx(FVector const& V)
    {
        return (static_cast<double>(V.Z) * V.Z + static_cast<double>(V.Y) * V.Y) +
               static_cast<double>(V.X) * V.X;
    }

    // (cdecl Out, A, B) -- Out = A - B, each a float.
    FVector Subtract(FVector const& A, FVector const& B)
    {
        FVector Out;
        Out.X = A.X - B.X;
        Out.Y = A.Y - B.Y;
        Out.Z = A.Z - B.Z;
        return Out;
    }

    // (cdecl Out, A, B) -- Out = A + B, each a float.
    FVector Add(FVector const& A, FVector const& B)
    {
        FVector Out;
        Out.X = A.X + B.X;
        Out.Y = A.Y + B.Y;
        Out.Z = A.Z + B.Z;
        return Out;
    }

    // Win3D camera the field, slot 0x34 Project(&x, &y, &z, world); TEST AL,AL.
    bool ProjectToScreen(FVector const& World, FVector* OutScreen)
    {
        ICamera* const Camera = AsCamera(Win3DGetCamera());
        return static_cast<unsigned char>(
                   Camera->Project(&OutScreen->X, &OutScreen->Y, &OutScreen->Z, &World.X)) != 0;
    }

    // (thiscall on the half extents, RET 8).
    bool RayHits(FVector const& HalfExtents, FTransform const& Xform, float const* Ray)
    {
        return Win3DRayHitsAabbHalfExtents(&HalfExtents.X,
                                           reinterpret_cast<float const*>(&Xform), Ray);
    }

    // The colour a part is drawn with: the byte quad into holder +0x14 / +0x18 / +0x1c / +0x24
    // (FILD, FMUL 1/255 the field, FSTP float -- the routine's arithmetic, inlined in
    // every Draw block), the routine(camera, 0, 1.0f, 0), then the routine(GL_DEPTH_TEST).
    void DrawPartMesh(FClientMeshHolder* Holder, unsigned long const* Rgba)
    {
        ClientMeshHolderSetColor(Holder, reinterpret_cast<unsigned char const*>(Rgba));
        SubmitWin3DEngineInstance(Holder, Win3DGetCamera(), 0, 1.0f, 0);
        Win3DDisable(kWin3DDepthTest);
    }

    int CursorPixelX()
    {
        int X = 0;
        int Y = 0;
        GetClientSoftwareCursor(&X, &Y, nullptr);
        return X;
    }

    int CursorPixelY()
    {
        int X = 0;
        int Y = 0;
        GetClientSoftwareCursor(&X, &Y, nullptr);
        return Y;
    }
}

ScrollControl::ScrollControl()
{
    // . WinControl's ctor the routine runs as the base. Every holder is the routine
    // + the routine on +0x14 + vftable 0x005d2efc + anim -1; every transform the routine with
    // a zero translation. The body then sets the working transforms (+0x360, +0x404, +0x4a8,
    // +0x540, +0x570, +0x5b8) to identity a second time -- same values, not repeated here.
    ClientMeshHolderConstruct(&Back.Holder);
    SetIdentityTransform(&Back.Xform);
    SetIdentityTransform(&Back.Hp);
    ClientMeshHolderConstruct(&Next.Holder);
    SetIdentityTransform(&Next.Xform);
    SetIdentityTransform(&Next.Hp);
    ClientMeshHolderConstruct(&Prev.Holder);
    SetIdentityTransform(&Prev.Xform);
    SetIdentityTransform(&Prev.Hp);
    ClientMeshHolderConstruct(&ThumbHolder);
    SetIdentityTransform(&ThumbStart);
    SetIdentityTransform(&ThumbEnd);
    SetIdentityTransform(&ThumbXform);
    SetIdentityTransform(&ThumbHp0);
    SetIdentityTransform(&ThumbHp1);

    // the vectors.
    Back.Offset = {0.0f, 0.0f, 0.0f};
    Back.MouseSize = {0.0f, 0.0f, 0.0f};
    Next.Offset = {0.0f, 0.0f, 0.0f};
    Next.MouseSize = {0.0f, 0.0f, 0.0f};
    Prev.Offset = {0.0f, 0.0f, 0.0f};
    Prev.MouseSize = {0.0f, 0.0f, 0.0f};
    ThumbOffset0 = {0.0f, 0.0f, 0.0f};
    ThumbOffset1 = {0.0f, 0.0f, 0.0f};
    ThumbMouse = {0.0f, 0.0f, 0.0f};

    // / 0x00580002 / 0x0058004b / 0x00580104: no part skips its mesh.
    Back.bNoMeshRender = 0;
    Next.bNoMeshRender = 0;
    Prev.bNoMeshRender = 0;
    bThumbNoMeshRender = 0;

    // . RepeatTime (+0x670) and bPageTowardMin (+0x67b) are not written.
    PageSize = 1;
    Step = 1;
    MinRange = 0;
    MaxRange = 0;
    Value = 0;
    bSliderBehavior = 0;
    bNoZEnable = 0;
    bThumbValid = 0;
    bDragging = 0;
    bPaging = 0;
    bNextPressed = 0;
    bPrevPressed = 0;
    bThumbHover = 0;
    bNextHover = 0;
    bPrevHover = 0;
}

ScrollControl::~ScrollControl()
{
    // the routine on the back, prev, next and thumb holders (in that order).
    ClientUiMeshHolderRelease(&Back.Holder);
    ClientUiMeshHolderRelease(&Prev.Holder);
    ClientUiMeshHolderRelease(&Next.Holder);
    ClientUiMeshHolderRelease(&ThumbHolder);

    // a held thumb, back plate or button drops the capture.
    if (bDragging != 0 || bPaging != 0 || bNextPressed != 0 || bPrevPressed != 0)
    {
        Win3DRemoveCapture(this);
        G_ScrollControlDragging = 0;
    }

    // the holders' own dtors the routine (the same release, which
    // finds nothing now) in reverse order -- thumb, prev, next, back: ~FClientMeshHolder in
    // reverse declaration order -- then WinControl's dtor the routine.
}

void ScrollControl::UpdateThumb()
{
    // . Min < Max, Min <= Value, Max >= Value (JGE / JG / JL to 0x005803b3).
    int const Min = MinRange;
    int const Max = MaxRange;
    if (Min >= Max || Min > Value || Max < Value)
    {
        bThumbValid = 0;
        return;
    }

    // FILD (Value - Min) / FIDIV (Max - Min), kept on the FPU stack.
    double const T = static_cast<double>(Value - Min) / static_cast<double>(Max - Min);
    bThumbValid = 1;
    ThumbXform = ThumbStart;

    // the translation between the two ends. dz is stored as a float
    // before its product, dx * t is stored as a float, dy * t stays on the stack.
    double const Dx = static_cast<double>(ThumbEnd.Translation.X) - ThumbStart.Translation.X;
    double const Dy = static_cast<double>(ThumbEnd.Translation.Y) - ThumbStart.Translation.Y;
    float const Dz = static_cast<float>(static_cast<double>(ThumbEnd.Translation.Z) -
                                        ThumbStart.Translation.Z);
    float const Dxt = static_cast<float>(Dx * T);
    float const Dzt = static_cast<float>(static_cast<double>(Dz) * T);
    ThumbXform.Translation.X = static_cast<float>(static_cast<double>(Dxt) +
                                                  ThumbStart.Translation.X);
    ThumbXform.Translation.Y = static_cast<float>(Dy * T + ThumbStart.Translation.Y);
    ThumbXform.Translation.Z = static_cast<float>(static_cast<double>(Dzt) +
                                                  ThumbStart.Translation.Z);

    // .
    PushInstanceMatrix(&ThumbHolder, &ThumbXform);
}

void ScrollControl::UpdateLayout()
{
    // . With a WinControl parent (+4, slot 2) the working transforms are rebuilt:
    // each is its hardpoint transform moved by (offset + position) + WinControl::Offset (+0xa8).
    if (Parent != nullptr && Parent->AsWinControl() != nullptr)
    {
        FVector const Position = {Win3D::Position[0], Win3D::Position[1], Win3D::Position[2]};
        FVector const ControlOffset = {Offset[0], Offset[1], Offset[2]};

        // back.
        Back.Xform = Back.Hp;
        FVector Moved = Add(Add(Back.Offset, Position), ControlOffset);
        Back.Xform.Translation.X += Moved.X;
        Back.Xform.Translation.Y += Moved.Y;
        Back.Xform.Translation.Z += Moved.Z;

        // next.
        Next.Xform = Next.Hp;
        Moved = Add(Add(Next.Offset, Position), ControlOffset);
        Next.Xform.Translation.X += Moved.X;
        Next.Xform.Translation.Y += Moved.Y;
        Next.Xform.Translation.Z += Moved.Z;

        // prev (the second sum through the routine).
        Prev.Xform = Prev.Hp;
        Moved = Add(Add(Prev.Offset, Position), ControlOffset);
        Prev.Xform.Translation.X += Moved.X;
        Prev.Xform.Translation.Y += Moved.Y;
        Prev.Xform.Translation.Z += Moved.Z;

        // the thumb's start (position + offset0, then + offset).
        ThumbStart = ThumbHp0;
        Moved = Add(Add(Position, ThumbOffset0), ControlOffset);
        ThumbStart.Translation.X += Moved.X;
        ThumbStart.Translation.Y += Moved.Y;
        ThumbStart.Translation.Z += Moved.Z;

        // the thumb's end.
        ThumbEnd = ThumbHp1;
        Moved = Add(Add(ThumbOffset1, Position), ControlOffset);
        ThumbEnd.Translation.X += Moved.X;
        ThumbEnd.Translation.Y += Moved.Y;
        ThumbEnd.Translation.Z += Moved.Z;
    }

    // the three buttons to the engine, then the thumb.
    PushInstanceMatrix(&Back.Holder, &Back.Xform);
    PushInstanceMatrix(&Next.Holder, &Next.Xform);
    PushInstanceMatrix(&Prev.Holder, &Prev.Xform);
    UpdateThumb();
}

void ScrollControl::ChoosePageDirection(unsigned ScreenX, unsigned ScreenY)
{
    // (RET 8). At either end of the range the answer is fixed first.
    if (Value == MinRange)
        bPageTowardMin = 0;
    else if (Value == MaxRange)
        bPageTowardMin = 1;

    // FILD qword {coordinate, 0}: the pixel as an unsigned integer.
    FVector const Mouse = {static_cast<float>(ScreenX), static_cast<float>(ScreenY), 0.0f};
    FVector const Zero = {0.0f, 0.0f, 0.0f};

    int const ToMax = abs(MaxRange - Value);
    int const ToMin = abs(MinRange - Value);
    if (ToMax < ToMin)
    {
        // nearer the top. Is the cursor, seen from the thumb's start,
        // at least as far along the start->thumb line as the thumb itself?
        FVector Current;
        FVector Start;
        HudTransformPoint(&ThumbXform, &Current, &Zero);
        HudTransformPoint(&ThumbStart, &Start, &Zero);
        FVector CurrentScreen;
        FVector StartScreen;
        if (!ProjectToScreen(Current, &CurrentScreen) || !ProjectToScreen(Start, &StartScreen))
        {
            bPageTowardMin = 1;
            return;
        }
        FVector const Track = Subtract(CurrentScreen, StartScreen);
        float const TrackLength = FastLengthExtended(SquaredLengthZyx(Track));
        FVector const ToMouse = Subtract(Mouse, StartScreen);
        float const MouseLength = FastLengthExtended(SquaredLengthZyx(ToMouse));
        if (TrackLength == 0.0f || MouseLength == 0.0f)
        {
            bPageTowardMin = 1;
            return;
        }
        float TrackDir[3];
        float MouseDir[3];
        HudVec3ScaleByInverse(TrackDir, &Track.X, TrackLength);
        HudVec3ScaleByInverse(MouseDir, &ToMouse.X, MouseLength);
        double const Dot = (static_cast<double>(MouseDir[2]) * TrackDir[2] +
                            static_cast<double>(MouseDir[1]) * TrackDir[1]) +
                           static_cast<double>(MouseDir[0]) * TrackDir[0];
        // TEST AH,5 / JNP: below -> 1, otherwise (and unordered) -> 0.
        bPageTowardMin = (Dot * MouseLength < TrackLength) ? 1 : 0;
        return;
    }

    // nearer the bottom (or halfway). The same test from the thumb's
    // end, through this TU's math engine.
    FVector Current;
    FVector End;
    ScrollControlMath()->TransformXform(Current, ThumbXform, Zero);
    ScrollControlMath()->TransformXform(End, ThumbEnd, Zero);
    FVector CurrentScreen;
    FVector EndScreen;
    if (!ProjectToScreen(Current, &CurrentScreen) || !ProjectToScreen(End, &EndScreen))
    {
        bPageTowardMin = 1;
        return;
    }
    FVector const Track = Subtract(CurrentScreen, EndScreen);
    float const TrackLength = FastLengthExtended(SquaredLengthZyx(Track));
    // z is FCHS of the end's z (the cursor's z is 0).
    FVector ToMouse;
    ToMouse.X = Mouse.X - EndScreen.X;
    ToMouse.Y = Mouse.Y - EndScreen.Y;
    ToMouse.Z = -EndScreen.Z;
    float const MouseLength = FastLengthExtended(SquaredLengthZyx(ToMouse));
    if (TrackLength == 0.0f || MouseLength == 0.0f)
    {
        bPageTowardMin = 1;
        return;
    }
    float TrackDir[3];
    HudVec3ScaleByInverse(TrackDir, &Track.X, TrackLength);
    // 1.0 / length kept on the stack, the products stored as floats.
    double const InvMouse = 1.0 / static_cast<double>(MouseLength);
    float const MouseDir[3] = {static_cast<float>(ToMouse.X * InvMouse),
                               static_cast<float>(ToMouse.Y * InvMouse),
                               static_cast<float>(ToMouse.Z * InvMouse)};
    double const Dot = (static_cast<double>(TrackDir[0]) * MouseDir[0] +
                        static_cast<double>(MouseDir[1]) * TrackDir[1]) +
                       static_cast<double>(MouseDir[2]) * TrackDir[2];
    // TEST AH,5 / JP: below -> 0, otherwise (and unordered) -> 1.
    bPageTowardMin = (Dot * MouseLength < TrackLength) ? 0 : 1;
}

void ScrollControl::PageStep()
{
    // . The cursor is read straight from the field / the field (FILD dword).
    FVector const Zero = {0.0f, 0.0f, 0.0f};
    FVector const Mouse = {static_cast<float>(CursorPixelX()), static_cast<float>(CursorPixelY()),
                           0.0f};

    if (bPageTowardMin != 0)
    {
        // towards the start while the cursor lies short of the thumb.
        FVector Current;
        FVector Start;
        HudTransformPoint(&ThumbXform, &Current, &Zero);
        HudTransformPoint(&ThumbStart, &Start, &Zero);
        FVector CurrentScreen;
        FVector StartScreen;
        if (!ProjectToScreen(Current, &CurrentScreen))
            return;
        if (!ProjectToScreen(Start, &StartScreen))
            return;
        FVector const Track = Subtract(CurrentScreen, StartScreen);
        float const TrackLength = FastLengthStored(SquaredLengthZyx(Track));
        FVector const ToMouse = Subtract(Mouse, StartScreen);
        float const MouseLength = FastLengthStored(SquaredLengthZyx(ToMouse));
        if (TrackLength == 0.0f || MouseLength == 0.0f)
            return;
        float TrackDir[3];
        float MouseDir[3];
        HudVec3ScaleByInverse(TrackDir, &Track.X, TrackLength);
        HudVec3ScaleByInverse(MouseDir, &ToMouse.X, MouseLength);
        double const Dot = (static_cast<double>(TrackDir[0]) * MouseDir[0] +
                            static_cast<double>(TrackDir[1]) * MouseDir[1]) +
                           static_cast<double>(TrackDir[2]) * MouseDir[2];
        // TEST AH,5 / JP: only strictly below goes on.
        if (!(Dot * MouseLength < TrackLength))
            return;
        Value -= PageSize;
        if (Value < MinRange)
            Value = MinRange;
    }
    else
    {
        // towards the end.
        FVector Current;
        FVector End;
        HudTransformPoint(&ThumbXform, &Current, &Zero);
        HudTransformPoint(&ThumbEnd, &End, &Zero);
        FVector CurrentScreen;
        FVector EndScreen;
        if (!ProjectToScreen(Current, &CurrentScreen))
            return;
        if (!ProjectToScreen(End, &EndScreen))
            return;
        FVector const Track = Subtract(CurrentScreen, EndScreen);
        float const TrackLength = FastLengthStored(SquaredLengthZyx(Track));
        FVector const ToMouse = Subtract(Mouse, EndScreen);
        float const MouseLength = FastLengthExtended(SquaredLengthZyx(ToMouse));
        if (TrackLength == 0.0f || MouseLength == 0.0f)
            return;
        float TrackDir[3];
        float MouseDir[3];
        HudVec3ScaleByInverse(TrackDir, &Track.X, TrackLength);
        HudVec3ScaleByInverse(MouseDir, &ToMouse.X, MouseLength);
        double const Dot = (static_cast<double>(TrackDir[0]) * MouseDir[0] +
                            static_cast<double>(TrackDir[1]) * MouseDir[1]) +
                           static_cast<double>(TrackDir[2]) * MouseDir[2];
        if (!(Dot * MouseLength < TrackLength))
            return;
        Value += PageSize;
        if (Value > MaxRange)
            Value = MaxRange;
    }

    // .
    UpdateThumb();
    NotifyValue();
}

void ScrollControl::DragThumb(unsigned ScreenX, unsigned ScreenY)
{
    // (RET 8). The cursor projected on the start->end track of the thumb, as a
    // fraction of the range.
    FVector const Zero = {0.0f, 0.0f, 0.0f};
    FVector Start;
    FVector End;
    ScrollControlMath()->TransformXform(Start, ThumbStart, Zero);
    ScrollControlMath()->TransformXform(End, ThumbEnd, Zero);
    FVector StartScreen;
    FVector EndScreen;
    if (!ProjectToScreen(Start, &StartScreen))
        return;
    if (!ProjectToScreen(End, &EndScreen))
        return;

    FVector const Mouse = {static_cast<float>(ScreenX), static_cast<float>(ScreenY), 0.0f};
    FVector const Track = Subtract(EndScreen, StartScreen);
    float const TrackLength = FastLengthExtended(SquaredLengthZyx(Track));
    FVector const ToMouse = Subtract(Mouse, StartScreen);
    float const MouseLength = FastLengthExtended(SquaredLengthZyx(ToMouse));
    if (TrackLength == 0.0f || MouseLength == 0.0f)
        return;

    // both directions through a reciprocal kept on the stack; the
    // track's reciprocal is also stored as a float and that float is used again below.
    double const InvTrack = 1.0 / static_cast<double>(TrackLength);
    float const InvTrackFloat = static_cast<float>(InvTrack);
    float const TrackDir[3] = {static_cast<float>(Track.X * InvTrack),
                               static_cast<float>(Track.Y * InvTrack),
                               static_cast<float>(Track.Z * InvTrack)};
    double const InvMouse = 1.0 / static_cast<double>(MouseLength);
    float const MouseDir[3] = {static_cast<float>(ToMouse.X * InvMouse),
                               static_cast<float>(ToMouse.Y * InvMouse),
                               static_cast<float>(ToMouse.Z * InvMouse)};
    double const Dot = (static_cast<double>(TrackDir[0]) * MouseDir[0] +
                        static_cast<double>(TrackDir[1]) * MouseDir[1]) +
                       static_cast<double>(TrackDir[2]) * MouseDir[2];

    // floor(dot * |mouse| * (1 / |track|) * range + 0.5) + min.
    int const Min = MinRange;
    int const Range = MaxRange - Min;
    double const Scaled = Dot * MouseLength * InvTrackFloat * Range + 0.5;
    Value = static_cast<int>(floor(Scaled)) + Min;
    if (Value > MaxRange)
        Value = MaxRange;
    if (Value < Min)
        Value = Min;

    // the routine, then the routine's body inlined.
    UpdateThumb();
    NotifyValue();
}

void ScrollControl::NotifyValue()
{
    // the value first (pushed as the third argument), then this->AsWinControl().
    if (Parent == nullptr)
        return;
    WinControl* const Owner = Parent->AsWinControl();
    if (Owner == nullptr)
        return;
    int const Current = Value;
    Owner->SetProperty(0x3e, reinterpret_cast<uintptr_t>(AsWinControl()),
                       static_cast<uintptr_t>(Current));
}

bool ScrollControl::Create(Win3D* NewParent, void* Data)
{
    // (RET 8): Win3D::Create's body inlined (the routine, the routine, flags),
    // then the [ScrollControl] keys from the INI_Reader in Data.
    Win3DReparent(this, NewParent);
    Win3DRegister(this);
    Flags = static_cast<unsigned char>((Flags & 0xfb) | kWin3DActive | kWin3DVisible);

    INI_Reader* const Reader = static_cast<INI_Reader*>(Data);
    if (Reader != nullptr)
    {
        while (Reader->read_value())
        {
            if (Reader->is_value("back_mesh"))
            {
                LoadPartMesh(this, &Back.Holder, Reader->get_value_string(), &Back.Xform);
            }
            else if (Reader->is_value("back_offset"))
            {
                ReadVector(Reader, &Back.Offset);
                UpdateLayout();
                PushInstancePosition(&Back.Holder, &Back.Xform.Translation);
            }
            else if (Reader->is_value("back_hp"))
            {
                // The hardpoint is asked of the Create argument, not of the linked parent.
                SetIdentityTransform(&Back.Hp);
                if (NewParent != nullptr)
                    AskHardpoint(NewParent, Reader->get_value_string(), &Back.Hp);
            }
            else if (Reader->is_value("back_mouse"))
            {
                ReadVector(Reader, &Back.MouseSize);
            }
            else if (Reader->is_value("back_no_mesh_render"))
            {
                Back.bNoMeshRender = ReadFlag(Reader);
            }
            else if (Reader->is_value("nextb_mesh"))
            {
                LoadPartMesh(this, &Next.Holder, Reader->get_value_string(), &Next.Xform);
            }
            else if (Reader->is_value("nextb_offset"))
            {
                ReadVector(Reader, &Next.Offset);
                UpdateLayout();
                PushInstancePosition(&Next.Holder, &Next.Xform.Translation);
            }
            else if (Reader->is_value("nextb_hp"))
            {
                SetIdentityTransform(&Next.Hp);
                if (NewParent != nullptr)
                    AskHardpoint(NewParent, Reader->get_value_string(), &Next.Hp);
            }
            else if (Reader->is_value("nextb_mouse"))
            {
                ReadVector(Reader, &Next.MouseSize);
            }
            else if (Reader->is_value("nextb_no_mesh_render"))
            {
                Next.bNoMeshRender = ReadFlag(Reader);
            }
            else if (Reader->is_value("prevb_mesh"))
            {
                LoadPartMesh(this, &Prev.Holder, Reader->get_value_string(), &Prev.Xform);
            }
            else if (Reader->is_value("prevb_offset"))
            {
                ReadVector(Reader, &Prev.Offset);
                UpdateLayout();
                PushInstancePosition(&Prev.Holder, &Prev.Xform.Translation);
            }
            else if (Reader->is_value("prevb_hp"))
            {
                SetIdentityTransform(&Prev.Hp);
                if (NewParent != nullptr)
                    AskHardpoint(NewParent, Reader->get_value_string(), &Prev.Hp);
            }
            else if (Reader->is_value("prevb_mouse"))
            {
                ReadVector(Reader, &Prev.MouseSize);
            }
            else if (Reader->is_value("prevb_no_mesh_render"))
            {
                Prev.bNoMeshRender = ReadFlag(Reader);
            }
            else if (Reader->is_value("thumb_mesh"))
            {
                LoadPartMesh(this, &ThumbHolder, Reader->get_value_string(), &ThumbXform);
            }
            else if (Reader->is_value("thumb_offset0"))
            {
                ReadVector(Reader, &ThumbOffset0);
                UpdateLayout();
                PushInstancePosition(&ThumbHolder, &ThumbXform.Translation);
            }
            else if (Reader->is_value("thumb_offset1"))
            {
                ReadVector(Reader, &ThumbOffset1);
                UpdateLayout();
                PushInstancePosition(&ThumbHolder, &ThumbXform.Translation);
            }
            else if (Reader->is_value("thumb_hp0"))
            {
                SetIdentityTransform(&ThumbHp0);
                if (NewParent != nullptr)
                    AskHardpoint(NewParent, Reader->get_value_string(), &ThumbHp0);
            }
            else if (Reader->is_value("thumb_hp1"))
            {
                SetIdentityTransform(&ThumbHp1);
                if (NewParent != nullptr)
                    AskHardpoint(NewParent, Reader->get_value_string(), &ThumbHp1);
            }
            else if (Reader->is_value("thumb_mouse"))
            {
                ReadVector(Reader, &ThumbMouse);
            }
            else if (Reader->is_value("thumb_no_mesh_render"))
            {
                bThumbNoMeshRender = ReadFlag(Reader);
            }
            else if (Reader->is_value("min_range"))
            {
                MinRange = static_cast<int>(Reader->get_value_float(0)); // ftol
            }
            else if (Reader->is_value("max_range"))
            {
                MaxRange = static_cast<int>(Reader->get_value_float(0));
            }
            else if (Reader->is_value("page_size"))
            {
                PageSize = static_cast<int>(Reader->get_value_float(0));
            }
            else if (Reader->is_value("slider_behavior"))
            {
                bSliderBehavior = ReadFlag(Reader);
            }
            else if (Reader->is_value("no_z_enable"))
            {
                bNoZEnable = ReadFlag(Reader);
            }
        }
    }

    // the linked parent re-applies its own position (slot 5).
    Win3D* const LinkedParent = Parent;
    if (LinkedParent != nullptr)
        LinkedParent->SetPosition(LinkedParent->Position);
    return true;
}

void ScrollControl::Init(char const* BackMesh, float const* BackOffset, char const* BackHp,
                         float const* BackMouse, bool bBackNoMeshRender, char const* NextMesh,
                         float const* NextOffset, char const* NextHp, float const* NextMouse,
                         bool bNextNoMeshRender, char const* PrevMesh, float const* PrevOffset,
                         char const* PrevHp, float const* PrevMouse, bool bPrevNoMeshRender,
                         char const* ThumbMesh, float const* NewThumbOffset0,
                         float const* NewThumbOffset1, char const* NewThumbHp0,
                         char const* NewThumbHp1, float const* NewThumbMouse,
                         bool bNewThumbNoMeshRender, int NewMinRange, int NewMaxRange,
                         int NewPageSize, bool bNewSliderBehavior, bool bNewNoZEnable)
{
    // (RET 0x6c). Hardpoints here are asked of the linked parent (+4).

    // back.
    if (BackMesh != nullptr)
        LoadPartMesh(this, &Back.Holder, BackMesh, &Back.Xform);
    Back.Offset = {BackOffset[0], BackOffset[1], BackOffset[2]};
    UpdateLayout();
    PushInstancePosition(&Back.Holder, &Back.Xform.Translation);
    if (BackHp != nullptr)
    {
        SetIdentityTransform(&Back.Hp);
        if (Parent != nullptr)
            AskHardpoint(Parent, BackHp, &Back.Hp);
    }
    Back.MouseSize = {BackMouse[0], BackMouse[1], BackMouse[2]};
    Back.bNoMeshRender = bBackNoMeshRender ? 1 : 0;

    // next.
    if (NextMesh != nullptr)
        LoadPartMesh(this, &Next.Holder, NextMesh, &Next.Xform);
    Next.Offset = {NextOffset[0], NextOffset[1], NextOffset[2]};
    UpdateLayout();
    PushInstancePosition(&Next.Holder, &Next.Xform.Translation);
    if (NextHp != nullptr)
    {
        SetIdentityTransform(&Next.Hp);
        if (Parent != nullptr)
            AskHardpoint(Parent, NextHp, &Next.Hp);
    }
    Next.MouseSize = {NextMouse[0], NextMouse[1], NextMouse[2]};
    Next.bNoMeshRender = bNextNoMeshRender ? 1 : 0;

    // prev -- all of it only with a mesh name (JZ 0x005824bf).
    if (PrevMesh != nullptr)
    {
        LoadPartMesh(this, &Prev.Holder, PrevMesh, &Prev.Xform);
        Prev.Offset = {PrevOffset[0], PrevOffset[1], PrevOffset[2]};
        UpdateLayout();
        PushInstancePosition(&Prev.Holder, &Prev.Xform.Translation);
        if (PrevHp != nullptr)
        {
            SetIdentityTransform(&Prev.Hp);
            if (Parent != nullptr)
                AskHardpoint(Parent, PrevHp, &Prev.Hp);
        }
        Prev.MouseSize = {PrevMouse[0], PrevMouse[1], PrevMouse[2]};
        Prev.bNoMeshRender = bPrevNoMeshRender ? 1 : 0;
    }

    // the thumb.
    if (ThumbMesh != nullptr)
        LoadPartMesh(this, &ThumbHolder, ThumbMesh, &ThumbXform);
    ThumbOffset0 = {NewThumbOffset0[0], NewThumbOffset0[1], NewThumbOffset0[2]};
    UpdateLayout();
    PushInstancePosition(&ThumbHolder, &ThumbXform.Translation);
    ThumbOffset1 = {NewThumbOffset1[0], NewThumbOffset1[1], NewThumbOffset1[2]};
    UpdateLayout();
    PushInstancePosition(&ThumbHolder, &ThumbXform.Translation);
    if (NewThumbHp0 != nullptr)
    {
        SetIdentityTransform(&ThumbHp0);
        if (Parent != nullptr)
            AskHardpoint(Parent, NewThumbHp0, &ThumbHp0);
    }
    if (NewThumbHp1 != nullptr)
    {
        SetIdentityTransform(&ThumbHp1);
        if (Parent != nullptr)
            AskHardpoint(Parent, NewThumbHp1, &ThumbHp1);
    }

    // .
    ThumbMouse = {NewThumbMouse[0], NewThumbMouse[1], NewThumbMouse[2]};
    bThumbNoMeshRender = bNewThumbNoMeshRender ? 1 : 0;
    MinRange = NewMinRange;
    MaxRange = NewMaxRange;
    PageSize = NewPageSize;
    bSliderBehavior = bNewSliderBehavior ? 1 : 0;
    bNoZEnable = bNewNoZEnable ? 1 : 0;
}

bool ScrollControl::HitTest(float const* Ray)
{
    // any button, or a valid thumb.
    if (RayHits(Back.MouseSize, Back.Xform, Ray))
        return true;
    if (RayHits(Next.MouseSize, Next.Xform, Ray))
        return true;
    if (RayHits(Prev.MouseSize, Prev.Xform, Ray))
        return true;
    if (bThumbValid != 0 && RayHits(ThumbMouse, ThumbXform, Ray))
        return true;
    return false;
}

void ScrollControl::SetPosition(float const* NewPosition)
{
    // WinControl's SetPosition (the routine), then the layout.
    WinControl::SetPosition(NewPosition);
    UpdateLayout();
}

void ScrollControl::SetOrientation(float const* NewOrientation)
{
    // nine dwords into Win3D +0x3c (not WinControl's slot 7), then the layout.
    memcpy(Orientation, NewOrientation, sizeof(Orientation));
    UpdateLayout();
}

void ScrollControl::Update()
{
    // the children first (the routine).
    WinControl::Update();

    if (bThumbValid == 0)
    {
        // .
        Win3DRemoveCapture(this);
        G_ScrollControlDragging = 0;
        bDragging = 0;
        bPaging = 0;
        bNextPressed = 0;
        bPrevPressed = 0;
        return;
    }

    if (bDragging != 0)
    {
        // the field / the field pushed as they are.
        DragThumb(static_cast<unsigned>(CursorPixelX()), static_cast<unsigned>(CursorPixelY()));
        return;
    }

    // - +0x670 against 0.3 (the field): TEST AH,0x41 / JNZ -- only strictly
    // later goes on.
    if (bPaging != 0)
    {
        if (GetClientElapsedTime() - static_cast<double>(RepeatTime) > 0.3)
        {
            PageStep();
            RepeatTime = static_cast<float>(GetClientElapsedTime());
        }
        return;
    }

    if (bNextPressed != 0)
    {
        if (!(GetClientElapsedTime() - static_cast<double>(RepeatTime) > 0.3))
            return;
        if (Value < MaxRange)
        {
            Value += Step;
            if (Value > MaxRange)
                Value = MaxRange;
            UpdateThumb();
            NotifyValue(); // , the routine inlined
        }
        RepeatTime = static_cast<float>(GetClientElapsedTime());
        return;
    }

    if (bPrevPressed != 0)
    {
        if (!(GetClientElapsedTime() - static_cast<double>(RepeatTime) > 0.3))
            return;
        if (Value > MinRange)
        {
            Value -= Step;
            if (Value < MinRange)
                Value = MinRange;
            UpdateThumb();
            NotifyValue();
        }
        RepeatTime = static_cast<float>(GetClientElapsedTime());
    }
}

void ScrollControl::Draw()
{
    // "[INTERFACE]" (option 6) and "NEURONET" (option 9), the children, then the
    // parts -- only while the thumb is valid.
    if (GetGameOption(kGameOptInterfaceGroup) == 0)
        return;
    if (GetGameOption(kGameOptNeuronet) == 0)
        return;
    Win3DDrawChildren(this);
    if (bThumbValid == 0)
        return;

    // the idle colour the field and the hot colour
    // {0xf5, 0xea, 0x52, 0xff}, each kept twice on the stack.
    unsigned long const Idle = G_NnPaletteArgb_679bb0;
    unsigned char const HotBytes[4] = {0xf5, 0xea, 0x52, 0xff};
    unsigned long Hot = 0;
    memcpy(&Hot, HotBytes, sizeof(Hot));
    FSpaceCamera* const Camera = Win3DGetCamera();

    // the back plate -- the untinted pass unless back_no_mesh_render,
    // then depth test off and always the idle-tinted pass.
    if (ClientMeshHolderHasInstance(&Back.Holder))
    {
        if (Back.bNoMeshRender == 0)
            DrawPartMesh(&Back.Holder, &Idle);
        Win3DDisable(kWin3DDepthTest);
        unsigned long Tint = Idle;
        SubmitWin3DTintedEngineInstanceWrapped(Camera, Back.Holder.EngineInstance, 1,
                                               reinterpret_cast<unsigned char const*>(&Tint), 1,
                                               1.0f, 0);
    }

    // next -- hot while pressed or hovered; no tinted pass.
    if (ClientMeshHolderHasInstance(&Next.Holder))
    {
        unsigned long const* const Color =
            (bNextPressed != 0 || bNextHover != 0) ? &Hot : &Idle;
        if (Next.bNoMeshRender == 0)
            DrawPartMesh(&Next.Holder, Color);
        Win3DDisable(kWin3DDepthTest);
    }

    // prev.
    if (ClientMeshHolderHasInstance(&Prev.Holder))
    {
        unsigned long const* const Color =
            (bPrevPressed != 0 || bPrevHover != 0) ? &Hot : &Idle;
        if (Prev.bNoMeshRender == 0)
            DrawPartMesh(&Prev.Holder, Color);
        Win3DDisable(kWin3DDepthTest);
    }

    // the thumb -- untinted in the idle colour unless
    // thumb_no_mesh_render, then tinted hot while dragged or hovered, then depth test off.
    if (bThumbValid == 0)
        return;
    if (!ClientMeshHolderHasInstance(&ThumbHolder))
        return;
    if (bThumbNoMeshRender == 0)
        DrawPartMesh(&ThumbHolder, &Idle);
    unsigned long Tint = (bDragging != 0 || bThumbHover != 0) ? Hot : Idle;
    SubmitWin3DTintedEngineInstanceWrapped(Camera, ThumbHolder.EngineInstance, 1,
                                           reinterpret_cast<unsigned char const*>(&Tint), 1, 1.0f,
                                           0);
    Win3DDisable(kWin3DDepthTest);
}

void ScrollControl::MouseOver(float const* Ray)
{
    // MOV EAX,[ECX] / JMP [EAX+0x3c] -- slot 15.
    MouseEnter(Ray);
}

void ScrollControl::MouseEnter(float const* Ray)
{
    // (RET 4): the hover bytes; a hovered valid thumb masks both buttons.
    bNextHover = RayHits(Next.MouseSize, Next.Xform, Ray) ? 1 : 0;
    bPrevHover = RayHits(Prev.MouseSize, Prev.Xform, Ray) ? 1 : 0;
    bThumbHover = 0;
    if (bThumbValid == 0)
        return;
    bThumbHover = RayHits(ThumbMouse, ThumbXform, Ray) ? 1 : 0;
    if (bThumbHover != 0)
    {
        bNextHover = 0;
        bPrevHover = 0;
    }
}

void ScrollControl::MouseLeave(float const* Ray)
{
    // , the same tail jump into slot 15.
    MouseEnter(Ray);
}

bool ScrollControl::MouseEvent(FWin3DMouseEvent const* Event)
{
    // (RET 4). Without a valid thumb nothing is taken; with one every event is
    // (EBX stays 1 on every other exit).
    if (bThumbValid == 0)
        return false;

    // a held part lets go on {-1, 1} (the button release).
    bool const bRelease = Event->Field0 == -1 && Event->Field1 == 1;
    if (bDragging != 0)
    {
        if (bRelease)
        {
            Win3DRemoveCapture(this);
            bDragging = 0;
            G_ScrollControlDragging = 0;
        }
        return true;
    }
    if (bNextPressed != 0)
    {
        if (bRelease)
        {
            Win3DRemoveCapture(this);
            bNextPressed = 0;
        }
        return true;
    }
    if (bPrevPressed != 0)
    {
        if (bRelease)
        {
            Win3DRemoveCapture(this);
            bPrevPressed = 0;
        }
        return true;
    }
    if (bPaging != 0)
    {
        if (bRelease)
        {
            Win3DRemoveCapture(this);
            bPaging = 0;
        }
        return true;
    }

    // only {-1, 2} (the press) goes on.
    if (Event->Field0 != -1 || Event->Field1 != 2)
        return true;

    // the cursor ray -- the pixel onto the near plane of the Win3D
    // camera the field through a copy of its orientation the field (this TU's math engine,
    // slot +0x2c TransformMatrix), times 1 / FSQRT(length).
    FSpaceCamera* const Cam = Win3DGetCamera();
    int const CursorX = CursorPixelX();
    int const CursorY = CursorPixelY();
    FVector Local;
    Local.X = static_cast<float>((static_cast<double>(CursorX) - Cam->ScreenCenterX) /
                                 Cam->XScale * Cam->ZNear);
    Local.Y = static_cast<float>((static_cast<double>(CursorY) - Cam->ScreenCenterY) /
                                 Cam->YScale * Cam->ZNear);
    Local.Z = -Cam->ZNear;
    FMatrix CamOrientation;
    memcpy(&CamOrientation, Cam->World, sizeof(CamOrientation));
    FVector World;
    ScrollControlMath()->TransformMatrix(World, CamOrientation, Local);
    double const Inv = 1.0 / sqrt((static_cast<double>(World.X) * World.X +
                                   static_cast<double>(World.Y) * World.Y) +
                                  static_cast<double>(World.Z) * World.Z);
    float Ray[3];
    Ray[0] = static_cast<float>(World.X * Inv);
    Ray[1] = static_cast<float>(World.Y * Inv);
    Ray[2] = static_cast<float>(World.Z * Inv);

    // the thumb starts a drag.
    bool bStartDrag = false;
    if (bThumbValid != 0 && RayHits(ThumbMouse, ThumbXform, Ray))
    {
        Win3DPushCapture(this);
        bStartDrag = true;
    }
    else if (RayHits(Next.MouseSize, Next.Xform, Ray))
    {
        // one step up at once, then auto-repeat from Update.
        Win3DPushCapture(this);
        if (Value < MaxRange)
        {
            Value += Step;
            if (Value > MaxRange)
                Value = MaxRange;
            UpdateThumb();
            NotifyValue();
        }
        bNextPressed = 1;
        RepeatTime = static_cast<float>(GetClientElapsedTime());
        return true;
    }
    else if (RayHits(Prev.MouseSize, Prev.Xform, Ray))
    {
        // one step down.
        Win3DPushCapture(this);
        if (Value > MinRange)
        {
            Value -= Step;
            if (Value < MinRange)
                Value = MinRange;
            UpdateThumb();
            NotifyValue();
        }
        bPrevPressed = 1;
        RepeatTime = static_cast<float>(GetClientElapsedTime());
        return true;
    }
    else
    {
        // the back plate.
        if (bThumbValid == 0)
            return true;
        if (!RayHits(Back.MouseSize, Back.Xform, Ray))
            return true;
        Win3DPushCapture(this);
        if (bSliderBehavior == 0)
        {
            // page, and keep paging from Update.
            bPaging = 1;
            RepeatTime = static_cast<float>(GetClientElapsedTime());
            ChoosePageDirection(static_cast<unsigned>(CursorPixelX()),
                                static_cast<unsigned>(CursorPixelY()));
            PageStep();
            return true;
        }
        bStartDrag = true;
    }

    // drag from here.
    if (bStartDrag)
    {
        bDragging = 1;
        G_ScrollControlDragging = 1;
        DragThumb(static_cast<unsigned>(CursorPixelX()), static_cast<unsigned>(CursorPixelY()));
    }
    return true;
}

void ScrollControl::RefreshAttachment()
{
    // Slot 35 is the routine (RET) in this vftable: a ScrollControl does not follow a
    // hardpoint the way WinControl's the routine does.
}

intptr_t ScrollControl::SetProperty(int Msg, uintptr_t A, uintptr_t B)
{
    // (RET 0xc): Msg - 6 through the byte table 0x00583358.
    switch (Msg)
    {
    case 0x06:
    {
        // WinControl::Offset (+0xa8) from the vector at A, then the layout.
        float const* const NewOffset = reinterpret_cast<float const*>(A);
        Offset[0] = NewOffset[0];
        Offset[1] = NewOffset[1];
        Offset[2] = NewOffset[2];
        UpdateLayout();
        return 0;
    }
    case 0x37:
    {
        // The range, ordered, the value clamped into it, reported.
        int const First = static_cast<int>(A);
        int const Second = static_cast<int>(B);
        MinRange = First;
        MaxRange = Second;
        if (First > Second)
        {
            MaxRange = First;
            MinRange = Second;
        }
        if (Value > MaxRange)
            Value = MaxRange;
        if (Value < MinRange)
            Value = MinRange;
        UpdateThumb();
        NotifyValue();
        return 0;
    }
    case 0x38:
    {
        // The value, not below the minimum; above the maximum only when the range is not empty.
        // Not reported.
        Value = static_cast<int>(A);
        if (Value < MinRange)
            Value = MinRange;
        if (Value > MaxRange && MaxRange > MinRange)
            Value = MaxRange;
        UpdateThumb();
        return 0;
    }
    case 0x39:
        return MaxRange;
    case 0x3a:
        return MinRange;
    case 0x3b:
        return Value;
    case 0x3c:
        // The page size, and the button step (0 means 1).
        PageSize = static_cast<int>(A);
        Step = (B != 0) ? static_cast<int>(B) : 1;
        return 0;
    case 0x3d:
        return PageSize;
    default:
        return WinControl::SetProperty(Msg, A, B);
    }
}

void ScrollControl::AddAttachOffset(float const* Delta)
{
    // (RET 4): Delta onto the translation of every hardpoint transform; nothing
    // is re-laid out.
    Back.Hp.Translation.X += Delta[0];
    Back.Hp.Translation.Y += Delta[1];
    Back.Hp.Translation.Z += Delta[2];
    Next.Hp.Translation.X += Delta[0];
    Next.Hp.Translation.Y += Delta[1];
    Next.Hp.Translation.Z += Delta[2];
    Prev.Hp.Translation.X += Delta[0];
    Prev.Hp.Translation.Y += Delta[1];
    Prev.Hp.Translation.Z += Delta[2];
    ThumbHp0.Translation.X += Delta[0];
    ThumbHp0.Translation.Y += Delta[1];
    ThumbHp0.Translation.Z += Delta[2];
    ThumbHp1.Translation.X += Delta[0];
    ThumbHp1.Translation.Y += Delta[1];
    ThumbHp1.Translation.Z += Delta[2];
}

WinControl* WinControl::CreateScrollControl(
    char const* Name, char const* BackMesh, float const* BackOffset, char const* BackHp,
    float const* BackMouse, bool bBackNoMeshRender, char const* NextMesh, float const* NextOffset,
    char const* NextHp, float const* NextMouse, bool bNextNoMeshRender, char const* PrevMesh,
    float const* PrevOffset, char const* PrevHp, float const* PrevMouse, bool bPrevNoMeshRender,
    char const* ThumbMesh, float const* ThumbOffset0, float const* ThumbOffset1,
    char const* ThumbHp0, char const* ThumbHp1, float const* ThumbMouse,
    bool bThumbNoMeshRender, int MinRange, int MaxRange, int PageSize, bool bSliderBehavior,
    bool bNoZEnableArg)
{
    // (RET 0x70): the routine(Name, "ScrollControl", this, 0), then Init with the
    // other 27 arguments.
    Win3D* const Created = WinFactoryCreate(Name, "ScrollControl", this, nullptr);
    if (Created != nullptr)
    {
        static_cast<ScrollControl*>(Created)->Init(
            BackMesh, BackOffset, BackHp, BackMouse, bBackNoMeshRender, NextMesh, NextOffset,
            NextHp, NextMouse, bNextNoMeshRender, PrevMesh, PrevOffset, PrevHp, PrevMouse,
            bPrevNoMeshRender, ThumbMesh, ThumbOffset0, ThumbOffset1, ThumbHp0, ThumbHp1,
            ThumbMouse, bThumbNoMeshRender, MinRange, MaxRange, PageSize, bSliderBehavior,
            bNoZEnableArg);
    }
    return static_cast<WinControl*>(Created);
}
