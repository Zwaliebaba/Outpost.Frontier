// Author: Giperion.

#include "hud_comm.h"

#include "character_info.h"   // GetCharacterInfoDb (the routine), AcquireScriptByName (the routine)
#include "client_gameobj.h"   // FClientGameobj : IObjInspectImpl
#include "client_objdb.h"     // ClientObjDb_FindObject (the routine)
#include "engine_character.h" // G_EngineCharacterCache (the field), FEngineCharacterBody
#include "engine_light.h"
#include "font_string.h"      // FFontString
#include "game_options.h"     // GetGameOption (the routine)
#include "globals.h"          // GetClientFrameTimeCapped, ShouldSkipLocalServerTick
#include "hud_target.h"       // GetShipPilotName, GetObjectIdsName, HudProjectScreenRect
#include "ids_strings.h"      // GetIdsStringWWithRawFallback (the routine)
#include "login_mgr.h"        // G_Server (the field)
#include "menu_sound_arch.h"
#include "menu_ui_init.h"     // PlayOrRewindUiSoundSlot (the routine)
#include "nn_button_control.h"
#include "nn_text_control.h"
#include "nn_text_scratch.h"  // G_NnTextScratchB (the field)
#include "nn_ui_pulse.h"      // G_HudChromeColorRgba (the field), G_NnPaletteArgb_679bb0
#include "perf_options.h"     // GetPerfOptionSlot (the routine)
#include "player_behavior.h"  // GetPlayerObjRW (the routine)
#include "sound_config.h"     // the [CFG] comm ducking / dialogue pan values
#include "space_camera.h"     // AsCamera, SpaceCameraSetHalfFovX, SpaceCameraSetViewportEx
#include "space_letterbox.h"  // IsLetterboxVisible (the routine)
#include "star_sphere.h"      // Win3DBindModelview (the routine), Win3DIdentityRotation3x3
#include "voice_profiles.h"   // LookupClientVoice (the routine), FVoiceProfile, FVoiceMsg
#include "win_factory.h"      // WinFactoryCreate (the routine)
#include "win3d_immediate.h"
#include "win3d_render.h"     // Win3DApplyCamera (the routine)

#include "common_cobject.h"   // CObject, CSimple, CShip
#include "common_csys.h"      // EulerMatrix, Matrix
#include "common_goods.h"     // COMM_* / WALLA_* / CHATTER_*, BaseWatcher, CBase
#include "common_id.h"        // CreateID
#include "common_iface.h"     // IObjRW, IObjInspectImpl, ObjRwFromWatchable
#include "common_ini.h"       // INI_Reader, Vector
#include "common_misc.h"      // ICamera
#include "common_server.h"    // IServer (G_Server, CommComplete vt+0xfc)
#include "soundmanager.h"     // ISoundSource

#include "NeuronRuntime.h"
#include "neuroncomponents.h"
#include "engine/sceneruntime.h"
#include "render/IRenderPipeline.h"
#include "render/shading.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include <list>
#include <string>

// FCommWindow has no port-only members, so its layout is baseline's.
static_assert(offsetof(FCommWindow, Rect) == 0x04, "comm window +0x04 rect");
static_assert(offsetof(FCommWindow, WinPos) == 0x24, "comm window +0x24 win_pos");
static_assert(offsetof(FCommWindow, BtnPos) == 0x30, "comm window +0x30 btn_pos");
static_assert(offsetof(FCommWindow, WinSizeX) == 0x3c, "comm window +0x3c win_size");
static_assert(offsetof(FCommWindow, OpenFraction) == 0x44, "comm window +0x44 open fraction");
static_assert(offsetof(FCommWindow, FlyTime) == 0x48, "comm window +0x48 fly_time");
static_assert(offsetof(FCommWindow, bOpenRequest) == 0x4c, "comm window +0x4c open request");
static_assert(offsetof(FCommWindow, bStatic) == 0x4e, "comm window +0x4e static");
static_assert(offsetof(FCommWindow, Corner) == 0x50, "comm window +0x50 corner holder");
static_assert(offsetof(FCommWindow, CornerXform) == 0x78, "comm window +0x78 corner transforms");
static_assert(sizeof(FCommWindow) == 0x138, "baseline comm window is 0x138 bytes");
static_assert(sizeof(FCommSlot) == 0x10, "baseline comm slot is 0x10 bytes");
static_assert(sizeof(FCommInbox) == 0x984, "baseline comm inbox is 0x984 bytes");
// FCommVoice / FCommChannel / HUD_Receiver carry a std::vector where baseline has VC6's
// four-dword vector plus an iterator (no baseline ABI, AGENTS.md), so their sizes are the port's.

FCommInbox G_CommInboxPlayer; // (XC the branch)
FCommInbox G_CommInboxOther;  // (XC the branch)
FCommDuck G_CommDuck = {0.0f, 0.0f, -1.0f, 0}; // ..the field (XC the branch)

namespace
{
    // .
    void* G_HudCommInstance = nullptr;

    // The static initializer the branch: the field, UHUD_Receiver::HUDFACTORY ("Receiver").
    HUDFACTORY<HUD_Receiver> G_HudReceiverFactory("Receiver");

    char const kHudCommCpp[] = "Client\\Space\\HUD_Comm.cpp";
    char const kRadioH[] = "Client\\space\\radio.h";

    unsigned const kCommInboxCapacity = 16;

    // baseline keeps the permutation in a 16-int stack array and writes it without a bound
    // (0x004caf26 / 0x004caf90); more than 16 numbers run into the next locals. A 0x100-wchar
    // resource string holds at most 128 numbers, so the port's array holds that many: every
    // input baseline survives behaves the same, and the overflow has nothing to corrupt.
    int const kCommPermutationSlots = 0x80;

    float const kLineRetrySeconds = 3.0f;  // at 0x004cb1b3 / 0x004cc04f
    float const kAdvanceStep = 0.2f;
    float const kAppearanceWarmUp = 0.1f;  // at 0x004cb5a1
    float const kCornerOpenRate = 2.5f;
    float const kHalf = 0.5f;
    float const kFrameInset = 0.105f;
    float const kFrameSize = 0.207f;
    float const kPortraitU1 = 1.0f;        // (read only)
    float const kPortraitV1 = 1.0f;        // (read only)
    float const kPortraitZ = -30.0f;       // (read only)

    // IDeformable::Play takes every trailing argument as a dword; the floats baseline pushes as
    // immediates (0x3f800000, 0xbf800000, 0x3e99999a) go through as their bit patterns.
    void* FloatArg(float Value)
    {
        uintptr_t Bits = 0;
        memcpy(&Bits, &Value, sizeof(Value));
        return reinterpret_cast<void*>(Bits);
    }
    float const kPortraitHalf = 3.0f;      // / 0xc0400000 at 0x004cc6d5..0x004cc74a
    unsigned char const kStaticBrightness = 0xff; // (read only)
    float const kStaticUvStep = 0.00390625f;
    float const kPanPitchLimit = 0.1745329350233078f;
    double const kPanMinDistance = 0.1;
    double const kPanRangeScale = 0.005555555555555556; // (1 / 180)
    double const kPanFloorScale = 0.01;
    double const kPanScale = 100.0;

    // slot 8 (get_id) against an id -- the body of the routine (against the
    // message's Sender) and the routine (its Receiver). Not player_spawn.h's IsPlayerShipId
    // (the routine, the field): a different test, hence a different name.
    bool IsPlayerObjRwId(unsigned ObjectId)
    {
        void* const Player = GetPlayerObjRW();
        if (Player == nullptr)
        {
            return false;
        }
        return static_cast<IObjRW*>(Player)->get_id() == ObjectId;
    }

    // The inlined lookup every speaker site repeats (0x004cb0d9..0x004cb10d, 0x004ce081..0x004ce0b7,
    // ): the routine(Id, 0), the gameobj's CBase face (+0xc) and its
    // CObject (+4), kept only when the class has all of 0x103.
    CObject* CommObjectOf(unsigned ObjectId)
    {
        FClientGameobj* const Gameobj = ClientObjDb_FindObject(ObjectId, 0);
        if (Gameobj == nullptr)
        {
            return nullptr;
        }
        CBase const* const Base = static_cast<IObjInspectImpl*>(Gameobj);
        CObject* const Object = Base->Object;
        if (Object == nullptr || (static_cast<unsigned>(Object->ObjectClass) & 0x103u) != 0x103u)
        {
            return nullptr;
        }
        return Object;
    }

    // The watcher-to-object step (`MOV EAX,[watcher]; ADD EAX,-0x9c`, both null-tested).
    CSimple* SimpleOfWatchable(Watchable const* Watched)
    {
        if (Watched == nullptr)
        {
            return nullptr;
        }
        return reinterpret_cast<CSimple*>(
            const_cast<char*>(reinterpret_cast<char const*>(Watched)) - offsetof(CSimple, SelfWatchable));
    }

    Watchable const* WatchableOf(CObject* Object)
    {
        if (Object == nullptr)
        {
            return nullptr;
        }
        return &static_cast<CSimple*>(Object)->SelfWatchable;
    }

    // The dword at +0x40 + TextCount * 4: the last text id (the caption ids when there are
    // none). Read as baseline does, byte offset from the record.
    unsigned LastTextId(FCommMessage const* Msg)
    {
        unsigned Value = 0;
        memcpy(&Value, reinterpret_cast<char const*>(Msg) + offsetof(FCommMessage, CaptionIds) +
                           static_cast<ptrdiff_t>(Msg->TextCount) * 4,
               sizeof(Value));
        return Value;
    }

    // on a VoiceMsg archetype (0x004cb188 / 0x004cc01b): baseline passes the
    // VoiceMsg itself (vtable 0x005cabfc, an Archetype::ISound) as the sound archetype, with the
    // alternate-start byte 0.
    unsigned char PlayVoiceMsgArch(FVoiceMsg* Msg, unsigned* PlayKey, ISoundSource** OutSource)
    {
        void* Handle = nullptr;
        unsigned char const bOk = PlayClientSoundArch(static_cast<Archetype::ISound*>(Msg),
                                                      PlayKey, &Handle, 0);
        *OutSource = static_cast<ISoundSource*>(Handle);
        return bOk;
    }

