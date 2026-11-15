// Author: Giperion.

#include "nn_messaging.h"

#include "menu_sound_arch.h"  // PlayClientSoundArchIfIdle (the routine), SetClientSoundSourceType
#include "player_behavior.h"  // GetPlayerObjRW (the routine)
#include "space_letterbox.h"  // IsLetterboxVisible (the routine)
#include "star_sys.h"         // StarSysHasVoiceMessage (the routine), StarSysPreloadVoiceMessage
#include "voice_profiles.h"   // LookupClientVoice (the routine), FVoiceProfile, FVoiceMsg

#include "common_id.h"        // CreateID
#include "neuroncomponents.h"            // FDUMP
#include "soundmanager.h"     // ISoundSource

#include <list>

namespace
{
    // One queue entry: the 12-byte payload of a list node (node +0x08..+0x13).
    struct FNnVoiceMessage
    {
        unsigned MsgId;         // +0x00
        unsigned char bKeyBit2; // +0x04
        ISoundSource* Source;   // +0x08

        FNnVoiceMessage(unsigned InMsgId, unsigned char bInKeyBit2)
            : MsgId(InMsgId), bKeyBit2(bInKeyBit2), Source(nullptr)
        {
        }

        // / the routine (flag 0) / the routine: stop (vt+0x60) and release
        // (vt+8) the playing source.
        ~FNnVoiceMessage()
        {
            if (Source != nullptr)
            {
                Source->Stop();
                Source->Release();
                Source = nullptr;
            }
        }

        FNnVoiceMessage(FNnVoiceMessage const&) = delete;
        FNnVoiceMessage& operator=(FNnVoiceMessage const&) = delete;
    };

    // constructed by the XC entry the branch (the routine), destroyed at exit
    // through the branch -> the routine. std::list keeps the same FIFO order and size count.
    std::list<FNnVoiceMessage> G_NnVoiceQueue;

    unsigned G_NnVoiceId = 0;
    FVoiceProfile* G_NnVoiceProfile = nullptr;

    // NN_Messaging.cpp's overflow limit (CMP EBP,0x4 / JGE at 0x00489b70).
    int const kNnVoiceQueueCapacity = 4;

    // (thiscall on the entry): start the entry's line through the NNVoice profile.
    void NnVoicePlay(FNnVoiceMessage* Entry)
    {
        if (IsLetterboxVisible())
        {
            return;
        }
        if (G_NnVoiceProfile == nullptr)
        {
            return;
        }
        // with a player ship, make sure the line is on the system's
        // preload list.
        if (GetPlayerObjRW() != nullptr)
        {
            if (!StarSysHasVoiceMessage(G_NnVoiceId, Entry->MsgId))
            {
                StarSysPreloadVoiceMessage(G_NnVoiceId, Entry->MsgId);
            }
        }
        FVoiceMsg* const Msg = G_NnVoiceProfile->FindMessage(Entry->MsgId); // profile vt+0x14
        if (Msg == nullptr)
        {
            return;
        }
        // the play key. Dword 1's low byte keeps its stale bits 3..7 and
        // takes the entry's flag in bit 2 (bits 0/1 cleared); the upper three bytes are stale.
        // The port has 0 where baseline has stack garbage. Dwords 0, 2..5 are 0.
        unsigned PlayKey[6] = {};
        PlayKey[1] = static_cast<unsigned>((Entry->bKeyBit2 << 2) & 4);
        void* Handle = nullptr;
        PlayClientSoundArchIfIdle(static_cast<Archetype::ISound*>(Msg), PlayKey, &Handle);
        // writes the out pointer only when it starts a source.
        if (Handle != nullptr)
        {
            Entry->Source = static_cast<ISoundSource*>(Handle);
        }
        // the source's category = 5 (voice).
        if (Entry->Source != nullptr)
        {
            SetClientSoundSourceType(Entry->Source, 5);
        }
    }

    // (thiscall on the field, RET 8).
    void NnVoiceQueuePush(unsigned MsgId, unsigned char bKeyBit2)
    {
        int const CountBefore = static_cast<int>(G_NnVoiceQueue.size());
        if (CountBefore >= kNnVoiceQueueCapacity)
        {
            FDUMP(0x100002, "%s(%d) : *** WARNING: %s",
                  "Client\\nn\\NN_Messaging.cpp", 0xa2,
                  "NN Message inbox overflow!");
            return;
        }
        // a message already queued is not queued twice.
        for (std::list<FNnVoiceMessage>::iterator It = G_NnVoiceQueue.begin();
             It != G_NnVoiceQueue.end(); ++It)
        {
            if (It->MsgId == MsgId)
            {
                return;
            }
        }
        // push_back of {MsgId, flag, null} (the routine + the routine);
        // the stack temporary's destructor at 0x00489bdd has no source to release.
        G_NnVoiceQueue.emplace_back(MsgId, bKeyBit2);
        // the queue was empty -- start the new head now.
        if (CountBefore == 0)
        {
            NnVoicePlay(&G_NnVoiceQueue.front());
        }
    }
}

void NnVoiceInit()
{
    // .
    G_NnVoiceId = CreateID("NNVoice");
    G_NnVoiceProfile = LookupClientVoice(G_NnVoiceId);
}

void NnVoiceTick(float Unused)
{
    // .
    (void)Unused;
    if (G_NnVoiceQueue.empty())
    {
        return;
    }
    // a head that is still playing stays (vt+0x74 IsFinished; NEG/SBB/
    // INC turns "not finished" into the early return).
    ISoundSource* const HeadSource = G_NnVoiceQueue.front().Source;
    if (HeadSource != nullptr && HeadSource->IsFinished() == 0)
    {
        return;
    }
    // unlink the head, the routine(0) on it, free the node.
    G_NnVoiceQueue.pop_front();
    // tail jump into the routine on the new head.
    if (!G_NnVoiceQueue.empty())
    {
        NnVoicePlay(&G_NnVoiceQueue.front());
    }
}

void NnVoiceEnqueue(unsigned MsgId, unsigned char bKeyBit2)
{
    // MOV ECX,the field; CALL the routine.
    NnVoiceQueuePush(MsgId, bKeyBit2);
}

void NnVoiceEnqueueByName(char const* Name)
{
    // .
    NnVoiceQueuePush(CreateID(Name), 0);
}
