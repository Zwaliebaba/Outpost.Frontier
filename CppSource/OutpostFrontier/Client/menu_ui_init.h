// Author: Giperion.

#pragma once

// The UI sound table of the post-DataConfig bootstrap (the routine at 0x005b1d58). The flat
// intro-menu shell InitMenuUiSlice that used to be declared here was a port scaffold and is gone.

// / the routine / the routine / the routine / the routine — the field[0x2d].
void InitUiSoundTable();
int FindUiSoundSlot(const char* Name);
// the routine(Arch, 0, 0): start only if no live source plays this slot.
void PlayUiSoundSlot(unsigned int Slot);
// the same lazy fill, then the routine(Arch, &Key, 0) with a play key whose
// +4 flag byte has bit 0 set (bits 1..2 cleared) and every other field zero. Callers:
// NN_Preferences::SetSliderOption (0x004b1778 / 004b17af / 004b190e / 004b1949 / 004b1967),
// .
void PlayUiSoundSlotFlag1(unsigned int Slot);
// the routine(Arch, 0, 0): rewind a live source of this slot, else start one.
void PlayOrRewindUiSoundSlot(unsigned int Slot);
// the routine(the field[Slot]): stop every live source of the cached
// archetype (an empty slot is passed through as null, like baseline).
void StopUiSoundSlot(unsigned int Slot);
// "ui_rollover" (slot 0x1d) through the routine, only while the intro menu is
// the current state (the routine) or the local server tick is held (the routine).
void PlayUiRolloverSound();
// true when a live source plays any cached UI archetype (the field up to the
// end sentinel 0x0067a55c, the routine on each non-null slot). Caller: the MissionBriefing
// state's update the branch (0x0043d3c6).
bool IsAnyUiSoundPlaying();
const char* GetUiSoundSlotName(unsigned int Slot);
// — release every cached UI-sound archetype reference and null the table.
void ShutdownUiSoundTable();