    // The ducking push Play inlines (0x004cb1c5..0x004cb1eb).
    void PushCommDuck()
    {
        PushClientSoundDucking();
        if (G_CommDuck.RefCount == 0)
        {
            G_CommDuck.TargetDb = GetClientCommDuckDownBy();
            G_CommDuck.FadeRemaining = GetClientCommDuckDownTime();
        }
        ++G_CommDuck.RefCount;
    }

    // The CObject of a watcher-derived CSimple, as Appearance's caption needs it.
    CObject* ObjectOfSimple(CSimple* Simple)
    {
        return Simple;
    }
}

// ============================================================================================
// Radio inbox
// ============================================================================================

FCommMessage::FCommMessage()
    : Sender(0),
      Receiver(0),
      VoiceId(0),
      CaptionIds(0),
      TextCount(1),
      PermutationIds(0),
      Attenuation(0.0f),
      Linger(0.0f),
      Result(CommResult::SUCCESS)
{
    // parts and accessory count 0, TextCount 1, +0x88 / +0x90 zero. The rest is
    // zeroed by the port (baseline leaves whatever was there).
    memset(&SenderCostume, 0, sizeof(SenderCostume));
    memset(TextIds, 0, sizeof(TextIds));
}

FCommInbox::FCommInbox() : Count(0)
{
}

void CommCopyCostume(Costume* To, Costume const* From)
{
    // .
    To->PartIds[0] = From->PartIds[0];
    To->PartIds[1] = From->PartIds[1];
    To->PartIds[2] = From->PartIds[2];
    To->PartIds[3] = From->PartIds[3];
    To->AccessoryCount = From->AccessoryCount;
    // TEST / JLE, then CMP / JL against the count just stored: a signed loop with no bound.
    for (int Index = 0; Index < static_cast<int>(To->AccessoryCount); ++Index)
    {
        To->AccessoryIds[Index] = From->AccessoryIds[Index];
    }
}

void CommCopyMessage(FCommMessage* To, FCommMessage const* From)
{
    // .
    To->Sender = From->Sender;
    To->Receiver = From->Receiver;
    CommCopyCostume(&To->SenderCostume, &From->SenderCostume);
    To->VoiceId = From->VoiceId;
    To->CaptionIds = From->CaptionIds;
    for (int Index = 0; Index < 16; ++Index)
    {
        To->TextIds[Index] = From->TextIds[Index];
    }
    To->TextCount = From->TextCount;
    To->PermutationIds = From->PermutationIds;
    To->Attenuation = From->Attenuation;
    To->Linger = From->Linger;
    To->Result = From->Result;
}

bool FCommInbox::Peek(FCommMessage* Out) const
{
    // CMP [ECX],0 / JBE.
    if (Count == 0)
    {
        return false;
    }
    CommCopyMessage(Out, &Records[0]);
    return true;
}

bool FCommInbox::Pop(FCommMessage* Out)
{
    // .
    if (Count == 0)
    {
        return false;
    }
    CommCopyMessage(Out, &Records[0]);
    for (unsigned Index = 1; Index < Count; ++Index)
    {
        CommCopyMessage(&Records[Index - 1], &Records[Index]);
    }
    --Count;
    return true;
}

void ClientSendComm(unsigned PlayerId, unsigned Sender, unsigned Receiver, unsigned VoiceId,
                    Costume const& SenderCostume, unsigned CaptionIds, unsigned const* TextIds,
                    int TextCount, unsigned PermutationIds, float Linger, bool bUnused)
{
    // . The record is built in the frame first (0x0053bfb6..0x0053c071).
    (void)PlayerId;
    (void)bUnused;
    FCommMessage Record;
    Record.Sender = Sender;
    Record.Receiver = Receiver;
    Record.SenderCostume.PartIds[0] = SenderCostume.PartIds[0];
    Record.SenderCostume.PartIds[1] = SenderCostume.PartIds[1];
    Record.SenderCostume.PartIds[2] = SenderCostume.PartIds[2];
    Record.SenderCostume.PartIds[3] = SenderCostume.PartIds[3];
    Record.VoiceId = VoiceId;
    Record.SenderCostume.AccessoryCount = SenderCostume.AccessoryCount;
    for (int Index = 0; Index < static_cast<int>(Record.SenderCostume.AccessoryCount); ++Index)
    {
        Record.SenderCostume.AccessoryIds[Index] = SenderCostume.AccessoryIds[Index];
    }
    Record.CaptionIds = CaptionIds;
    if (TextCount > 0)
    {
        memcpy(Record.TextIds, TextIds, static_cast<size_t>(TextCount) * sizeof(unsigned));
    }
    Record.TextCount = TextCount;
    Record.PermutationIds = PermutationIds;
    Record.Linger = Linger;
    // +0x8c and +0x94 are never written here: baseline queues whatever its frame held there. The
    // port's record carries 0 (attenuation 0 dB, result SUCCESS). Dispatch overwrites +0x8c
    // before any read; +0x94 reaches the server unchanged when Dispatch cancels the message
    // (the routine from 0x004ce446).

    // the player's inbox when the player's ship sends or receives it.
    FCommInbox* Inbox = &G_CommInboxOther;
    void* const Player = GetPlayerObjRW();
    if (Player != nullptr && (static_cast<IObjRW*>(Player)->get_id() == Sender ||
                              static_cast<IObjRW*>(Player)->get_id() == Receiver))
    {
        Inbox = &G_CommInboxPlayer;
    }

    if (Inbox->Count >= kCommInboxCapacity)
    {
        FDUMP(0x100002, "%s(%d) : *** WARNING: %s", kRadioH, 0x7c,
              "inbox overflow - too many comm messages");
        return;
    }
    FCommMessage* const Slot = &Inbox->Records[Inbox->Count];
    ++Inbox->Count;
    Slot->Sender = Record.Sender;
    Slot->Receiver = Record.Receiver;
    CommCopyCostume(&Slot->SenderCostume, &Record.SenderCostume);
    Slot->VoiceId = Record.VoiceId;
    Slot->CaptionIds = Record.CaptionIds;
    for (int Index = 0; Index < 16; ++Index)
    {
        Slot->TextIds[Index] = Record.TextIds[Index];
    }
    Slot->TextCount = Record.TextCount;
    Slot->PermutationIds = Record.PermutationIds;
    Slot->Attenuation = Record.Attenuation;
    Slot->Linger = Record.Linger;
    Slot->Result = Record.Result;
}

// ============================================================================================
// Ducking
// ============================================================================================

void FCommDuck::Release()
{
    // .
    --RefCount;
    if (RefCount == 0)
    {
        TargetDb = 0.0f;
        FadeRemaining = GetClientCommDuckUpTime();
    }
}

// ============================================================================================
// FCommVoice
// ============================================================================================

FCommVoice::FCommVoice() : Cursor(0)
{
    // 0x004ca6b4..0x004ca6e3: the watcher zeroed and set(0), the vector, then
    // . baseline leaves +0x34 (the iterator) unset.
    Speaker.set(nullptr);
    Pad09[0] = Pad09[1] = Pad09[2] = 0;
    Pad16[0] = Pad16[1] = 0;
    Pad21[0] = Pad21[1] = Pad21[2] = 0;
    Static = nullptr;
    ResetState();
}

FCommVoice::~FCommVoice()
{
    // .
    StopStatic();
    StopLines();
    DropDucking();
    ResetState();
    // The vector's buffer and the watcher go with the members.
}

void FCommVoice::ResetState()
{
    // .
    if (Speaker.target() != nullptr)
    {
        Speaker.set(nullptr); // BaseWatcher::set_pointer(0)
    }
    Static = nullptr;
    Timer = 0.0f;
    bDucking = 0;
    bKeyBit2 = 0;
    Attenuation = 0.0f;
    DuckDb = 0.0f;
    bWatchSpeaker = 1;
    bPositional = 1;
}

void FCommVoice::StopStatic()
{
    if (Static != nullptr)
    {
        Static->Stop();    // vt+0x60
        Static->Release(); // vt+0x08
        Static = nullptr;
    }
}

void FCommVoice::StopLines()
{
    // .
    for (size_t Index = 0; Index < Lines.size(); ++Index)
    {
        FCommLine& Line = Lines[Index];
        if (Line.Msg != nullptr && Line.bAcquired == 1)
        {
            Line.Msg->Release(); // VoiceMsg vt+0x10
            Line.bAcquired = 0;
        }
        if (Line.Source != nullptr)
        {
            Line.Source->Stop();
            Line.Source->Release();
            Line.Source = nullptr;
        }
    }
    Lines.clear();
}

void FCommVoice::DropDucking()
{
    if (bDucking != 0)
    {
        PopClientSoundDucking();
        G_CommDuck.Release();
        bDucking = 0;
    }
}

void FCommVoice::StopVoice()
{
    // . The cursor keeps its value.
    StopStatic();
    StopLines();
    DropDucking();
    ResetState();
}

