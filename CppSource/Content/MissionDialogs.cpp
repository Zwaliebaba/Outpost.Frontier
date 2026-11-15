// Author: Giperion.

// TU28 (06ef2070-06ef3630): mission dialogs. See MissionDialogs.h.

#include "MissionDialogs.h"

#include "ContentStaticIds.h"   // TU28's 16 static-init thunks 06ef2070-06ef2250
#include "ContentPlayerState.h" // FindContentPlayerState (the routine), ...ByShip (the routine)
#include "SystemInfo.h"         // FSystemInfo (TU58), FindSystemInfo (the routine)
#include "common_ini.h"
#include "common_server.h"      // pub::GetSystemID, pub::GetNicknameId

#include <memory>
#include <utility>

// the routine on the field, atexit the routine (the map teardown).
std::map<unsigned int, FMissionDialog> GMissionDialogs;

// (06ef2380-06ef2608, RET 4). The id of the previous line (06ef23af, stack
// +0x10) starts at 0 and carries over from speaker to speaker.
void FMissionDialog::Load(INI_Reader* Reader)
{
    unsigned int PrevLineId = 0;
    while (Reader->read_value())
    {
        if (Reader->is_value("line"))
        {
            // the speaker name (the routine into an empty TString).
            TString<0x30> SpeakerName;
            SpeakerName.Length = 0;
            SpeakerName.Data[0] = '\0';
            SpeakerName = Reader->get_value_string(0);
            // find the speaker (the routine); a new one is inserted with an
            // empty vector (pair the routine, insert the routine) and found again.
            std::map<TString<0x30>, std::vector<FMissionDialogLine>, FMissionDialogSpeakerLess>::iterator
                It = Speakers.find(SpeakerName);
            if (It == Speakers.end())
            {
                std::vector<FMissionDialogLine> NoLines;
                Speakers.insert(std::pair<TString<0x30> const, std::vector<FMissionDialogLine>>(
                    SpeakerName, NoLines));
                It = Speakers.find(SpeakerName);
            }
            // the line: both names empty, +0x68/+0x69/+0x78 zero; the speaker
            // copied from the key (the routine, max 0x30), the target from value 1 (empty when
            // absent); +0x6c the previous id, +0x70 GetNicknameId(value 2), +0x68 set when the
            // previous id has none of its low 30 bits (AND 0x3fffffff; NEG; SBB; INC).
            FMissionDialogLine Line;
            Line.Speaker.Length = 0;
            Line.Speaker.Data[0] = '\0';
            Line.Target.Length = 0;
            Line.Target.Data[0] = '\0';
            Line.Field_0x69 = 0;
            Line.bFirst = 0;
            Line.Comm.Field_0x0c = 0;
            Line.Speaker = SpeakerName.Data;
            Line.Target = Reader->get_value_string(1);
            Line.Comm.Key = PrevLineId;
            Line.Comm.TextId = pub::GetNicknameId(Reader->get_value_string(2));
            Line.Comm.TargetId = 0;   // baseline leaves +0x74 unset until StartMissionDialog
            Line.bFirst = (PrevLineId & 0x3fffffff) == 0 ? 1 : 0;
            // the routine (insert 1 at the end).
            It->second.push_back(Line);
            PrevLineId = Line.Comm.TextId;
        }
        else if (Reader->is_value("system"))
        {
            // .
            unsigned int SystemId;
            pub::GetSystemID(SystemId, Reader->get_value_string(0));
            System = FindSystemInfo(&SystemId);
        }
    }
}

