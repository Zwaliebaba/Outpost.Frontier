// Author: Giperion.

#include "type_select.h"

#include "globals.h"
#include "ids_rdl.h"      // LoadIdsIntoRdl (the routine), RdlRemoveLastParagraph (the routine)
#include "login_mgr.h"
#include "menu_ui_init.h"
#include "nn_prompt.h"    // / the routine

#include "common_errorhandler.h" // ErrorHandler::log_last_error, SeverityWarning
#include "common_text.h"  // RenderDisplayList

#include <mmsystem.h>
#include <string.h>

namespace
{
    // set once the multiplayer notice has been accepted (0x00566c53); read
    // only at 0x00566c25.
    unsigned char G_bMultiplayerNoticeAccepted = 0;

    // s_Client\Interface\Intro.cpp (0x005e2724) and the WARNING format
    // (0x005e27a0) every ErrorHandler::log_last_error call of Intro.cpp passes.
    char const kIntroSourceFile[] = "Client\\Interface\\Intro.cpp";
    char G_IntroLastErrorFormat[] = "%s(%d) : WARNING:LastError:%s";

    // 01 in the PE image and never written (two reads, 0x00566576 and
    // ): the IsNetworkAlive call is always skipped.
    unsigned char const G_bSkipNetworkAliveProbe = 1;

    // DLLVERSIONINFO as the inlined DllGetVersion probe fills it (cbSize = 0x14).
    struct FDllVersionInfo
    {
        DWORD Size;
        DWORD Major;
        DWORD Minor;
        DWORD Build;
        DWORD PlatformId;
    };
    typedef HRESULT(CALLBACK* FDllGetVersionFn)(FDllVersionInfo* Info);

    // The click the Win3D input router builds for a primary button release
    // (the routine; see FWin3DMouseEvent).
    float const kSlideX = -0.455000013f;
    float const kColumnTopY = 0.172499999f;
    float const kColumnStepY = 0.0822000017f;
    float const kColumnZ = -1.0f;

    // Button i's slide position for the current AnimTime (0x00566612..0x005666d9, the same
    // body again at 0x00566772..0x00566837 for the slide-out).
    void PlaceColumnButton(TypeSelect* Self, unsigned Index)
    {
        int const Count = 6 - static_cast<int>(Index);
        double const Offset = static_cast<double>(Count) * 0.1;
        double const SlideX = Offset + Self->AnimTime - 0.45500001311302185;
        float Position[3];
        Position[1] = static_cast<float>(static_cast<double>(kColumnTopY) -
                                         static_cast<double>(Index) *
                                             static_cast<double>(kColumnStepY));
        Position[2] = kColumnZ;
        float StaticOffset[3] = {0.0f, 0.0f, 0.0f};
        // FCOM the field (-0.455) / TEST AH,0x41 / JNZ: at or left of -0.455 (or unordered)
        // the button follows the slide and the mesh is pushed by Offset + AnimTime; right of
        // it the button rests at -0.455 and the mesh at its origin.
        if (SlideX > -0.45500001311302185)
        {
            Position[0] = kSlideX;
        }
        else
        {
            Position[0] = static_cast<float>(SlideX);
            StaticOffset[0] = static_cast<float>(Offset + Self->AnimTime);
        }
        Self->Buttons[Index].SetPosition(Position);   // slot 5
        Self->Statics[Index]->SetPosition(StaticOffset); // slot 5
    }

    TypeSelect G_TypeSelect;
}

// ---------------------------------------------------------------------------------------
// TextMsgButton
// ---------------------------------------------------------------------------------------

TextMsgButton::TextMsgButton()
    : Command(0)
{
    // Win3D and TextWin parts come from the base constructors; then +0xec = 0,
    // vftable 0x005e2844, +0xe6 = 1.
    bUsePalette = 1;
}

TextMsgButton::~TextMsgButton()
{
    // JMP the routine -- the TextWin destructor does the work.
}

void TextMsgButton::MouseEnter(float const* /*Ray*/)
{
    // .
    if (bUsePalette != 0)
        PlayUiRolloverSound();
}

bool TextMsgButton::MouseEvent(FWin3DMouseEvent const* Event)
{
    // .
    if (Event->Field1 != 2)
        return false;
    if (bUsePalette == 0)
        return false;
    if (Event->Field0 != -1)
        return false;
    if (Parent == nullptr)
        return false;
    return Parent->Slot23(0x3004, static_cast<unsigned>(Command), 0);
}