void FCommVoice::Play(FCommMessage const* Msg, bool bStaticSound, bool bDuck, bool bPositionalArg,
                      unsigned* OutSender, unsigned* OutReceiver, unsigned* OutLastMsgId)
{
    // .
    bKeyBit2 = 0;
    *OutSender = 0;
    *OutReceiver = 0;
    *OutLastMsgId = 0;
    Attenuation = Msg->Attenuation;
    bPositional = bPositionalArg ? 1 : 0;
    // No null test on the profile: every caller has already looked it up.
    FVoiceProfile* const Profile = LookupClientVoice(Msg->VoiceId);
    StopLines();

    // the play order.
    int Permutation[kCommPermutationSlots];
    int PermutationCount = 0;
    if (Msg->PermutationIds != 0)
    {
        wchar_t OrderText[0x100];
        if (GetIdsStringWWithRawFallback(Msg->PermutationIds, OrderText, 0x100) == 0)
        {
            FDUMP(0x100001, "%s(%d) : *** ERROR: Error loading resource string %d\n", kHudCommCpp,
                  0x2e8, Msg->PermutationIds);
        }
        else
        {
            wchar_t* Scan = OrderText;
            for (;;)
            {
                while (*Scan == L' ' || *Scan == L'\t' || *Scan == L',')
                {
                    ++Scan;
                }
                wchar_t* End = nullptr;
                long const Value = wcstol(Scan, &End, 10);
                // CMP ECX,ESI / JBE: stop when nothing was consumed.
                if (!(End > Scan))
                {
                    break;
                }
                Permutation[PermutationCount] = static_cast<int>(Value);
                ++PermutationCount;
                Scan = End;
            }
        }
    }
    else
    {
        if (Msg->TextCount > 1)
        {
            FDUMP(0x100002,
                  "%s(%d) : *** WARNING: No permutation for comm, playing %d messages in order\n",
                  kHudCommCpp, 0x2f3, Msg->TextCount);
        }
        for (int Index = 0; Index < Msg->TextCount; ++Index)
        {
            Permutation[Index] = Index;
        }
        PermutationCount = Msg->TextCount;
    }

    // queue the lines.
    unsigned LastMsgId = 0;
    for (int Index = 0; Index < Msg->TextCount; ++Index)
    {
        if (Index >= PermutationCount)
        {
            FDUMP(0x100002,
                  "%s(%d) : *** WARNING: Comm message index %d is >= permutation size %d, "
                  "skipping message part\n",
                  kHudCommCpp, 0x305, Index, PermutationCount);
            continue;
        }
        int const Part = Permutation[Index];
        // CMP / JL: signed. A negative number from the resource string passes and indexes
        // below TextIds, as in baseline.
        if (Part >= Msg->TextCount)
        {
            FDUMP(0x100002,
                  "%s(%d) : *** WARNING: Ignoring comm sequence permutation index %d which is >= "
                  "message count %d\n",
                  kHudCommCpp, 0x30f, Part, Msg->TextCount);
            continue;
        }
        unsigned const MsgId = Msg->TextIds[Part];
        FVoiceMsg* const VoiceMsg = Profile->FindMessage(MsgId); // VoiceProfile vt+0x14
        if (VoiceMsg == nullptr)
        {
            FDUMP(0x100002, "%s(%d) : *** WARNING: invalid MSG id (%u) for voice profile (%s)\n",
                  kHudCommCpp, 0x318, MsgId, Profile->GetName()); // vt+0x10
            continue;
        }
        FCommLine Line;
        Line.MsgId = MsgId;
        Line.Msg = VoiceMsg;
        Line.bAcquired = 0;
        Line.Source = nullptr;
        Lines.push_back(Line);
        LastMsgId = MsgId;
    }

    if (Lines.empty())
    {
        return;
    }

    // .
    *OutSender = Msg->Sender;
    *OutReceiver = Msg->Receiver;
    *OutLastMsgId = LastMsgId;

    // watch the sender.
    if (Msg->Sender != 0)
    {
        Watchable const* const Watch = WatchableOf(CommObjectOf(Msg->Sender));
        if (Watch != Speaker.target())
        {
            Speaker.set(Watch); // set_pointer
        }
        bWatchSpeaker = 1;
    }
    else
    {
        if (Speaker.target() != nullptr)
        {
            Speaker.set(nullptr);
        }
        bWatchSpeaker = 0;
    }

    // the first line, with the same body as the routine.
    Timer = 0.0f;
    Cursor = 0;
    PlayLine(&Lines[0]);

    if (bDuck)
    {
        PushCommDuck();
        bDucking = 1;
    }

    if (bStaticSound)
    {
        // .
        ID_String StaticId;
        StaticId.Id = CreateID("comm_static_1");
        void* const StaticArch = LookupClientSoundArch(&StaticId);
        void* Handle = nullptr;
        unsigned char const bStarted = PlayClientSoundArch(StaticArch, nullptr, &Handle, 0);
        Static = static_cast<ISoundSource*>(Handle);
        if (bStarted != 0)
        {
            SetClientSoundSourceType(Static, 2);
        }
    }
}

bool FCommVoice::PlayLine(FCommLine* Line)
{
    // . The play key: +0 = 0, +4 flags with bits 0/1 cleared and bit 2 from +0x15
    // (baseline keeps whatever its frame held in bits 3..31; the port has 0 there), +8/+0xc/+0x10
    // = 0, +0x14 = the attenuation.
    unsigned PlayKey[6] = {};
    PlayKey[1] = static_cast<unsigned>((bKeyBit2 << 2) & 4);
    float const KeyAttenuation = static_cast<float>(static_cast<double>(Attenuation) + DuckDb);
    memcpy(&PlayKey[5], &KeyAttenuation, sizeof(float));
    if (PlayVoiceMsgArch(Line->Msg, PlayKey, &Line->Source) != 0)
    {
        SetClientSoundSourceType(Line->Source, 2);
        Line->Source->SetAttenuation(
            static_cast<float>(static_cast<double>(Attenuation) + DuckDb)); // vt+0x80
        return true;
    }
    Timer = kLineRetrySeconds;
    return true;
}

float FCommVoice::Pan(float X, float Y, float Z)
{
    // .
    double const PlanarLength = sqrt(static_cast<double>(X) * X + static_cast<double>(Y) * Y);
    float const Distance = static_cast<float>(PlanarLength);
    // FCOMPP / TEST AH,0x41 / JNE: on unless the pitch term is strictly larger.
    if (fabs(static_cast<double>(Z) * kPanPitchLimit) > PlanarLength)
    {
        return 0.0f;
    }
    // TEST AH,0x41 / JP: on when above 0.1 (or NaN).
    if (static_cast<double>(Distance) <= kPanMinDistance)
    {
        return 0.0f;
    }
    double const Inverse = 1.0 / static_cast<double>(Distance);
    float const NormalX = static_cast<float>(static_cast<double>(X) * Inverse);
    double NormalY = static_cast<double>(Y) * Inverse;
    // TEST AH,5 / JP: negated only when below zero.
    if (NormalY < 0.0)
    {
        NormalY = -NormalY;
    }
    double const Angle = atan2(-static_cast<double>(NormalX), NormalY) *
                         (static_cast<double>(GetClientDialoguePanRange()) * kPanRangeScale);
    float const Factor = GetClientDialoguePanFactor();
    float const NegSin = static_cast<float>(-sin(Angle));
    double Cosine = cos(Angle);
    float const Floor = static_cast<float>(static_cast<double>(Factor) * kPanFloorScale);
    // TEST AH,5 / JP: the floor replaces a cosine below it.
    if (Cosine < static_cast<double>(Floor))
    {
        Cosine = Floor;
    }
    return static_cast<float>(static_cast<double>(Factor) * NegSin * kPanScale / Cosine);
}

// ============================================================================================
// FCommWindow
// ============================================================================================

FCommWindow::FCommWindow()
    : bOpen(0),
      OpenFraction(0.0f),
      FlyTime(0.5f),
      bOpenRequest(0),
      bCloseRequest(0),
      bStatic(1),
      Pad4f(0)
{
    // The window half of the routine (0x004ca6e8..0x004ca7b6).
    Pad01[0] = Pad01[1] = Pad01[2] = 0;
    Rect[0] = 0;
    Rect[1] = 0;
    Rect[2] = 1;
    Rect[3] = 1;
    memset(Unknown14, 0, sizeof(Unknown14)); // port-only: baseline never writes +0x14..+0x23
    WinPos.X = 0.3f;
    WinPos.Y = 0.3f;
    WinPos.Z = -1.0f;
    BtnPos.X = 0.0f;
    BtnPos.Y = 0.0f;
    BtnPos.Z = -2.0f;
    WinSizeX = 0.2f;
    WinSizeY = 0.2f;
    // on +0x50 is the holder's own member initialisation.
    for (int Index = 0; Index < 4; ++Index)
    {
        // identity rotation, zero translation.
        Win3DIdentityRotation3x3(&CornerXform[Index].Rotation.D[0][0]);
        CornerXform[Index].Translation.X = 0.0f;
        CornerXform[Index].Translation.Y = 0.0f;
        CornerXform[Index].Translation.Z = 0.0f;
    }
}

FCommWindow::~FCommWindow()
{
    // .
    if (ClientMeshHolderHasInstance(&Corner))
    {
        ClientUiMeshHolderRelease(&Corner);
    }
    // Then the holder's own dtor the routine -- ~FClientMeshHolder.
}

bool FCommWindow::ReadIni(INI_Reader& Reader)
{
    // .
    if (Reader.is_value("corner"))
    {
        ClientMeshLoadFromPath(&Corner, Reader.get_value_string(), 0, false); // (p, 0, 0)
        // the field's four bytes / 255 into +0x14/+0x18/+0x1c/+0x24,
        // 's body inlined.
        unsigned long const Palette = G_NnPaletteArgb_679bb0;
        unsigned char Colour[4];
        memcpy(Colour, &Palette, sizeof(Colour));
        ClientMeshHolderSetColor(&Corner, Colour);
        return true;
    }
    if (Reader.is_value("btn_pos"))
    {
        // baseline reads value 1 first, then 0, then 2.
        float const Y = Reader.get_value_float(1);
        float const X = Reader.get_value_float(0);
        float const Z = Reader.get_value_float(2);
        BtnPos.X = X;
        BtnPos.Y = Y;
        BtnPos.Z = Z;
        return true;
    }
    if (Reader.is_value("win_pos"))
    {
        float const Y = Reader.get_value_float(1);
        float const X = Reader.get_value_float(0);
        float const Z = Reader.get_value_float(2);
        WinPos.X = X;
        WinPos.Y = Y;
        WinPos.Z = Z;
        return true;
    }
    if (Reader.is_value("win_size"))
    {
        WinSizeX = Reader.get_value_float(0);
        WinSizeY = Reader.get_value_float(1);
        return true;
    }
    if (Reader.is_value("fly_time"))
    {
        FlyTime = Reader.get_value_float(0);
        return true;
    }
    if (Reader.is_value("static"))
    {
        // FCOMP 0.0 / TEST AH,0x44 / JNP: 0 only when equal to zero.
        bStatic = (Reader.get_value_float(0) != 0.0f) ? 1 : 0;
        return true;
    }
    return false;
}

