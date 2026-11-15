// Author: Giperion.

#pragma once

// NN voice queue -- Client\nn\NN_Messaging.cpp (OutpostFrontier.exe
// ). The same baseline file's NeuroNetMissionMessage pop-up
// (0x00489fe0..0x0048b280) is nn_mission_message.h / .cpp.

// The NeuroNet voice lines ("insufficient credits", "incoming missile", "scanning cargo", ...)
// all go through one short FIFO: the VC6 std::list at the field (head the field, size
// ), capacity 4. Each entry is {message id, flag byte, playing ISoundSource*}. The
// head entry plays through the NNVoice profile (the field = CreateID("NNVoice"),
// = its FVoiceProfile); the per-frame tick drops the head when its source has
// finished and starts the next one.

// Callers of NnVoiceEnqueue (the routine, 21 sites) and NnVoiceEnqueueByName (the routine) are
// spread over the NeuroNet screens and the HUD. NnVoiceInit is the routine's tail
// (NnSystemInit, nn_modal.cpp) and NnVoiceTick is called from NN_MainButton's update
// (the branch, 0x0045c957; nn_main_button.cpp).

// the field = CreateID("NNVoice"), the field = the routine of it.
void NnVoiceInit();

// (cdecl; the float frame time it is passed is never read) -- drop the head once
// its source has finished, then start the next one.
void NnVoiceTick(float Unused);

// the routine on the field: queue MsgId (ignored when already queued, FDUMP
// "NN Message inbox overflow!" at 4); the first entry starts at once. bKeyBit2 lands in bit 2 of
// the play key's flag byte (the routine).
void NnVoiceEnqueue(unsigned MsgId, unsigned char bKeyBit2);

// NnVoiceEnqueue(CreateID(Name), 0).
void NnVoiceEnqueueByName(char const* Name);