void TextMsgButton::Init(Win3D* NewParent, wchar_t const* Caption, float BoxWidth,
                         float BoxHeight, int NewCommand)
{
    // .
    Command = NewCommand;
    bUsePalette = 1;
    Create(NewParent, nullptr); // slot 9, result not checked
    Width = BoxWidth;
    Height = BoxHeight;
    SetText(Caption);             // slot 28
    SetFont("ButtonText", 1.0f);  // (s_ButtonText 0x005d0410)
}

void TextMsgButton::SetFlagsE4E5(unsigned char A, unsigned char B)
{
    // .
    FlagE3 = 1;
    FieldE4 = A;
    FieldE5 = B;
}

// ---------------------------------------------------------------------------------------
// TypeSelect
// ---------------------------------------------------------------------------------------

TypeSelect::TypeSelect()
    : NextState(0)
    , Version(nullptr)
    , SensApi(nullptr)
    , IsNetworkAlive(nullptr)
    , PollIntervalMs(0)
    , LastPollMs(0)
    , Pad55c(0)
    , AnimTime(0.0)
    , PendingCommand(0)
    , bAnimating(0)
    , Phase(0)
{
    // the inlined Win3D ctor, Event::Handler vftable 0x005cbed8 at +0x7c, the
    // five TextMsgButtons (`eh vector constructor iterator` the routine, ctor the routine,
    // dtor the routine), +0x550 = 0, +0x54c = 0, vftables 0x005e27cc / 0x005e27c4.
    // +0x80, +0x534..+0x548 and +0x554..+0x570 are not written by the baseline ctor; the
    // instance is static storage (zero), which the member initialisers reproduce.
    Pad56d[0] = 0;
    Pad56d[1] = 0;
    Pad56d[2] = 0;
    for (unsigned Index = 0; Index < 5; ++Index)
        Statics[Index] = nullptr;
}

TypeSelect::~TypeSelect()
{
    // .
    if (SensApi != nullptr)
    {
        if (!FreeLibrary(SensApi))
            ErrorHandler::log_last_error(kIntroSourceFile, 0x17ed, SeverityWarning,
                                         G_IntroLastErrorFormat, GetLastError());
        SensApi = nullptr;
    }
    IsNetworkAlive = nullptr;
    // Then `eh vector destructor iterator` over the buttons (the routine each), the
    // Event::Handler dtor the routine on +0x7c and the inlined Win3D dtor -- the C++ member
    // and base destructors, in that order.
}

unsigned char TypeSelect::ProbeMultiplayerAvailable()
{
    // `CDPClient::GetClient(&the field)` (the routine is the
    // NeuronRuntime import thunk); none -> false. Otherwise, with the field set or no
    // IsNetworkAlive, true; else IsNetworkAlive(&flags) != 0. the field is the CDPClient
    // base of G_LoginMgr (the field, login_mgr.h).
    if (G_LoginMgr.GetClient() == nullptr)
        return 0;
    if (G_bSkipNetworkAliveProbe != 0 || IsNetworkAlive == nullptr)
        return 1;
    DWORD Flags = 0;
    return IsNetworkAlive(&Flags) != 0 ? 1 : 0;
}