void FCommWindow::Layout(ICamera* Cam)
{
    // .
    float Grow = static_cast<float>(static_cast<double>(OpenFraction) * kCornerOpenRate);
    if (Grow < 0.0f)
    {
        Grow = 0.0f;
    }
    else if (Grow > 1.0f)
    {
        Grow = 1.0f;
    }
    float Size = OpenFraction;
    if (Size < 0.0f)
    {
        Size = 0.0f;
    }
    else if (Size > 1.0f)
    {
        Size = 1.0f;
    }

    // / the routine / the routine: P = BtnPos + (WinPos - BtnPos) * Grow.
    FVector Delta;
    Delta.X = WinPos.X - BtnPos.X;
    Delta.Y = WinPos.Y - BtnPos.Y;
    Delta.Z = WinPos.Z - BtnPos.Z;
    FVector Scaled;
    Scaled.X = static_cast<float>(static_cast<double>(Delta.X) * Grow);
    Scaled.Y = static_cast<float>(static_cast<double>(Delta.Y) * Grow);
    Scaled.Z = static_cast<float>(static_cast<double>(Delta.Z) * Grow);
    FVector Centre;
    Centre.X = BtnPos.X + Scaled.X;
    Centre.Y = BtnPos.Y + Scaled.Y;
    Centre.Z = BtnPos.Z + Scaled.Z;
    float const HalfW = static_cast<float>(static_cast<double>(Size) * WinSizeX * kHalf);
    float const HalfH = static_cast<float>(static_cast<double>(Size) * WinSizeY * kHalf);

    // Corner 0: identity, upper left.
    Win3DIdentityRotation3x3(&CornerXform[0].Rotation.D[0][0]);
    CornerXform[0].Translation.X = Centre.X + -HalfW;
    CornerXform[0].Translation.Y = Centre.Y + HalfH;
    CornerXform[0].Translation.Z = Centre.Z + 0.0f;

    Vector Angles;
    Matrix Rotation;

    // Corner 1: turned 180 about Y, upper right.
    Angles.x = 0.0f;
    Angles.y = 180.0f;
    Angles.z = 0.0f;
    Rotation = EulerMatrix(Angles);
    memcpy(&CornerXform[1].Rotation, &Rotation, sizeof(FMatrix));
    CornerXform[1].Translation.X = static_cast<float>(static_cast<double>(Centre.X) + HalfW);
    CornerXform[1].Translation.Y = static_cast<float>(static_cast<double>(Centre.Y) + HalfH);
    CornerXform[1].Translation.Z = Centre.Z;

    // Corner 2: turned 180 about X, lower left.
    Angles.x = 180.0f;
    Angles.y = 0.0f;
    Angles.z = 0.0f;
    Rotation = EulerMatrix(Angles);
    memcpy(&CornerXform[2].Rotation, &Rotation, sizeof(FMatrix));
    CornerXform[2].Translation.X = static_cast<float>(static_cast<double>(-HalfW) + Centre.X);
    CornerXform[2].Translation.Y = static_cast<float>(static_cast<double>(-HalfH) + Centre.Y);
    CornerXform[2].Translation.Z = Centre.Z;

    // Corner 3: turned 180 about X and Y, lower right.
    Angles.x = 180.0f;
    Angles.y = 180.0f;
    Angles.z = 0.0f;
    Rotation = EulerMatrix(Angles);
    memcpy(&CornerXform[3].Rotation, &Rotation, sizeof(FMatrix));
    CornerXform[3].Translation.X = static_cast<float>(static_cast<double>(Centre.X) + HalfW);
    CornerXform[3].Translation.Y = static_cast<float>(static_cast<double>(-HalfH) + Centre.Y);
    CornerXform[3].Translation.Z = Centre.Z;

    // ICamera slot 17 on corners 0 and 3, truncated (_ftol).
    float Projected0[3];
    float Projected3[3];
    Cam->ProjectVec(Projected0, &CornerXform[0].Translation.X);
    Cam->ProjectVec(Projected3, &CornerXform[3].Translation.X);
    Rect[0] = static_cast<int>(Projected0[0]);
    Rect[1] = static_cast<int>(Projected0[1]);
    Rect[2] = static_cast<int>(Projected3[0]);
    Rect[3] = static_cast<int>(Projected3[1]);
}

void FCommWindow::DrawCorners()
{
    // .
    EngineLightClearRegisteredList();
    unsigned char const White[3] = {0xff, 0xff, 0xff};
    EngineLightSetAmbientRgbBytes(White);
    if (!ClientMeshHolderHasInstance(&Corner))
    {
        return;
    }
    Win3DApplyCamera(AsCamera(Win3DGetCamera()));
    for (int Index = 0; Index < 4; ++Index)
    {
        ClientMeshHolderSetMatrix(&Corner, &CornerXform[Index]);
        ClientMeshHolderRender(&Corner, Win3DGetCamera(), 0x100);
    }
}

// ============================================================================================
// FCommChannel
// ============================================================================================

FCommChannel::FCommChannel()
    : Body(nullptr),
      bNoPortrait(0),
      NameIds(0),
      Linger(0.0f),
      CommText(nullptr),
      bWasBusy(0),
      CompleteSender(0),
      CompleteReceiver(0),
      CompleteMsgId(0)
{
    // 0x004ca7ba..0x004ca7ea. SpeakerRotate and the two offsets are left alone by
    // baseline until HUD_Receiver's ctor copies its own in; the port zeroes them.
    Pad175[0] = Pad175[1] = Pad175[2] = 0;
    memset(&SpeakerRotate, 0, sizeof(SpeakerRotate));
    memset(&MaleOffset, 0, sizeof(MaleOffset));
    memset(&FemaleOffset, 0, sizeof(FemaleOffset));
}

FCommChannel::~FCommChannel()
{
    // 0x004ca962..0x004caa0f; the window's and the voice's destructors follow.
    if (CommText != nullptr)
    {
        Win3DDeleteTree(CommText); // , inlined
        CommText = nullptr;
    }
    StopStatic();
    StopLines();
    DropDucking();
    ResetState();
    Body = nullptr;
    bNoPortrait = 0;
    bWasBusy = 0;
    CompleteSender = 0;
    CompleteReceiver = 0;
    CompleteMsgId = 0;
}

void FCommChannel::Reset()
{
    // . baseline clears the watcher through BaseWatcher::set(0) here and through
    // `if (watched) set_pointer(0)` in the routine; both leave it empty.
    if (CommText != nullptr)
    {
        Win3DDeleteTree(CommText);
        CommText = nullptr;
    }
    StopStatic();
    StopLines();
    DropDucking();
    ResetState();
    Body = nullptr;
    bNoPortrait = 0;
    bWasBusy = 0;
    CompleteSender = 0;
    CompleteReceiver = 0;
    CompleteMsgId = 0;
}

void FCommChannel::PlayNoPortrait(FCommMessage const* Msg, bool bPositionalArg)
{
    // .
    if (CommText != nullptr)
    {
        Win3DDeleteTree(CommText);
        CommText = nullptr;
    }
    StopStatic();
    StopLines();
    DropDucking();
    ResetState();
    Body = nullptr;
    CompleteSender = 0;
    CompleteReceiver = 0;
    bNoPortrait = 0;
    bWasBusy = 0;
    CompleteMsgId = 0;
    Play(Msg, false, false, bPositionalArg, &CompleteSender, &CompleteReceiver, &CompleteMsgId);
    Linger = Msg->Linger;
}

