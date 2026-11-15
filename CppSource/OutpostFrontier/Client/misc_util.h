// Author: Giperion.

#pragma once

// MiscUtil -- Client\cmn\MiscUtil.cpp.

// The client-side FmtStr formatter ("FmtStr::sprintf()" in its own FDUMP text): expands the
// mad-lib template named by FmtStr::MadLibId against the FmtStr's Val list, resolving each
// value through Universe / GoodList / Reputation into display text.

#include "common_text.h"
#include "client_cliobj.h"

#include <vector>

struct FAnimScriptSet;

// __thiscall on the FmtStr: the first Val whose TypeTag equals Code and whose
// Flags (the per-kind running index) equals Index, or null.
FmtStr::Val const* FmtStrFindVal(FmtStr const* Fmt, unsigned char Index, unsigned short Code);

// __thiscall on the FmtStr, RET 8: writes at most DestChars wide characters
// (terminator included) into Dest and returns the number written, terminator excluded.
int FmtStrSprintf(FmtStr const* Fmt, wchar_t* Dest, int DestChars);

// the %R expansion: the reputation instance's display name, rebuilt from its
// second name FmtStr (and cached back through Reputation::Vibe::SetName) when it is empty.
// End is taken but never read, as in baseline.
void MiscUtilAppendRepInstanceName(int RepId, wchar_t** Cursor, wchar_t* End);

// append one ids string at *Cursor, FDUMP on a miss, and re-terminate the last
// character when the copy lands exactly on End.
void MiscUtilAppendIds(wchar_t** Cursor, wchar_t* End, unsigned Ids);

// a number with thousands separators ("1,234,567"), after an optional ids
// prefix: PrefixIds < 0 loads ids 0x2140, PrefixIds > 0 loads that ids, 0 starts empty.
// The prefix is loaded with a 0xff-character limit whatever Out's real size is.
void FormatGroupedNumber(wchar_t* Out, int Value, int PrefixIds);

// load ids Ids into a 0x400-character buffer (FDUMP an error on a miss) and
// heap-copy it into *Out with Common's malloc_wstrcpy; returns what malloc_wstrcpy returns.
wchar_t const* MiscUtilLoadIdsCopy(wchar_t** Out, unsigned Ids);

// (cdecl) -- open File as a "UTF" file system, load every texture below its
// "Texture library" node (NeuronRuntime::TextureLib slot 5) and return the texture group id. Callers:
// (fx\efx.txm), 0x0045f56a.
int MiscUtilLoadTextureLibrary(char const* File);

// (cdecl) -- NeuronRuntime::TextureLib slot 7 on Group: release a group
// MiscUtilLoadTextureLibrary loaded. Callers: 0x0045e426, 0x0046c7b6.
void MiscUtilReleaseTextureLibrary(int Group);

// (cdecl) -- open File as a file system (implementation 0), load its material
// library into a new group (NeuronRuntime::MaterialLib slot 8), apply-verify it at 1.0 (slot 11) and
// resolve its textures (slot 12); returns the group id (-1 when the load wrote none). Released
// with MaterialLib slot 10 (DeleteGroup). Callers: NN_MissionVendor::Create 0x0048bb7f,
// .
int MiscUtilLoadMaterialLibrary(char const* File);

// (cdecl, 7 arguments) -- create an engine instance of the .cmp/.3db File.
// When the engine does not know the archetype yet: open File's own file system (FDUMP a
// warning and return -1 when it does not exist), take its material library entry
// (the routine) and the at most 5 extra material files in MaterialPaths (the routine), load
// the archetype (FDUMP and return -1 on failure), instance it, read the animation scripts
// into Scripts, and hand the material entries to Entries -- or release them when Entries is
// null. When it does: instance it, and read Scripts from a fresh file system. *OutHandle
// receives the archetype handle when OutHandle is not null. Data is never read.
void* MiscUtilLoadEngineInstance(char const* File, IFileSystem* Data, FAnimScriptSet* Scripts,
                                 void* Unused,
                                 Archetype::FCliVc6Vector<Archetype::FCliMaterialPathPair> const*
                                     MaterialPaths,
                                 std::vector<Archetype::FCliMaterialEntry*>* Entries,
                                 long* OutHandle);

struct ICamera;

// (cdecl) -- the sphere (Center, Radius) through the camera's view transform
// (ICamera slot 16) with 3DMathEngine TransformXform (the file's the field, acquired on
// first use), then ICamera slot 15 ClassifySphere; true when it answers 3 or more (straddle or
// inside). Callers: CharacterObject::DrawOpaque (0x004451d6) and the LightsCamera particle draw
// (0x00557527).
bool MiscUtilSphereInView(ICamera* Camera, float const* Center, float Radius);