bool TypeSelect::Create(Win3D* NewParent, void* /*Data*/)
{
    // .
    AnimTime = 1.0; // +0x560/+0x564 = 0 / 0x3ff00000
    Win3DReparent(this, NewParent);
    Win3DRegister(this);
    Flags = static_cast<unsigned char>((Flags & 0xfb) | kWin3DActive | kWin3DVisible);

    if (SensApi == nullptr)
    {
        // DllGetVersion of shdocvw.dll, inlined.
        DWORD Major;
        DWORD Minor;
        bool bHaveVersion = false;
        if (!IsBadWritePtr(&Major, 4) && !IsBadWritePtr(&Minor, 4))
        {
            Major = 0;
            Minor = 0;
            HMODULE const ShDocVw = LoadLibraryA("shdocvw.dll");
            if (ShDocVw != nullptr)
            {
                HRESULT Result;
                FDllGetVersionFn const GetVersion = reinterpret_cast<FDllGetVersionFn>(
                    GetProcAddress(ShDocVw, "DllGetVersion"));
                if (GetVersion != nullptr)
                {
                    FDllVersionInfo Info;
                    Info.Size = 0x14;
                    Info.Major = 0;
                    Info.Minor = 0;
                    Info.Build = 0;
                    Info.PlatformId = 0;
                    Result = GetVersion(&Info);
                    if (Result >= 0)
                    {
                        Major = Info.Major;
                        Minor = Info.Minor;
                    }
                }
                else
                {
                    Result = static_cast<HRESULT>(0x80004005L); // E_FAIL
                }
                FreeLibrary(ShDocVw);
                // only E_FAIL itself skips the version test.
                bHaveVersion = Result != static_cast<HRESULT>(0x80004005L);
            }
        }
        else
        {
            // / 0x00566961: a bad pointer jumps straight to the version test with
            // whatever the locals held; they are stack slots the probe owns, never bad.
            Major = 0;
            bHaveVersion = true;
        }
        (void)Minor;

        // version 5 or later -> sensapi.dll!IsNetworkAlive.
        if (bHaveVersion && Major >= 5)
        {
            SensApi = LoadLibraryA("sensapi.dll");
            if (SensApi != nullptr)
            {
                IsNetworkAlive = reinterpret_cast<FIsNetworkAliveFn>(
                    GetProcAddress(SensApi, "IsNetworkAlive"));
                if (IsNetworkAlive == nullptr)
                {
                    if (!FreeLibrary(SensApi))
                        ErrorHandler::log_last_error(kIntroSourceFile, 0x17d9, SeverityWarning,
                                                     G_IntroLastErrorFormat, GetLastError());
                    IsNetworkAlive = nullptr;
                    SensApi = nullptr;
                }
            }
            else
            {
                IsNetworkAlive = nullptr;
                SensApi = nullptr;
            }
        }
        else
        {
            // .
            IsNetworkAlive = nullptr;
            SensApi = nullptr;
        }
    }

    // .
    Buttons[2].bUsePalette = ProbeMultiplayerAvailable(); // +0x34a
    PollIntervalMs = 1000;
    LastPollMs = timeGetTime();
    return true;
}

void TypeSelect::Destroy()
{
    // .
    UnregisterAllEvents(); // on +0x7c
    if (SensApi != nullptr)
    {
        if (!FreeLibrary(SensApi))
            ErrorHandler::log_last_error(kIntroSourceFile, 0x17ed, SeverityWarning,
                                         G_IntroLastErrorFormat, GetLastError());
        SensApi = nullptr;
    }
    IsNetworkAlive = nullptr;
    Flags = static_cast<unsigned char>(Flags & 0xfc);

    // every child -- copy its class name (<= 31 chars) first, then
    // slot 10, then delete it through slot 0 if the name was "StaticControl" (s_ 0x005cb118,
    // a 14-byte compare). The next link is read before the calls.
    Win3D* Child = ChildHead;
    while (Child != nullptr)
    {
        Win3D* const Next = Child->NextSibling;
        char ClassCopy[32];
        ClassCopy[0] = '\0';
        if (Child->ClassName != nullptr)
        {
            size_t Length = strlen(Child->ClassName);
            if (Length >= 0x20)
                Length = 0x1f;
            memcpy(ClassCopy, Child->ClassName, Length);
            ClassCopy[Length] = '\0';
        }
        Child->Destroy();
        if (memcmp(ClassCopy, "StaticControl", 14) == 0)
            delete Child;
        Child = Next;
    }

    // the Version control goes through slot 0 (scalar deleting dtor) as well.
    if (Version != nullptr)
        delete Version;
    Version = nullptr;
    Win3DUnregister(this);
}

void TypeSelect::Update()
{
    // .
    Win3D::Update(); // inlined, 0x0056651e..0x00566548

    unsigned const Now = timeGetTime();
    if (Now - LastPollMs > PollIntervalMs)
    {
        Buttons[2].bUsePalette = ProbeMultiplayerAvailable(); // +0x34a
        LastPollMs = Now;
    }

    // FCOMP 1.0 / TEST AH,0x41 / JP -- above 1.0 (or unordered) only a running
    // slide-out continues; otherwise a running slide-out continues and an idle column slides
    // in.
    bool const bSlideIn = (AnimTime <= 1.0) && bAnimating == 0;
    if (!(AnimTime <= 1.0) && bAnimating == 0)
        return;

    if (bSlideIn)
    {
        // dt capped at 0.5 (the field), times 1.5 (the field).
        double Dt = GetClientFrameTimeCapped();
        if (Dt > 0.5)
            Dt = 0.5;
        AnimTime = Dt * 1.5 + AnimTime;
        for (unsigned Index = 0; Index < 5; ++Index)
            PlaceColumnButton(this, Index);

        // the swish while -0.8 < AnimTime < -0.1 (the field / the field).
        if (!(AnimTime > -0.80000000000000004))
            return;
        if (AnimTime < -0.10000000000000001)
            PlayUiSoundSlot(0x1b); // "ui_motion_swish"
        return;
    }

    // slide-out, dt uncapped.
    AnimTime = AnimTime - GetClientFrameTimeCapped() * 1.5;
    for (unsigned Index = 0; Index < 5; ++Index)
        PlaceColumnButton(this, Index);

    // the swish while -0.4 < AnimTime < 0.0 (the field / the field).
    if (AnimTime < 0.0 && AnimTime > -0.40000000000000002)
        PlayUiSoundSlot(0x1b);

    // at or below -0.9 (the field) the column is hidden; on the second such
    // frame the pending command becomes the next state.
    if (!(AnimTime <= -0.90000000000000002))
        return;
    Phase = Phase + 1;
    for (unsigned Index = 0; Index < 5; ++Index)
    {
        Buttons[Index].Flags = static_cast<unsigned char>(Buttons[Index].Flags & 0xfc);
        Statics[Index]->Flags = static_cast<unsigned char>(Statics[Index]->Flags & 0xfc);
    }
    if (Phase >= 2)
    {
        NextState = PendingCommand;
        bAnimating = 0;
    }
}