void FCommChannel::Appearance(CObject* SpeakerObject, CObject* ReceiverUnused,
                              Costume SpeakerCostume, unsigned InNameIds,
                              FVoiceProfile* Profile, Win3D* Parent)
{
    // .
    (void)ReceiverUnused;
    if (CommText != nullptr)
    {
        Win3DDeleteTree(CommText);
        CommText = nullptr;
    }
    StopVoice();
    Body = nullptr;
    bNoPortrait = 0;
    bWasBusy = 0;
    CompleteSender = 0;
    CompleteReceiver = 0;
    CompleteMsgId = 0;
    Watchable const* const Watch = WatchableOf(SpeakerObject);
    if (Watch != Speaker.target())
    {
        Speaker.set(Watch); // set_pointer
    }
    NameIds = InNameIds;
    Window.bOpenRequest = 1;
    Window.bCloseRequest = 0;
    Window.bOpen = 1;

    // the head only above detail level 0 (NaN included goes without).
    if (static_cast<double>(GetPerfOptionSlot(5)) > 0.0)
    {
        Body = G_EngineCharacterCache.FindOrCreate(SpeakerCostume);
        bNoPortrait = 0;
        if (Body != nullptr)
        {
            int const Gender = GetCostumeDescriptions()->get_costume_gender(SpeakerCostume);
            FTransform Pose;
            Win3DIdentityRotation3x3(&Pose.Rotation.D[0][0]); // , then overwritten
            Pose.Translation.X = 0.0f;
            Pose.Translation.Y = 0.0f;
            Pose.Translation.Z = 0.0f;
            Pose.Rotation = SpeakerRotate;
            if (Gender == 1)
            {
                Pose.Translation = MaleOffset;
            }
            else if (Gender == 2)
            {
                Pose.Translation = FemaleOffset;
            }
            else
            {
                FDUMP(0x100002, "%s(%d) : *** WARNING: %s", kHudCommCpp, 0x523,
                      "Invalid gender for comm appearance\n");
                Pose.Translation = MaleOffset;
            }
            // IDeformable slots 24 (only with an instance), 19 and 20 (always).
            if (Body->Deformable != nullptr)
            {
                NeuronRuntime::Deform->SetHeight(Body->Deformable, Pose.Translation.Y);
            }
            NeuronRuntime::Deform->SetPosePos(Body->Deformable, &Pose.Translation.X);
            NeuronRuntime::Deform->SetPoseOri(Body->Deformable, &Pose.Rotation);

            // one pass over the voice's scripts (VoiceProfile vt+0x18).
            std::list<std::string>& Scripts = Profile->GetScripts();
            for (std::list<std::string>::iterator It = Scripts.begin(); It != Scripts.end(); ++It)
            {
                char const* const ScriptName = It->c_str();
                FTransform Root;
                Body->GetRootTransform(&Root); // inlined: slot 18, IEngine slot 40
                Body->RootTransform = Root;
                if (ScriptName[0] != 0)
                {
                    int PlaybackSlot = -1;
                    for (int Index = 0; Index < 4; ++Index)
                    {
                        if (Body->AnimIds[Index] == -1)
                        {
                            PlaybackSlot = Index;
                            break;
                        }
                    }
                    // No free slot leaves -1 and baseline writes the name one slot below the
                    // array; unreachable while nothing below fills a slot.
                    Body->AnimNames[PlaybackSlot] = ScriptName;
                    // the script from gf\CharacterInfo.cpp's database
                    // (the routine, the routine on the stored name), then IDeformable slot 7
                    // (instance, &AnimIds[slot], script, name, 0, 0, 1.0f, 1.0f, 0, -1.0f, 0);
                    // non-zero -> FDUMP with the routine(0x10000, 2) = 0x100002.
                    void* const AnimScript =
                        GetCharacterInfoDb()->AcquireScriptByName(Body->AnimNames[PlaybackSlot].Data);
                    if (NeuronRuntime::Deform->Play(Body->Deformable, &Body->AnimIds[PlaybackSlot], AnimScript,
                                            const_cast<char*>(ScriptName), nullptr, nullptr,
                                            FloatArg(1.0f), FloatArg(1.0f), nullptr,
                                            FloatArg(-1.0f), nullptr) != 0)
                    {
                        unsigned int Flags = 0;
                        PackNibbleByte(&Flags, 0x10000, 2);
                        FDUMP(Flags, "%s(%d) : *** WARNING: failed to play character animation '%s'\n",
                              "cmn\\EngineCharacter.h", 0x1f7, ScriptName);
                    }
                }
            }

            // settle the pose.
            float Remaining = kAppearanceWarmUp;
            double RemainingExact = 0.0;
            do
            {
                float const Step = Remaining < kAdvanceStep ? Remaining : kAdvanceStep;
                Body->Advance(Step);
                RemainingExact = static_cast<double>(Remaining) - Step;
                Remaining = static_cast<float>(RemainingExact);
            } while (RemainingExact > 0.0);

            Body->StopAnimations(); // , inlined

            // the second pass.
            for (std::list<std::string>::iterator It = Scripts.begin(); It != Scripts.end(); ++It)
            {
                char const* const ScriptName = It->c_str();
                FTransform Root;
                Body->GetRootTransform(&Root);
                Body->RootTransform = Root;
                if (ScriptName[0] != 0)
                {
                    int PlaybackSlot = -1;
                    for (int Index = 0; Index < 4; ++Index)
                    {
                        if (Body->AnimIds[Index] == -1)
                        {
                            PlaybackSlot = Index;
                            break;
                        }
                    }
                    Body->AnimNames[PlaybackSlot] = ScriptName;
                    // as above with (instance, &AnimIds[slot], script,
                    // name, 0, 0.3f, 1.0f, 1.0f, 2, -1.0f, 0) and FDUMP 0x100002.
                    void* const AnimScript =
                        GetCharacterInfoDb()->AcquireScriptByName(Body->AnimNames[PlaybackSlot].Data);
                    if (NeuronRuntime::Deform->Play(Body->Deformable, &Body->AnimIds[PlaybackSlot], AnimScript,
                                            const_cast<char*>(ScriptName), nullptr,
                                            FloatArg(0.3f), FloatArg(1.0f), FloatArg(1.0f),
                                            reinterpret_cast<void*>(static_cast<uintptr_t>(2)),
                                            FloatArg(-1.0f), nullptr) != 0)
                    {
                        FDUMP(0x100002, "%s(%d) : *** WARNING: failed to play character animation '%s'\n",
                              "cmn\\EngineCharacter.h", 0x1f7, ScriptName);
                    }
                }
            }
        }
    }
    else
    {
        Body = nullptr;
        bNoPortrait = 1;
    }

    // the caption control.
    if (CommText == nullptr)
    {
        // "TextControl" is the WINFACTORY<NnTextControl> static (nn_text_control.cpp),
        // registered before WinMain as in baseline.
        CommText = static_cast<NnTextControl*>(WinFactoryCreate("CommText", "TextControl", Parent,
                                                                nullptr));
    }
    if (CommText == nullptr)
    {
        return;
    }
    CommText->Flags = static_cast<unsigned char>(CommText->Flags | 3);
    float const TextPos[3] = {-0.51f, 0.229f, -1.0f};
    CommText->Init("Normal", TextPos, nullptr, 0.129f, 0.06f, 1.0f, 0, 1, 0, 1);
    FFontString* const Font = CommText->GetFontString();
    if (Font->bMultiLine != 1)
    {
        Font->bMultiLine = 1;
        Font->LayoutGen = 0;
    }
    CommText->SetProperty(0x33, reinterpret_cast<uintptr_t>(G_HudChromeColorRgba), 1); // vt+0xa8

    // pilot name, group name or object name, then NameIds.
    wchar_t IdsText[0x100];
    G_NnTextScratchB[0] = 0;
    int LineCount = 0;
    CSimple* const Watched = SimpleOfWatchable(Speaker.target());
    if (Watched != nullptr)
    {
        CObject* const Object = ObjectOfSimple(Watched);
        FHudWideText Pilot = {nullptr, 0};
        if (GetShipPilotName(Object, &Pilot) != 0)
        {
            wcscat(G_NnTextScratchB, Pilot.Text);
            LineCount = 1;
        }
        CSimple* const Again = SimpleOfWatchable(Speaker.target());
        if (Again != nullptr &&
            (static_cast<unsigned>(Again->ObjectClass) & 0x503u) == 0x503u)
        {
            unsigned const GroupIds = static_cast<CShip const*>(ObjectOfSimple(Again))->get_group_name();
            if (GroupIds != 0)
            {
                if (GetIdsStringWWithRawFallback(GroupIds, IdsText, 0x100) == 0)
                {
                    IdsText[0] = 0;
                }
                if (LineCount > 0)
                {
                    wcscat(G_NnTextScratchB, L"\n");
                }
                wcscat(G_NnTextScratchB, IdsText);
                ++LineCount;
            }
        }
        if (LineCount == 0)
        {
            unsigned const NameIdsOfObject =
                GetObjectIdsName(ObjectOfSimple(SimpleOfWatchable(Speaker.target())));
            if (NameIdsOfObject != 0)
            {
                if (GetIdsStringWWithRawFallback(NameIdsOfObject, IdsText, 0x100) == 0)
                {
                    IdsText[0] = 0;
                }
                wcscat(G_NnTextScratchB, IdsText);
                LineCount = 1;
            }
        }
        if (Pilot.Text != nullptr)
        {
            free(Pilot.Text);
        }
    }
    if (NameIds != 0 && LineCount < 2)
    {
        if (LineCount > 0)
        {
            wcscat(G_NnTextScratchB, L"\n");
        }
        if (GetIdsStringWWithRawFallback(NameIds, IdsText, 0x100) == 0)
        {
            IdsText[0] = 0;
        }
        wcscat(G_NnTextScratchB, IdsText);
        ++LineCount;
    }
    if (LineCount > 0)
    {
        CommText->SetText(G_NnTextScratchB);
    }
}

bool FCommChannel::TickVoice(float Dt)
{
    // .
    bool const bNoLines = Lines.empty();
    if ((bWatchSpeaker != 0 && SimpleOfWatchable(Speaker.target()) == nullptr) || bNoLines)
    {
        StopVoice();
    }
    else
    {
        FCommLine* const Current = &Lines[Cursor];
        float PanValue = 0.0f;
        if (!GetSpaceScriptCameraLatch() && bPositional == 1)
        {
            ICamera* const Listener = static_cast<ICamera*>(GetClientSoundListenerObject());
            CSimple const* const SpeakerSimple = SimpleOfWatchable(Speaker.target());
            if (SpeakerSimple != nullptr && Listener != nullptr)
            {
                // the speaker in the listener's frame (ICamera slot 4:
                // nine rotation floats, then the position).
                Vector const SpeakerPos = SpeakerSimple->WorldPosition;
                float View[12];
                memcpy(View, Listener->GetOrientation(), sizeof(View));
                float const DeltaX = SpeakerPos.x - View[9];
                float const DeltaY = SpeakerPos.y - View[10];
                float const DeltaZ = SpeakerPos.z - View[11];
                // / the routine / the routine take the columns, the routine is
                // z*z' + y*y' + x*x'.
                float const X = static_cast<float>(static_cast<double>(DeltaZ) * View[6] +
                                                   static_cast<double>(DeltaY) * View[3] +
                                                   static_cast<double>(DeltaX) * View[0]);
                float const Y = static_cast<float>(-(static_cast<double>(DeltaZ) * View[7] +
                                                     static_cast<double>(DeltaY) * View[4] +
                                                     static_cast<double>(DeltaX) * View[1]));
                float const Z = static_cast<float>(static_cast<double>(DeltaZ) * View[8] +
                                                   static_cast<double>(DeltaY) * View[5] +
                                                   static_cast<double>(DeltaX) * View[2]);
                PanValue = Pan(X, Y, Z);
            }
        }
        if (Current->Source != nullptr)
        {
            Current->Source->SetPan(PanValue); // vt+0x84
            Current->Source->SetAttenuation(
                static_cast<float>(static_cast<double>(DuckDb) + Attenuation)); // vt+0x80
        }
        if (Current->Source != nullptr && Current->Source->IsFinished() != 0) // vt+0x74
        {
            ++Cursor;
            if (Cursor == Lines.size())
            {
                StopVoice();
            }
            else
            {
                PlayLine(&Lines[Cursor]);
            }
        }
        else if (Timer > 0.0f)
        {
            Timer = static_cast<float>(static_cast<double>(Timer) - Dt);
            if (Timer < 0.0f)
            {
                StopVoice();
            }
        }
    }

    // .
    bool bBusy = Window.bOpen != 0 && Window.bCloseRequest == 0;
    if (bWatchSpeaker == 0 || SimpleOfWatchable(Speaker.target()) != nullptr)
    {
        bBusy = true;
    }
    else if (!bBusy)
    {
        if (Linger > 0.0f)
        {
            Linger = static_cast<float>(static_cast<double>(Linger) - Dt);
        }
        bBusy = Linger > 0.0f;
    }
    bool const bFinished = !bBusy;
    if (bWasBusy == 1)
    {
        if (!bBusy)
        {
            bWasBusy = 0;
            if (SinglePlayer())
            {
                G_Server->CommComplete(CompleteSender, CompleteReceiver, CompleteMsgId,
                                       CommResult::SUCCESS); // IServer vt+0xfc
            }
        }
    }
    else if (bBusy)
    {
        bWasBusy = 1;
    }
    return bFinished;
}