// (06ef2610-06ef29ff, RET 4). Locals: bResult (local_79, starts 1), Speaker
// (local_74, the speaker object of the last first line), TargetId / TextId (local_64 /
// local_58, that line's). The pending-comm vector (local_50) is the one EH state.
bool FMissionDialog::Start(unsigned int const* PlayerId)
{
    bool bResult = true;
    // a player in another system than the dialog's -> false.
    FContentPlayerState* PlayerState = FindContentPlayerState(*PlayerId);
    if (PlayerState != nullptr)
    {
        FSystemInfo* PlayerSystem = PlayerState->GetSystemRecord();
        if (PlayerSystem != nullptr && System != nullptr && PlayerSystem->SystemId != System->SystemId)
            return false;
    }
    if (System == nullptr)
        return bResult;

    FContentSpaceObj* Speaker = nullptr;
    unsigned int TargetId = 0;
    unsigned int TextId = 0;
    std::map<TString<0x30>, std::vector<FMissionDialogLine>, FMissionDialogSpeakerLess>::iterator
        It = Speakers.begin();
    for (; It != Speakers.end(); ++It)
    {
        std::vector<FSpaceObjPendingComm> Comms;
        std::vector<FMissionDialogLine>::iterator LineIt = It->second.begin();
        for (; LineIt != It->second.end(); ++LineIt)
        {
            FMissionDialogLine& Line = *LineIt;
            // the target. "Player" is the ship of the player whose ship has
            // the resolved id, else of the mission's player; any other name must be a ship or
            // solar of the system. A miss clears bResult and skips to the comm (the line keeps
            // its old target id and its first-line part is skipped).
            unsigned int LineTargetId = System->FindObjectIdByName(&Line.Target, ControllerId);
            bool bTargetFound = false;
            if (Line.Target == "Player")
            {
                FContentPlayerState* TargetPlayer = FindContentPlayerStateByShip(&LineTargetId);
                if (TargetPlayer == nullptr)
                    TargetPlayer = FindContentPlayerState(*PlayerId);
                if (TargetPlayer != nullptr && TargetPlayer->ShipObject != nullptr)
                {
                    LineTargetId = TargetPlayer->ShipObject->SpaceObjId;
                    bTargetFound = true;
                }
            }
            else
            {
                bTargetFound = System->FindShipOrSolar(LineTargetId) != nullptr;
            }

            if (!bTargetFound)
            {
                bResult = false;
            }
            else
            {
                Line.Comm.TargetId = LineTargetId;
                if (Line.bFirst != 0)
                {
                    // a TString copy of the speaker's chars is built and never
                    // read (FindObjectIdByName gets the line's own name) - not reproduced.
                    // the speaker. A missing player state keeps the previous
                    // Speaker and clears bResult; a player without a ship, or a missing object,
                    // sets Speaker null and clears bResult.
                    unsigned int SpeakerId = System->FindObjectIdByName(&Line.Speaker, ControllerId);
                    if (Line.Speaker == "Player")
                    {
                        FContentPlayerState* SpeakerPlayer = FindContentPlayerStateByShip(&SpeakerId);
                        if (SpeakerPlayer == nullptr)
                            SpeakerPlayer = FindContentPlayerState(*PlayerId);
                        if (SpeakerPlayer == nullptr)
                        {
                            // Speaker kept; bResult cleared (again at
                            // when Speaker is null).
                            bResult = false;
                        }
                        else
                        {
                            Speaker = SpeakerPlayer->ShipObject;
                            if (Speaker == nullptr)
                                bResult = false;
                        }
                    }
                    else
                    {
                        Speaker = System->FindShipOrSolar(SpeakerId);
                        if (Speaker == nullptr)
                            bResult = false;
                    }
                    TargetId = Line.Comm.TargetId;
                    TextId = Line.Comm.TextId;
                }
            }
            // the routine (insert 1 at the end) of the line's +0x6c.
            Comms.push_back(Line.Comm);
        }

        // the speaker object of this map entry (same resolution as the
        // target) gets the comms (the routine = vector assignment on +0x108); a miss clears
        // bResult.
        unsigned int OwnerId = System->FindObjectIdByName(&It->first, ControllerId);
        FContentSpaceObj* Owner = nullptr;
        if (It->first == "Player")
        {
            FContentPlayerState* OwnerPlayer = FindContentPlayerStateByShip(&OwnerId);
            if (OwnerPlayer == nullptr)
                OwnerPlayer = FindContentPlayerState(*PlayerId);
            if (OwnerPlayer != nullptr && OwnerPlayer->ShipObject != nullptr)
                Owner = OwnerPlayer->ShipObject;
        }
        else
        {
            Owner = System->FindShipOrSolar(OwnerId);
        }
        if (Owner != nullptr)
            Owner->PendingComms = Comms;
        else
            bResult = false;
    }

    // the first line from its speaker; 1.0f when the target is a player's
    // ship (the first match in the player map the field ends the walk), else 0.5f.
    if (Speaker == nullptr)
        return bResult;
    float Duration = 0.5f;
    std::map<unsigned int, std::unique_ptr<FContentPlayerState>>& Players = GetContentPlayerStates();
    std::map<unsigned int, std::unique_ptr<FContentPlayerState>>::iterator PlayerIt = Players.begin();
    for (; PlayerIt != Players.end(); ++PlayerIt)
    {
        if (TargetId == PlayerIt->second->GetShipId())
        {
            Duration = 1.0f;
            break;
        }
    }
    System->SendComm(Speaker->SpaceObjId, TargetId, TextId, 0, 0, nullptr, Duration);
    return bResult;
}

// (06ef2a10-06ef2a63, cdecl): find (the routine lower bound + key test).
bool __cdecl StartMissionDialog(unsigned int const* DialogId, unsigned int const* PlayerId)
{
    std::map<unsigned int, FMissionDialog>::iterator It = GMissionDialogs.find(*DialogId);
    if (It == GMissionDialogs.end())
        return false;
    return It->second.Start(PlayerId);
}