void TypeSelect::Draw()
{
    // JMP the routine.
    Win3DDrawChildren(this);
}

bool TypeSelect::Slot23(unsigned Msg, unsigned A, unsigned /*B*/)
{
    // .
    if (Msg != 0x3004)
        return false;
    PlayOrRewindUiSoundSlot(0); // (0)

    // Byte table 0x00566fa8 over A - 3 (0..0xe), jump table 0x00566f94.
    switch (A)
    {
    case 3:    // Load Game
    case 0xa:  // Options
    case 0x10: // New Game
        // start the slide-out unless one is running.
        if (bAnimating != 0)
            return false;
        PendingCommand = static_cast<int>(A);
        Phase = 0;
        bAnimating = 1;
        AnimTime = 0.34999999999999998; // _66666666
        return true;

    case 4:
    {
        // Multiplayer. Until it has been accepted once (the field), the online-
        // play notice (interfaceextras 0x601eb, style 0xc9b7, no server step, no tool tips);
        // declined -> 0x00566ee5: return true, nothing started, the latch left clear.
        if (G_bMultiplayerNoticeAccepted == 0)
        {
            if (!NnPromptRunYesNo(0x601eb, 0xc9b7, false, 0, 0))
                return true;
        }

        // the latch, then three lists, each loaded with the routine
        // and, when that succeeds, trimmed by the routine: the Yes caption 0x601ed ("LAN"),
        // the No caption 0x601ec ("INTERNET") and the question 0x601f0.
        G_bMultiplayerNoticeAccepted = 1;
        RenderDisplayList LanCaption;      // [ESP+0x1c]
        RenderDisplayList InternetCaption; // [ESP+0x34]
        RenderDisplayList Question;        // [ESP+0x28]
        if (LoadIdsIntoRdl(0x601ed, LanCaption))
            RdlRemoveLastParagraph(LanCaption);
        if (LoadIdsIntoRdl(0x601ec, InternetCaption))
            RdlRemoveLastParagraph(InternetCaption);
        if (LoadIdsIntoRdl(0x601f0, Question))
            RdlRemoveLastParagraph(Question);

        // the routine(&question, 0xc9b7, &lan, &internet, &choice, 0)
        // with the choice preset to -1; no choice -> 0x00566e31: return true.
        int Choice = -1;
        if (!NnPromptRunChoice(&Question, 0xc9b7, &LanCaption, &InternetCaption, &Choice,
                               false))
            return true;


        G_bEnumerateLanHosts = 1;
        G_bInternetServerList = 0;
        if (Choice == 1)
            G_bInternetServerList = 1;
        G_LoginMgr.ResetServerLists();                 // (&the field)

        // the slide-out of case 3 inlined; its outcome is the result.
        if (bAnimating != 0)
            return false;
        PendingCommand = static_cast<int>(A);
        Phase = 0;
        bAnimating = 1;
        AnimTime = 0.34999999999999998; // _66666666
        return true;
    }

    case 0xb:
        // the routine(0) -- NN_Exit.
        RequestClientQuitDialog();
        return true;

    case 0x11:
        // .
        NextState = static_cast<int>(A);
        return true;

    default:
        return false;
    }
}

bool TypeSelect::OnEvent(unsigned MsgId)
{
    // USER_CANCEL (0xf) raises NN_Exit.
    if (MsgId != 0xf)
        return false;
    RequestClientQuitDialog(); // (0)
    return true;
}

TypeSelect& GetTypeSelect()
{
    return G_TypeSelect;
}