void FCommChannel::CloseWindow()
{
    Window.bOpenRequest = 0;
    Window.bCloseRequest = 1;
    Body = nullptr;
    if (CommText != nullptr)
    {
        CommText->Flags = static_cast<unsigned char>(CommText->Flags & 0xfc);
    }
}

void FCommChannel::WindowTick(float Dt, FGameCamera* Cam)
{
    // .
    if (Window.bOpen == 0)
    {
        return;
    }
    if (Window.bOpenRequest != 0)
    {
        Window.OpenFraction = 1.0f;
        Window.bOpenRequest = 0;
        Window.bOpen = 1;
    }
    else if (Window.bCloseRequest != 0)
    {
        Window.OpenFraction = 0.0f;
        Window.bCloseRequest = 0;
        Window.bOpen = 0;
    }
    Window.Layout(AsCamera(Win3DGetCamera())); // &the field

    // Cam's viewport is the screen rect of corners 0 and 3.
    FVector const Upper = Window.CornerXform[0].Translation;
    FVector const Lower = Window.CornerXform[3].Translation;
    int Projected[4];
    int const* const Screen = HudProjectScreenRect(Projected, &Upper, &Lower);
    int ViewRect[4] = {Screen[0], Screen[1], Screen[2], Screen[3]};
    SpaceCameraSetViewportEx(Cam, ViewRect, 1);

    if (Window.bOpen == 0)
    {
        return;
    }
    if (Body != nullptr && Dt > 0.0f)
    {
        float Remaining = Dt;
        double RemainingExact = 0.0;
        do
        {
            float const Step = Remaining < kAdvanceStep ? Remaining : kAdvanceStep;
            Body->Advance(Step);
            RemainingExact = static_cast<double>(Remaining) - Step;
            Remaining = static_cast<float>(RemainingExact);
        } while (RemainingExact > 0.0);
    }
    if (bWatchSpeaker != 0 && SimpleOfWatchable(Speaker.target()) == nullptr && Window.bOpen != 0 &&
        Window.bCloseRequest == 0)
    {
        CloseWindow();
    }
}

void FCommChannel::Cancel()
{
    // .
    if (Window.bOpen != 0 && Window.bCloseRequest == 0)
    {
        CloseWindow();
    }
    if (SinglePlayer())
    {
        G_Server->CommComplete(CompleteSender, CompleteReceiver, CompleteMsgId,
                               CommResult::CANCELLED);
    }
    StopVoice(); // inlined
}

void FCommChannel::DrawFrame()
{
    // .
    Win3DDisable(0xde1);
    Win3DDisable(0xb44);
    Win3DDisable(0xb71);
    Win3DDisable(0xb60);
    ICamera* const Cam = AsCamera(Win3DGetCamera());
    Win3DSetViewport(Cam, kHudCommCpp, 0x660);
    Win3DApplyCamera(Cam);
    EngineLightClearRegisteredList();
    NeuronRuntime::Pipeline->SetRenderState(D3DRS_AMBIENT, -1);
    NeuronRuntime::Pipeline->SetRenderState(D3DRS_CULLMODE, 1);

    float const WinX = Window.WinPos.X;
    float const WinY = Window.WinPos.Y;
    float const WinZ = Window.WinPos.Z;
    float Model[12];
    Win3DIdentityRotation3x3(Model);
    Model[9] = 0.0f;
    Model[10] = 0.0f;
    Model[11] = 0.0f;
    Win3DBindModelview(nullptr, Model);

    Win3DDisable(0xb71);
    Win3DDisable(0xb44);
    Win3DDisable(0xde1);
    Win3DEnable(0xbe2);
    Win3DSetBlendFunc(2, 1);
    Win3DBegin(2);
    Win3DColor4ubv(G_HudChromeColorRgba);

    // the top-left corner.
    float const Left = WinX - kFrameInset;
    float const Top = WinY + kFrameInset;
    float const Right = Left + kFrameSize;
    float const Bottom = Top - kFrameSize;
    Win3DVertex3f(Left, Top, WinZ);
    Win3DVertex3f(Right, Top, WinZ);
    Win3DVertex3f(Right, Top, WinZ);
    Win3DVertex3f(Right, Bottom, WinZ);
    Win3DVertex3f(Right, Bottom, WinZ);
    Win3DVertex3f(Left, Bottom, WinZ);
    Win3DVertex3f(Left, Bottom, WinZ);
    Win3DVertex3f(Left, Top, WinZ);
    Win3DEnd(); // tail jump the routine
}

void FCommChannel::DrawPortrait(FGameCamera* Cam, FEngineLight* Light, void* BackdropTexture)
{
    // .
    ICamera* const CamIface = AsCamera(Cam);
    Win3DDisable(0xb71);
    Win3DDisable(0xbe2);
    Win3DApplyCamera(CamIface);
    Win3DSetViewport(CamIface, kHudCommCpp, 0x6a1);
    Win3DDisable(0xbe2);
    Win3DBindTexture(BackdropTexture);
    Win3DColor3ub(0x80, 0x80, 0xc8);
    Win3DBegin(8);
    Win3DTexCoord2f(0.0f, 0.0f);
    Win3DVertex3f(-kPortraitHalf, kPortraitHalf, kPortraitZ);
    Win3DTexCoord2f(kPortraitU1, 0.0f);
    Win3DVertex3f(kPortraitHalf, kPortraitHalf, kPortraitZ);
    Win3DTexCoord2f(kPortraitU1, kPortraitV1);
    Win3DVertex3f(kPortraitHalf, -kPortraitHalf, kPortraitZ);
    Win3DTexCoord2f(0.0f, kPortraitV1);
    Win3DVertex3f(-kPortraitHalf, -kPortraitHalf, kPortraitZ);
    Win3DEnd();

    unsigned char const Dim[3] = {0x14, 0x14, 0x14};
    EngineLightSetAmbientRgbBytes(Dim);
    FEngineLight* const Lights[1] = {Light};
    EngineLightApplyList(Lights, 1, CamIface, nullptr);
    if (GetGameOption(0x18) == 0)
    {
        unsigned char const Full[3] = {0xff, 0xff, 0xff};
        EngineLightSetAmbientRgbBytes(Full);
    }
    Win3DEnable(0xb44);
    Win3DEnable(0xb71);
    Win3DSetDepthFunc(4);
    Win3DSetDepthWrite(1);

    if (Body != nullptr && Body->Deformable != nullptr)
    {
        int Zeros[5] = {0, 0, 0, 0, 0};
        int OutLod = 0;
        NeuronRuntime::Deform->SetLod(Body->Deformable, 0.0f, Zeros);                  // slot 15
        NeuronRuntime::Deform->Draw(Body->Deformable, Cam, Zeros, nullptr, &OutLod, 0); // slot 16
        NeuronRuntime::MaterialBatcher->FlushAll(1);                                   // slot 10
    }
    EngineLightClearRegisteredList();
}

void CommDrawStatic(int const* Rect)
{
    // .
    FTextureRef* const StaticTexture = GetHudStaticTexture();
    if (StaticTexture->Texture == nullptr)
    {
        TextureRefLoad(StaticTexture, "static");
    }
    Win3DSetOrtho2D();
    Win3DBindTexture(TexShapeGetGpuHandle(StaticTexture, 0));
    Win3DDisable(0xb71);
    Win3DDisable(0xb44);
    Win3DEnable(0xbe2);
    Win3DColor3ub(kStaticBrightness, kStaticBrightness, kStaticBrightness);
    switch (GetHudStaticBlend()) // , jump table 0x004ccba8
    {
    case 1:
        Win3DSetBlendFunc(5, 6);
        break;
    case 2:
        Win3DSetBlendFunc(5, 2);
        break;
    case 3:
        Win3DSetBlendFunc(3, 6);
        break;
    case 4:
        Win3DSetBlendFunc(3, 2);
        break;
    case 5:
        Win3DSetBlendFunc(2, 6);
        break;
    case 6:
        Win3DColor3ub(0x80, 0x80, 0x80);
        Win3DSetBlendFunc(2, 2);
        break;
    case 7:
        Win3DDisable(0xbe2);
        Win3DSetBlendFunc(3, 4);
        break;
    default:
        return;
    }
    int const Random = rand();
    float const U = static_cast<float>(static_cast<double>(Random & 0xff) * kStaticUvStep);
    float const V = static_cast<float>(static_cast<double>((Random >> 4) & 0xfc) * kStaticUvStep);
    Win3DBegin(8);
    Win3DTexCoord2f(U, V);
    Win3DVertex3f(static_cast<float>(Rect[0]), static_cast<float>(Rect[1]), 0.0f);
    float const U1 = U + 1.0f;
    Win3DTexCoord2f(U1, V);
    Win3DVertex3f(static_cast<float>(Rect[2]), static_cast<float>(Rect[1]), 0.0f);
    float const V1 = V + 1.0f;
    Win3DTexCoord2f(U1, V1);
    Win3DVertex3f(static_cast<float>(Rect[2]), static_cast<float>(Rect[3]), 0.0f);
    Win3DTexCoord2f(U, V1);
    Win3DVertex3f(static_cast<float>(Rect[0]), static_cast<float>(Rect[3]), 0.0f);
    Win3DEnd();
}

// ============================================================================================
// HUD_Receiver
// ============================================================================================

HUD_Receiver::HUD_Receiver(INI_Reader& Reader) : Light(nullptr), LowEndCommButton(nullptr)
{
    // .
    SlotA.Inbox = nullptr;
    SlotA.Channel = nullptr;
    SlotA.Priority = 0;
    SlotA.bActive = 0;
    SlotA.bDeferred = 0;
    SlotA.Pad0e[0] = SlotA.Pad0e[1] = 0;
    SlotB = SlotA;
    // baseline leaves these until the ini sets them; the port zeroes them.
    memset(&SpeakerRotate, 0, sizeof(SpeakerRotate));
    memset(&MaleOffset, 0, sizeof(MaleOffset));
    memset(&FemaleOffset, 0, sizeof(FemaleOffset));
    GameCameraConstruct(&Camera);

    Win3DReparent(this, nullptr);
    Win3DRegister(this);
    Flags = static_cast<unsigned char>((Flags & 0xfb) | 3);
    HUDItem::Name = "RadioReceiver"; // (+0x84, 0x20, ...)
    Light = nullptr;
    // the two stores, nothing else.
    Camera.ZNear = 1.4f;
    Camera.ZFar = 50.0f;

    while (Reader.read_value())
    {
        if (ChannelA.Window.ReadIni(Reader))
        {
            continue;
        }
        if (Reader.is_value("speaker_offset"))
        {
            FDUMP(0x100002,
                  "%s(%d) : *** WARNING: speaker_offset is obsolete in hud ini file %s, use "
                  "male_speaker_offset or female_speaker_offset instead\n",
                  kHudCommCpp, 0x7e7, Reader.get_file_name());
        }
        else if (Reader.is_value("male_speaker_offset"))
        {
            float const X = Reader.get_value_float(0);
            float const Y = Reader.get_value_float(1);
            float const Z = Reader.get_value_float(2);
            MaleOffset.X = X;
            MaleOffset.Y = Y;
            MaleOffset.Z = Z;
        }
        else if (Reader.is_value("female_speaker_offset"))
        {
            float const X = Reader.get_value_float(0);
            float const Y = Reader.get_value_float(1);
            float const Z = Reader.get_value_float(2);
            FemaleOffset.X = X;
            FemaleOffset.Y = Y;
            FemaleOffset.Z = Z;
        }
        else if (Reader.is_value("speaker_rotate"))
        {
            Vector Angles;
            Angles.x = Reader.get_value_float(0);
            Angles.y = Reader.get_value_float(1);
            Angles.z = Reader.get_value_float(2);
            Matrix const Rotation = EulerMatrix(Angles);
            memcpy(&SpeakerRotate, &Rotation, sizeof(FMatrix));
        }
        else if (Reader.is_value("video_fovx"))
        {
            SpaceCameraSetHalfFovX(&Camera, Reader.get_value_float(0));
        }
        else if (Reader.is_value("backdrop"))
        {
            TextureRefLoad(&Backdrop, Reader.get_value_string());
        }
    }

    // the portrait key light.
    FEngineLight* const NewLight = static_cast<FEngineLight*>(operator new(sizeof(FEngineLight)));
    EngineLightInit(NewLight, -1);
    EngineLightSetType(NewLight, 1, false);
    float const LightPos[3] = {0.23f, 0.31f, -0.92f};
    EngineLightSetPosition(NewLight, LightPos);
    unsigned char const LightColour[3] = {0x80, 0x80, 0x80};
    EngineLightSetColorBytesAlt(NewLight, LightColour);
    EngineLightSetRadius(NewLight, 1.0e9f);
    if (EngineLightRegister(NewLight))                  // vt+4
    {
        Light = NewLight;
    }
    else
    {
        EngineLightDelete(NewLight); // vt+0 (1)
        Light = nullptr;
    }

    // channel A takes the receiver's pose, and its window a fixed spot.
    ChannelA.SpeakerRotate = SpeakerRotate;
    ChannelA.MaleOffset = MaleOffset;
    ChannelA.FemaleOffset = FemaleOffset;
    ChannelA.Window.WinPos.X = -1.068f;
    ChannelA.Window.WinPos.Y = 0.65f;
    ChannelA.Window.WinPos.Z = -2.392f;

    SlotA.Channel = &ChannelA;
    SlotA.Inbox = &G_CommInboxPlayer;
    SlotB.Inbox = &G_CommInboxOther;
    SlotB.Channel = &ChannelB;
    G_CommDuck.TargetDb = 0.0f;
    G_CommDuck.CurrentDb = 0.0f;
    G_CommDuck.FadeRemaining = -1.0f;
    G_CommDuck.RefCount = 0;
    SetHudCommInstance(this);

    // the low-detail comm button.
    LowEndCommButton = WinFactoryCreate("LowEndCommButton", "ButtonControl", this, nullptr);
    if (LowEndCommButton != nullptr)
    {
        // ButtonControl::Init the routine("interface\\HUD\\HUD_genericcomm.3db",
        // 0, 0, 0, &{-0.1617, 0.0986, -0.362}, 0, &{0, 0, 0}, 0, 0, 0, 0, 0, 0, 0).
        float const CommButtonOffset[3] = {-0.1617f, 0.0986f, -0.362f};
        float const CommButtonHalfExtents[3] = {0.0f, 0.0f, 0.0f};
        static_cast<NnButtonControl*>(LowEndCommButton)
            ->Init("interface\\HUD\\HUD_genericcomm.3db", nullptr, nullptr, nullptr,
                   CommButtonOffset, nullptr, CommButtonHalfExtents, 0, 0, 0, 0, 0, 0, 0);
        LowEndCommButton->Flags = static_cast<unsigned char>(LowEndCommButton->Flags & 0xfc);
    }
}

HUD_Receiver::~HUD_Receiver()
{
    // .
    Win3DDeleteTree(LowEndCommButton);
    if (Backdrop.Texture != nullptr)
    {
        NeuronRuntime::TextureLib->ReleaseTextureRecord(Backdrop.Texture); // slot +0x38
        Backdrop.Texture = nullptr;
    }
    if (Light != nullptr)
    {
        EngineLightDelete(Light);
    }
    Light = nullptr;
    G_EngineCharacterCache.DeleteAll();
    SetHudCommInstance(nullptr);
    Camera.Watched.set(nullptr);        // , inlined
    // The members follow: Backdrop (nothing left to release), channel B (the routine,
    // , the routine, the vector, the watcher), channel A, then HUDItem's watcher and
    // Win3D's destructor.
}

bool HUD_Receiver::Init()
{
    // .
    G_CommInboxPlayer.Count = 0;
    G_CommInboxOther.Count = 0;
    return true;
}

void HUD_Receiver::UpdateInactive()
{
    // JMP [EAX+0x2c].
    Update();
}

void HUD_Receiver::Update()
{
    // . No null test on the button.
    if (ChannelA.Window.bOpen != 0 &&
        (!(static_cast<double>(GetPerfOptionSlot(5)) > 0.0) || ChannelA.bNoPortrait == 1))
    {
        LowEndCommButton->Flags = static_cast<unsigned char>(LowEndCommButton->Flags | 3);
    }
    else
    {
        LowEndCommButton->Flags = static_cast<unsigned char>(LowEndCommButton->Flags & 0xfc);
    }
    if (ShouldSkipLocalServerTick())
    {
        return;
    }
    // Win3D's child walk, the next sibling read first.
    Win3D* Child = ChildHead;
    while (Child != nullptr)
    {
        Win3D* const Next = Child->NextSibling;
        if ((Child->Flags & 1) != 0)
        {
            Child->Update();
        }
        else
        {
            Child->UpdateInactive();
        }
        Child = Next;
    }

    float const Dt = static_cast<float>(GetClientFrameTimeCapped());
    // Slot A is the routine inlined (0x004cda6a..0x004cdbad), with the routine called for the
    // head's priority where the out-of-line copy inlines it; the two bodies are the same.
    UpdateSlot(&SlotA, Dt);
    UpdateSlot(&SlotB, Dt);
    ClientSoundFadeDb(G_CommDuck.CurrentDb, G_CommDuck.TargetDb, Dt, &G_CommDuck.FadeRemaining,
                      &G_CommDuck.CurrentDb);
    if (SlotB.Channel != nullptr)
    {
        SlotB.Channel->DuckDb = G_CommDuck.CurrentDb;
    }
    ChannelA.WindowTick(Dt, &Camera);
}

void HUD_Receiver::Draw()
{
    // .
    if (IsLetterboxVisible())
    {
        return;
    }
    if (IsNeuroNetNavMapOpen())
    {
        return;
    }
    if (Win3DFindByName("NN_Trade") != nullptr)
    {
        return;
    }
    void* const BackdropTexture = TexShapeGetGpuHandle(&Backdrop, 0);
    FEngineLight* const PortraitLight = Light;
    if (ChannelA.Window.bOpen != 0)
    {
        ChannelA.DrawFrame();
        if (static_cast<double>(GetPerfOptionSlot(5)) > 0.0)
        {
            ICamera* const Win3DCam = AsCamera(Win3DGetCamera());
            if (ChannelA.Body != nullptr && ChannelA.Body->Deformable != nullptr)
            {
                ChannelA.DrawPortrait(&Camera, PortraitLight, BackdropTexture);
                Win3DSetViewport(Win3DCam, kHudCommCpp, 0x6ee);
            }
            Win3DSetViewport(Win3DCam, kHudCommCpp, 0xbd8);
            ChannelA.Window.DrawCorners();
            Win3DSetOrtho2D();
            Win3DDisable(0xb44);
            if (ChannelA.Window.bStatic != 0)
            {
                CommDrawStatic(ChannelA.Window.Rect);
            }
        }
        ICamera* const Win3DCam = AsCamera(Win3DGetCamera());
        Win3DSetViewport(Win3DCam, kHudCommCpp, 0x6f5);
        Win3DApplyCamera(Win3DCam);
    }
    Win3DDrawChildren(this); // tail jump the routine
}

int HUD_Receiver::MaxVoicePriority(FCommMessage const* Msg)
{
    // .
    int Best = -100;
    FVoiceProfile* const Profile = LookupClientVoice(Msg->VoiceId);
    if (Profile == nullptr)
    {
        return -100;
    }
    for (int Index = 0; Index < Msg->TextCount; ++Index)
    {
        FVoiceMsg* const VoiceMsg = Profile->FindMessage(Msg->TextIds[Index]);
        if (VoiceMsg != nullptr)
        {
            int const Priority = VoiceMsg->GetPriority(); // VoiceMsg vt+0x38
            if (Priority > Best)
            {
                Best = Priority;
            }
        }
    }
    return Best;
}

void HUD_Receiver::CommCancelled(FCommMessage const* Msg)
{
    // .
    if (SinglePlayer())
    {
        G_Server->CommComplete(Msg->Sender, Msg->Receiver, LastTextId(Msg), Msg->Result);
    }
}

void HUD_Receiver::UpdateSlot(FCommSlot* Slot, float Dt)
{
    // .
    bool const bFinished = Slot->Channel->TickVoice(Dt);
    if (bFinished)
    {
        Slot->Priority = 0;
        Slot->bActive = 0;
    }
    FCommMessage Head;
    if (!Slot->Inbox->Peek(&Head)) // inlined
    {
        return;
    }
    int const Best = MaxVoicePriority(&Head); // inlined

    // a negative-priority head drops itself when something higher is
    // playing.
    bool bDropped = false;
    if (Best < 0 && Slot->bActive != 0 && Best < Slot->Priority)
    {
        FCommMessage Dropped;
        Slot->Inbox->Pop(&Dropped); // inlined
        bDropped = true;
        Dropped.Result = CommResult::CANCELLED;
        CommCancelled(&Dropped); // inlined
    }
    // a negative-priority message playing is cut by anything above it.
    if (Slot->bActive != 0 && Slot->Priority < 0 && Best > Slot->Priority)
    {
        Slot->Channel->Cancel(); // inlined, the routine at the end
    }
    if (bDropped)
    {
        return;
    }
    if (bFinished)
    {
        Slot->Inbox->Pop(&Head); // inlined, into the peek record
        Dispatch(Slot, &Head);
        Slot->bDeferred = 0;
    }
    else if (Best > COMM_CONFLICT_PRIORITY_CUTOFF)
    {
        if (Slot->bDeferred == 0)
        {
            Slot->bDeferred = 1;
        }
    }
    else
    {
        FCommMessage Dropped;
        Slot->Inbox->Pop(&Dropped);
        Dropped.Result = CommResult::CANCELLED;
        CommCancelled(&Dropped);
    }
}

void HUD_Receiver::Dispatch(FCommSlot* Slot, FCommMessage* Msg)
{
    // .
    FVoiceProfile* const Profile = LookupClientVoice(Msg->VoiceId);
    if (Profile == nullptr)
    {
        FDUMP(0x100002, "%s(%d) : *** WARNING: invalid VOICE id (%u) attempted to speak\n",
              kHudCommCpp, 0x934, Msg->VoiceId);
        return;
    }
    Msg->Attenuation = 0.0f;

    // the distance attenuation against the HUD's target (the player).
    Watchable const* const TargetWatch = HUDItem::Target.target();
    IObjRW* const TargetRw = ObjRwFromWatchable(const_cast<Watchable*>(TargetWatch));
    if (TargetWatch != nullptr && TargetRw != nullptr && Msg->Sender != 0)
    {
        CObject* const SenderObject = CommObjectOf(Msg->Sender);
        if (SenderObject == nullptr)
        {
            CommCancelled(Msg);
            return;
        }
        if (!IsPlayerObjRwId(Msg->Sender))
        {
            Vector const ObjectPos = SenderObject->WorldPosition;
            Vector const TargetPos = TargetRw->get_position(); // slot 0
            float const DeltaX = ObjectPos.x - TargetPos.x;
            float const DeltaY = ObjectPos.y - TargetPos.y;
            float const DeltaZ = ObjectPos.z - TargetPos.z;
            double const Length = sqrt(static_cast<double>(DeltaX) * DeltaX +
                                       static_cast<double>(DeltaY) * DeltaY +
                                       static_cast<double>(DeltaZ) * DeltaZ);
            float const Distance = static_cast<float>(Length - SenderObject->Radius);
            if (IsPlayerObjRwId(Msg->Receiver))
            {
                // FCOMP / TEST AH,0x41 / JNE: far from FAR_DIST on (NaN included).
                if (!(COMM_PLAYER_FAR_DIST > Distance))
                {
                    Msg->Attenuation = COMM_PLAYER_FAR_DIST_ATTEN;
                }
                else
                {
                    Msg->Attenuation = 0.0f;
                }
            }
            else
            {
                float MaxAtten = 0.0f;
                float StartAtten = 0.0f;
                float MaxDist = 0.0f;
                if (MaxVoicePriority(Msg) > WALLA_PRIORITY_CUTOFF)
                {
                    MaxDist = CHATTER_MAX_DIST;
                    StartAtten = CHATTER_START_ATTEN;
                    MaxAtten = CHATTER_MAX_DIST_ATTEN;
                }
                else
                {
                    MaxDist = WALLA_MAX_DIST;
                    StartAtten = WALLA_START_ATTEN;
                    MaxAtten = WALLA_MAX_DIST_ATTEN;
                }
                if (static_cast<double>(Distance) <= 0.0)
                {
                    Msg->Attenuation = StartAtten;
                }
                else if (Distance > MaxDist)
                {
                    CommCancelled(Msg);
                    return;
                }
                else
                {
                    Msg->Attenuation = static_cast<float>(
                        (static_cast<double>(MaxAtten) - StartAtten) *
                            (static_cast<double>(Distance) / MaxDist) +
                        StartAtten);
                }
            }
        }
    }

    // the priority of what is about to play.
    Slot->Priority = MaxVoicePriority(Msg); // inlined
    Slot->bActive = 1;

    if (!IsPlayerObjRwId(Msg->Receiver) && !IsPlayerObjRwId(Msg->Sender))
    {
        // someone else's chatter, positional, no window.
        FCommChannel* const Channel = Slot->Channel;
        Channel->Reset();
        Channel->Play(Msg, false, false, true, &Channel->CompleteSender, &Channel->CompleteReceiver,
                      &Channel->CompleteMsgId);
        Channel->Linger = Msg->Linger;
        return;
    }

    bool const bToPlayer = IsPlayerObjRwId(Msg->Receiver);
    bool const bFromPlayer = IsPlayerObjRwId(Msg->Sender);
    bool const bNoCostume = Msg->SenderCostume.PartIds[0] == 0 &&
                            Msg->SenderCostume.PartIds[1] == 0 &&
                            Msg->SenderCostume.PartIds[2] == 0 &&
                            Msg->SenderCostume.PartIds[3] == 0 &&
                            Msg->SenderCostume.AccessoryCount == 0;
    bool const bLetterbox = IsLetterboxVisible();
    if (bToPlayer && !bFromPlayer && !bNoCostume && !bLetterbox)
    {
        // the talking head.
        FCommChannel* const Channel = Slot->Channel;
        Channel->Reset();
        PlayOrRewindUiSoundSlot(0x2c);
        CObject* const SenderObject = CommObjectOf(Msg->Sender);
        CObject* const ReceiverObject = CommObjectOf(Msg->Receiver);
        Channel->Appearance(SenderObject, ReceiverObject, Msg->SenderCostume, Msg->CaptionIds,
                            Profile, this);
        Channel->Play(Msg, false, true, false, &Channel->CompleteSender, &Channel->CompleteReceiver,
                      &Channel->CompleteMsgId);
        Channel->Linger = Msg->Linger;
        return;
    }
    Slot->Channel->PlayNoPortrait(Msg, !bLetterbox);
}

// ============================================================================================
// Free functions
// ============================================================================================

void* GetHudCommInstance()
{
    return G_HudCommInstance;
}

void SetHudCommInstance(void* Instance)
{
    G_HudCommInstance = Instance;
}

bool HudCommIsWatchedObjectId(unsigned ObjectId)
{
    // . 0x004cc880..0x004cc888: nothing without the element.
    HUD_Receiver const* const Instance = static_cast<HUD_Receiver const*>(G_HudCommInstance);
    if (Instance == nullptr)
    {
        return false;
    }

    // the watcher at +0x284 (MOV EAX,[ECX+0x284]; ADD EAX,-0x9c; both
    // null-tested; CMP [EAX+0xb0],id).
    CSimple const* const First = SimpleOfWatchable(Instance->ChannelB.Speaker.target());
    if (First != nullptr && First->Id == ObjectId)
    {
        return true;
    }

    // the watcher at +0xb4, the same way.
    CSimple const* const Second = SimpleOfWatchable(Instance->ChannelA.Speaker.target());
    if (Second != nullptr && Second->Id == ObjectId)
    {
        return true;
    }
    return false;
}

bool HudCommIsWindowOpen()
{
    // the window byte masked by !the routine().
    HUD_Receiver const* const Instance = static_cast<HUD_Receiver const*>(G_HudCommInstance);
    if (Instance == nullptr)
    {
        return false;
    }
    unsigned char const bOpen = Instance->ChannelA.Window.bOpen;
    return !IsLetterboxVisible() && bOpen != 0;
}

void HudCommPreloadCostume(Costume const* SpeakerCostume)
{
    // .
    if (G_HudCommInstance == nullptr)
    {
        return;
    }
    if (G_EngineCharacterCache.FindOrCreate(*SpeakerCostume) == nullptr)
    {
        FDUMP(0x100002, "%s(%d) : *** WARNING: Failed to load costume %d/%d\n", kHudCommCpp, 0x119,
              SpeakerCostume->PartIds[1], SpeakerCostume->PartIds[0]);
    }
}

bool IsNeuroNetNavMapOpen()
{
    // .
    return Win3DFindByName("NeuroNetNavMap") != nullptr;
}
