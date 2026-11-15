// Author: Giperion.

#include "vis_effect_arch.h"

#include "particlefx.h"
#include "client_cliobj.h" // Archetype::ResolveMaterialEntry (the routine) / ReleaseMaterialEntrySlot (the routine)
#include "common_id.h"
#include "common_ini.h"
#include "neuroncomponents.h"
#include "NeuronRuntime.h"
#include "effect_type_info.h"
#include "menu_sound_arch.h"
#include "readfile.h"

#include <map>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

// Defined below; used by BeamArchFindOrInsert / LoadEffectSection.
FBeamEffectArch* FindBeamEffectArchByNickname(char const* Nickname);
FVisEffectArch* FindVisEffectByNickname(char const* Nickname);
FEffectArch* FindEffectArch(unsigned const* NicknameId);

namespace
{
    static const float kColorScale = 0.003921569f; // 1/255
    static const float kBrightMin = 0.01f;
    static const unsigned kBrightMinBits = 0x3c23d70a; // 0.01f
    static const unsigned kOneBits = 0x3f800000;       // 1.0f

    // the EffectArch registry, a std::map keyed by the nickname's CreateID
    // (unsigned compare, the routine / the routine) holding operator-new'd EffectArchs.
    // Built by the static initializer the branch, destroyed through atexit the branch.
    std::map<unsigned, FEffectArch*> G_EffectArchs;

    // set by the routine once the FxRuntime is bound, cleared by the routine.
    bool G_bFxRuntimeBound = false;

    // s_Client\cmn\EffectArch.cpp_005c84cc.
    char const kEffectArchCpp[] = "Client\\cmn\\EffectArch.cpp";

    // The nickname id as baseline's ID_String::get_string formats it.
    char const* EffectArchNickname(FEffectArch const* Arch)
    {
        ID_String Name;
        Name.Id = Arch->NicknameId;
        return Name.get_string();
    }

    // Client\cmn\ALELib.cpp. The map the field is ordered by stricmp (the routine /
    // ); its value is { use count (node + 0x10), handle (node + 0x14) }.
    struct FAleNameLess
    {
        bool operator()(std::string const& A, std::string const& B) const
        {
            return _stricmp(A.c_str(), B.c_str()) < 0;
        }
    };

    struct FAleLibEntry
    {
        int RefCount;
        int Handle;
    };

    std::map<std::string, FAleLibEntry, FAleNameLess> G_AleLib;
    int G_AleLibNextHandle = 1;                                 // (.data init 1)

    // Client\cmn\VisEffectArch.cpp's VisEffect map, built by the static
    // initializer the branch and torn down through atexit the branch. Node: key at +0x0c,
    // the VisEffect by value at +0x10. baseline keys it by the StringAlloc'd nickname POINTER
    // (unsigned compare: the routine 0x0042f110..0x0042f117 CMP / SETC, lower bound
    // , find the routine). StringAlloc(Text, false) interns through Common's
    // StringCache, a map ordered by stricmp (Common the routine), so two keys are one entry
    // exactly when their texts are stricmp-equal, and every empty text is the one shared empty
    // string. The port's StringAlloc does not intern (ledger "INI_Reader", StringAlloc line), so
    // the key here is the text itself under the same stricmp: the same entries and the same
    // lookups. Only the iteration order differs, and nothing but the exit teardown walks it.
    // Teardown order as in baseline's XC table: G_AleLib (slot 0x0060f010, above in this file)
    // and the material cache (client_cliobj.cpp, slot 0x0060f0bc, linked before this file --
    // see the note in OutpostFrontier.vcxproj) are built first, so both outlive this map, whose
    // ~FVisEffectArch (the routine) releases through them.
    std::map<std::string, FVisEffectArch, FAleNameLess> G_VisEffects;

    // the BeamSpear / BeamBolt map, the same shape: built by the static
    // initializer at 0x0042e320, torn down through atexit 0x0042e350, keyed by the StringAlloc'd
    // nickname pointer (operator[] the routine, find the routine through the routine's
    // lower bound), the value (the routine defaults) at node + 0x10. Keyed here by the text
    // under stricmp for the reason given on G_VisEffects.
    std::map<std::string, FBeamEffectArch, FAleNameLess> G_BeamArches;

    void CopyZ(char* Dest, int DestChars, char const* Src)
    {
        if (Dest == nullptr || DestChars <= 0)
            return;
        if (Src == nullptr)
        {
            Dest[0] = '\0';
            return;
        }
        strncpy(Dest, Src, static_cast<size_t>(DestChars - 1));
        Dest[DestChars - 1] = '\0';
    }

    // 5-char baseline strncmp style (counts down including NUL for short words).
    bool Match5(char const* Value, char const* Literal)
    {
        if (Value == nullptr || Literal == nullptr)
            return false;
        int Remain = 5;
        char const* A = Value;
        char const* B = Literal;
        bool bEqual = true;
        while (Remain != 0)
        {
            Remain -= 1;
            bEqual = (*A == *B);
            if (!bEqual)
                break;
            A += 1;
            B += 1;
        }
        return bEqual;
    }

    // (thiscall) -- resolve one material-cache entry per texture name, in order.
    // A name that does not resolve is reported and erased from the name list (the iterator
    // stays put); a resolved one is appended to TextureSlots. Returns the last iteration's
    // outcome, false for an empty list.
    bool VisEffectArchLoadTextures(FVisEffectArch* Vis)
    {
        bool bOk = false;
        size_t Index = 0;
        while (Index != Vis->Textures.size())
        {
            // the routine(name, 1, -1).
            Archetype::FCliMaterialEntry* const Slot =
                Archetype::ResolveMaterialEntry(Vis->Textures[Index].c_str(), true, 0xffffffffu);
            bOk = Slot != nullptr;
            if (bOk)
            {
                // the routine -- push_back.
                Vis->TextureSlots.push_back(Slot);
                ++Index;
            }
            else
            {
                // the "%s" before the name is the .rdata literal "temp".
                FDUMP(0x100001,
                      "%s(%d) : *** ERROR: [VisEffect] %s: Failed to load texture library: %s.  "
                      "Removing from txm list.",
                      "Client\\cmn\\VisEffectArch.cpp", 0x166, "temp",
                      Vis->Textures[Index].c_str());
                // erase in place.
                Vis->Textures.erase(Vis->Textures.begin() + static_cast<ptrdiff_t>(Index));
            }
        }
        return bOk;
    }

    // and 0x0042e16f..0x0042e1af: the routine on every slot, then
    // end = begin.
    void VisEffectArchReleaseTextures(FVisEffectArch* Vis)
    {
        for (size_t Index = 0; Index < Vis->TextureSlots.size(); ++Index)
            Archetype::ReleaseMaterialEntrySlot(&Vis->TextureSlots[Index]);
        Vis->TextureSlots.clear();
    }

    // — default TrailAppearance / BeamEffectArch (all 1.0f).
    void BeamArchInitDefaults(FBeamEffectArch* Arch)
    {
        memset(Arch, 0, sizeof(*Arch));
        Arch->Nickname = nullptr;
        Arch->SegmentMode = 0;
        Arch->UvModeV = 0;
        Arch->UvModeU = 0;
        float* F = &Arch->ColorCoreR;
        int Index = 0;
        for (Index = 0; Index < 28; ++Index)
            F[Index] = 1.0f;
    }

    // — clamp lengths vs core_width and brightness to [0.01, 1].
    void BeamArchClamp(FBeamEffectArch* Arch)
    {
        if (Arch->Radius0 < Arch->RadiusNear)
            Arch->Radius0 = Arch->RadiusNear;
        if (Arch->SegmentMode == 0)
        {
            if (Arch->Radius1 < Arch->RadiusNear)
                Arch->Radius1 = Arch->RadiusNear;
        }
        else if (Arch->SegmentMode == 1)
        {
            if (Arch->Radius1 < Arch->SecCoreWidth)
                Arch->Radius1 = Arch->SecCoreWidth;
        }
        if (Arch->RadiusTip < Arch->RadiusNear)
            Arch->RadiusTip = Arch->RadiusNear;

        if (Arch->IntensityScale0 < kBrightMin)
        {
            unsigned Bits = kBrightMinBits;
            Arch->IntensityScale0 = *reinterpret_cast<float*>(&Bits);
        }
        else if (Arch->IntensityScale0 > 1.0f)
        {
            unsigned Bits = kOneBits;
            Arch->IntensityScale0 = *reinterpret_cast<float*>(&Bits);
        }

        if (Arch->IntensityScale1 < kBrightMin)
        {
            unsigned Bits = kBrightMinBits;
            Arch->IntensityScale1 = *reinterpret_cast<float*>(&Bits);
            return;
        }
        if (Arch->IntensityScale1 > 1.0f)
        {
            unsigned Bits = kOneBits;
            Arch->IntensityScale1 = *reinterpret_cast<float*>(&Bits);
        }
    }

    void WriteRgb255(float* Dest, float R, float G, float B)
    {
        Dest[0] = R * kColorScale;
        Dest[1] = G * kColorScale;
        Dest[2] = B * kColorScale;
    }

    // — one INI value into an open BeamSpear/Bolt arch.
    bool BeamArchParseValue(FBeamEffectArch* Arch, INI_Reader* Reader)
    {
        if (Reader->is_value("tip_color"))
        {
            WriteRgb255(&Arch->ColorCoreR, Reader->get_value_float(0), Reader->get_value_float(1),
                        Reader->get_value_float(2));
            return true;
        }
        if (Reader->is_value("tail_color"))
        {
            WriteRgb255(&Arch->ColorMidR, Reader->get_value_float(0), Reader->get_value_float(1),
                        Reader->get_value_float(2));
            return true;
        }
        if (Reader->is_value("core_color"))
        {
            WriteRgb255(&Arch->ColorOuterR, Reader->get_value_float(0), Reader->get_value_float(1),
                        Reader->get_value_float(2));
            return true;
        }
        if (Reader->is_value("sec_core_color"))
        {
            WriteRgb255(&Arch->ColorBand1R, Reader->get_value_float(0), Reader->get_value_float(1),
                        Reader->get_value_float(2));
            return true;
        }
        if (Reader->is_value("outter_color"))
        {
            WriteRgb255(&Arch->ColorTipR, Reader->get_value_float(0), Reader->get_value_float(1),
                        Reader->get_value_float(2));
            return true;
        }
        if (Reader->is_value("sec_outter_color"))
        {
            WriteRgb255(&Arch->ColorBand2R, Reader->get_value_float(0), Reader->get_value_float(1),
                        Reader->get_value_float(2));
            return true;
        }
        if (Reader->is_value("tip_length"))
        {
            Arch->Radius0 = Reader->get_value_float(0);
            return true;
        }
        if (Reader->is_value("tail_length"))
        {
            Arch->Radius1 = Reader->get_value_float(0);
            return true;
        }
        if (Reader->is_value("core_length"))
        {
            Arch->RadiusMid = Reader->get_value_float(0);
            return true;
        }
        if (Reader->is_value("core_width"))
        {
            Arch->RadiusNear = Reader->get_value_float(0);
            return true;
        }
        if (Reader->is_value("sec_core_width"))
        {
            Arch->SecCoreWidth = Reader->get_value_float(0);
            return true;
        }
        if (Reader->is_value("head_brightness"))
        {
            Arch->IntensityScale0 = Reader->get_value_float(0);
            return true;
        }
        if (Reader->is_value("trail_brightness"))
        {
            Arch->IntensityScale1 = Reader->get_value_float(0);
            return true;
        }
        if (Reader->is_value("flash_size"))
        {
            Arch->RadiusNearAlt = Reader->get_value_float(0);
            return true;
        }
        if (Reader->is_value("head_width"))
        {
            Arch->RadiusTip = Reader->get_value_float(0);
            return true;
        }
        if (Reader->is_value("head_texture"))
        {
            // Listing: ball → 1, star/other → 0 (5-char compare).
            char const* Value = Reader->get_value_string();
            if (!Match5(Value, "star") && Match5(Value, "ball"))
                Arch->UvModeV = 1;
            else
                Arch->UvModeV = 0;
            return true;
        }
        if (Reader->is_value("trail_texture"))
        {
            // Listing: thin → 1, wide/other → 0.
            char const* Value = Reader->get_value_string();
            if (!Match5(Value, "wide") && Match5(Value, "thin"))
                Arch->UvModeU = 1;
            else
                Arch->UvModeU = 0;
            return true;
        }
        return false;
    }

    FBeamEffectArch* BeamArchFindOrInsert(char const* Nickname)
    {
        // (thiscall on the field, RET 4): the map's operator[] -- a default
        // value from the routine (0x0042ed55), the pair built (the routine) and inserted
        // unless the key is present (the routine); the node's value (+0x10) either way. An
        // existing entry is returned untouched.
        std::map<std::string, FBeamEffectArch, FAleNameLess>::iterator Found =
            G_BeamArches.find(std::string(Nickname));
        if (Found != G_BeamArches.end())
            return &Found->second;
        FBeamEffectArch Defaults;
        BeamArchInitDefaults(&Defaults);
        std::pair<std::map<std::string, FBeamEffectArch, FAleNameLess>::iterator, bool> Inserted =
            G_BeamArches.insert(
                std::pair<std::string const, FBeamEffectArch>(std::string(Nickname), Defaults));
        return &Inserted.first->second;
    }

    // (cdecl) -- one [BeamSpear] / [BeamBolt] section; both headers reach it from
    // the one call at 0x0040ed09. Returns whether an arch was selected (0x0042e90e..0x0042e913).
    bool LoadBeamSection(INI_Reader* Reader)
    {
        FBeamEffectArch* Current = nullptr;
        bool bHaveValue = Reader->read_value();
        while (bHaveValue)
        {
            if (Reader->is_value("nickname"))
            {
                // StringAlloc(value, false), the routine on it, and the
                // arch's first field takes the StringAlloc'd pointer.
                char* const Nick = StringAlloc(Reader->get_value_string(), false);
                Current = BeamArchFindOrInsert(Nick);
                Current->Nickname = Nick;
                // the kind from the reader's current header --
                // "BeamSpear" 0, "BeamBolt" 1, anything else leaves +0x04 as it was.
                if (Reader->is_header("BeamSpear"))
                    Current->SegmentMode = 0;
                else if (Reader->is_header("BeamBolt"))
                    Current->SegmentMode = 1;
            }
            else if (Current != nullptr)
            {
                BeamArchParseValue(Current, Reader);
            }
            bHaveValue = Reader->read_value();
        }
        if (Current != nullptr)
            BeamArchClamp(Current);
        return Current != nullptr;
    }

    // _vsnprintf into a 0x2000-byte buffer; the routine hands the text to
    // INI_Reader::log_link.
    void EffectArchLogLink(INI_Reader* Reader, char const* Format, char const* A, char const* B)
    {
        char Buffer[0x2000];
        _snprintf(Buffer, sizeof(Buffer), Format, A, B);
        Reader->log_link(Buffer);
    }

    // (thiscall on the arch, RET 4) -- one INI value into an open [Effect] arch;
    // true for every key it knows (a failed lookup included), false otherwise.
    bool EffectArchParseValue(FEffectArch* Effect, INI_Reader* Reader)
    {
        // CreateID into +0x04.
        if (Reader->is_value("nickname"))
        {
            Effect->NicknameId = CreateID(Reader->get_value_string());
            return true;
        }
        // .
        if (Reader->is_value("effect_type"))
        {
            // into +0x00; 0x2f (unknown): log_link("") and an error.
            int const TypeIndex = EffectTypeIndexFromName(Reader->get_value_string());
            Effect->EffectType = TypeIndex;
            if (TypeIndex == kEffectTypeCount)
            {
                Reader->log_link("");
                FDUMP(0x100001, "%s(%d) : ERROR:General:%s", kEffectArchCpp, 0x3a,
                      "unknown effect type");
                return true;
            }
            // ; no info -> an error naming the index.
            FEffectTypeInfo const* const Info = EffectTypeInfoByIndex(TypeIndex);
            if (Info == nullptr)
            {
                FDUMP(0x100001,
                      "%s(%d) : *** ERROR: EffectTypeInfo missing for EffectType (%d)",
                      kEffectArchCpp, 0x47, Effect->EffectType);
                return true;
            }
            // FCOMP run_time (+0x18) vs 0.0, TEST AH,5 / JP -- no jump
            // (C0 only: ordered less) raises bit 0 of +0x50, the jump (not less, or
            // unordered) clears it.
            if (Info->RunTime < 0.0f)
                Effect->Flags50 = static_cast<unsigned char>(Effect->Flags50 | 1u);
            else
                Effect->Flags50 = static_cast<unsigned char>(Effect->Flags50 & ~1u);
            return true;
        }
        // StringAlloc'd name looked up (the routine) into +0x08; a hit
        // also copies the VisEffect's first field (its nickname) into +0x0c.
        if (Reader->is_value("vis_effect"))
        {
            char const* const Name = StringAlloc(Reader->get_value_string(), false);
            Effect->VisEffect = FindVisEffectByNickname(Name);
            if (Effect->VisEffect != nullptr)
                Effect->VisEffectName = Effect->VisEffect->Nickname;
            if (Effect->VisEffect == nullptr)
            {
                EffectArchLogLink(Reader, "[Effect] %s couldn't find [VisEffect] %s.",
                                  EffectArchNickname(Effect), Name);
            }
            return true;
        }
        // the same lookup into +0x10; a miss reports the same
        // [VisEffect] text (it jumps back into vis_effect's test).
        if (Reader->is_value("vis_generic"))
        {
            char const* const Name = StringAlloc(Reader->get_value_string(), false);
            Effect->VisGeneric = FindVisEffectByNickname(Name);
            if (Effect->VisGeneric == nullptr)
            {
                EffectArchLogLink(Reader, "[Effect] %s couldn't find [VisEffect] %s.",
                                  EffectArchNickname(Effect), Name);
            }
            return true;
        }
        // the routine into +0x14.
        if (Reader->is_value("vis_beam"))
        {
            char const* const Name = StringAlloc(Reader->get_value_string(), false);
            Effect->VisBeam = FindBeamEffectArchByNickname(Name);
            if (Effect->VisBeam == nullptr)
            {
                EffectArchLogLink(Reader, "[Effect] %s couldn't find [VisBeam] %s.",
                                  EffectArchNickname(Effect), Name);
            }
            return true;
        }
        // CreateID, the routine into +0x18; a miss names both ids.
        if (Reader->is_value("snd_effect"))
        {
            char const* const Name = Reader->get_value_string();
            ID_String SoundId;
            SoundId.Id = CreateID(Name);
            Effect->SndEffect = LookupClientSoundArch(&SoundId);
            if (Effect->SndEffect == nullptr)
            {
                char const* const SoundText = SoundId.get_string();
                EffectArchLogLink(Reader, "[Effect] %s couldn't find [Sound] %s.",
                                  EffectArchNickname(Effect), SoundText);
            }
            return true;
        }
        // a null string empties the name; otherwise at most 0x1f
        // characters are copied, terminated, and the length stored at +0x1c.
        if (Reader->is_value("lgt_effect"))
        {
            char const* const Name = Reader->get_value_string();
            if (Name == nullptr)
            {
                Effect->LgtEffectLen = 0;
                Effect->LgtEffect[0] = '\0';
                return true;
            }
            size_t Len = strlen(Name);
            if (Len >= 0x20)
                Len = 0x1f;
            memcpy(Effect->LgtEffect, Name, Len);
            Effect->LgtEffect[Len] = '\0';
            Effect->LgtEffectLen = static_cast<unsigned>(Len);
            return true;
        }
        // get_value_float(0) stored as it is (FSTP float).
        if (Reader->is_value("lgt_range_scale"))
        {
            Effect->LgtRangeScale = Reader->get_value_float(0);
            return true;
        }
        if (Reader->is_value("lgt_time_scale"))
        {
            Effect->LgtTimeScale = Reader->get_value_float(0);
            return true;
        }
        if (Reader->is_value("lgt_radius"))
        {
            Effect->LgtRadius = Reader->get_value_float(0);
            return true;
        }
        // three ftol'd floats into the bytes +0x4c..+0x4e.
        if (Reader->is_value("lgt_color"))
        {
            Effect->LgtColorR = static_cast<unsigned char>(static_cast<long>(Reader->get_value_float(0)));
            Effect->LgtColorG = static_cast<unsigned char>(static_cast<long>(Reader->get_value_float(1)));
            Effect->LgtColorB = static_cast<unsigned char>(static_cast<long>(Reader->get_value_float(2)));
            return true;
        }
        return false;
    }

    // The four bytes +0x4c..+0x4f as the one dword the routine compares and prints.
    unsigned EffectArchColorDword(FEffectArch const* Arch)
    {
        return (static_cast<unsigned>(Arch->LgtColorA) << 24) |
               (static_cast<unsigned>(Arch->LgtColorB) << 16) |
               (static_cast<unsigned>(Arch->LgtColorG) << 8) |
               static_cast<unsigned>(Arch->LgtColorR);
    }

    // The dword +0x50..+0x53 the routine compares and copies.
    unsigned EffectArchFlagsDword(FEffectArch const* Arch)
    {
        return static_cast<unsigned>(Arch->Flags50) |
               (static_cast<unsigned>(Arch->Pad51[0]) << 8) |
               (static_cast<unsigned>(Arch->Pad51[1]) << 16) |
               (static_cast<unsigned>(Arch->Pad51[2]) << 24);
    }

    // (thiscall on the registered arch, RET 4) -- a reload's copy of an [Effect]
    // merged into the registered one: every field that differs is warned about, then copied.
    void EffectArchMerge(FEffectArch* Existing, FEffectArch const* Incoming)
    {
        // VisEffect (+0x08); the new one is named by its +0x08 (the
        // StringAlloc'd ale name), "NULL" without one. +0x08 and +0x0c copied.
        if (Incoming->VisEffect != Existing->VisEffect)
        {
            char const* const NewName =
                Incoming->VisEffect != nullptr ? Incoming->VisEffect->ParticleFxPath : "NULL";
            FDUMP(0x100002, "%s(%d) : *** WARNING: %s: 'vis_effect' changed to %s",
                  kEffectArchCpp, 0x89, EffectArchNickname(Existing), NewName);
            Existing->VisEffect = Incoming->VisEffect;
            Existing->VisEffectName = Incoming->VisEffectName;
        }

        // SoundArch (+0x18), the new one named by its slot 1 (+0x04,
        // the file name), "NULL" without one. The pointer is copied; nothing is acquired.
        if (Incoming->SndEffect != Existing->SndEffect)
        {
            char const* const NewName =
                Incoming->SndEffect != nullptr
                    ? static_cast<Archetype::ISound*>(Incoming->SndEffect)->GetFileName()
                    : "NULL";
            FDUMP(0x100002, "%s(%d) : *** WARNING: %s: 'snd_effect' changed to %s",
                  kEffectArchCpp, 0x93, EffectArchNickname(Existing), NewName);
            Existing->SndEffect = Incoming->SndEffect;
        }

        // the light names compared byte by byte (strcmp); a change
        // prints (old, new) and copies at most 0x1f characters with the length at +0x1c.
        if (strcmp(Existing->LgtEffect, Incoming->LgtEffect) != 0)
        {
            FDUMP(0x100002, "%s(%d) : *** WARNING: %s: 'lgt_effect.name' changed to %s.",
                  kEffectArchCpp, 0x97, Existing->LgtEffect, Incoming->LgtEffect);
            size_t Len = strlen(Incoming->LgtEffect);
            if (Len >= 0x20)
                Len = 0x1f;
            memmove(Existing->LgtEffect, Incoming->LgtEffect, Len);
            Existing->LgtEffect[Len] = '\0';
            Existing->LgtEffectLen = static_cast<unsigned>(Len);
        }

        // FCOMP / TEST AH,0x44 / JNP skips only on "equal" (C3 alone);
        // a difference or an unordered pair prints (old, new) as doubles and copies.
        if (Incoming->LgtRangeScale != Existing->LgtRangeScale)
        {
            FDUMP(0x100002,
                  "%s(%d) : *** WARNING: %.2f: 'lgt_effect.range_scale' changed to %.2f.",
                  kEffectArchCpp, 0x98, static_cast<double>(Existing->LgtRangeScale),
                  static_cast<double>(Incoming->LgtRangeScale));
            Existing->LgtRangeScale = Incoming->LgtRangeScale;
        }
        if (Incoming->LgtTimeScale != Existing->LgtTimeScale)
        {
            FDUMP(0x100002,
                  "%s(%d) : *** WARNING: %.2f: 'lgt_effect.time_scale' changed to %.2f.",
                  kEffectArchCpp, 0x99, static_cast<double>(Existing->LgtTimeScale),
                  static_cast<double>(Incoming->LgtTimeScale));
            Existing->LgtTimeScale = Incoming->LgtTimeScale;
        }

        // the colour as one dword.
        unsigned const OldColor = EffectArchColorDword(Existing);
        unsigned const NewColor = EffectArchColorDword(Incoming);
        if (NewColor != OldColor)
        {
            FDUMP(0x100002, "%s(%d) : *** WARNING: %x: 'lgt_color_value' changed to %x.",
                  kEffectArchCpp, 0x9b, OldColor, NewColor);
            Existing->LgtColorR = Incoming->LgtColorR;
            Existing->LgtColorG = Incoming->LgtColorG;
            Existing->LgtColorB = Incoming->LgtColorB;
            Existing->LgtColorA = Incoming->LgtColorA;
        }

        // .
        if (Incoming->LgtRadius != Existing->LgtRadius)
        {
            FDUMP(0x100002, "%s(%d) : *** WARNING: %2.f: 'lgt_radius' changed to %2.f.",
                  kEffectArchCpp, 0x9c, static_cast<double>(Existing->LgtRadius),
                  static_cast<double>(Incoming->LgtRadius));
            Existing->LgtRadius = Incoming->LgtRadius;
        }

        // the flags dword.
        unsigned const OldFlags = EffectArchFlagsDword(Existing);
        unsigned const NewFlags = EffectArchFlagsDword(Incoming);
        if (NewFlags != OldFlags)
        {
            FDUMP(0x100002, "%s(%d) : *** WARNING: %x: 'flags' changed to %x.", kEffectArchCpp,
                  0x9e, OldFlags, NewFlags);
            Existing->Flags50 = Incoming->Flags50;
            Existing->Pad51[0] = Incoming->Pad51[0];
            Existing->Pad51[1] = Incoming->Pad51[1];
            Existing->Pad51[2] = Incoming->Pad51[2];
        }
    }

    // 's [Effect] branch (0x0040eb4a..0x0040ec93).
    void LoadEffectSection(INI_Reader* Reader, bool bReload)
    {
        // operator new(0x58) and the inlined the routine (a failed
        // new leaves a null arch, which the value loop then writes through, as baseline).
        FEffectArch* const Arch = static_cast<FEffectArch*>(operator new(sizeof(FEffectArch)));
        if (Arch != nullptr)
            EffectArchConstruct(Arch);

        // every value through the routine.
        while (Reader->read_value())
            EffectArchParseValue(Arch, Reader);

        // the routine -- insert {+0x04, arch}; a new key is done.
        std::pair<std::map<unsigned, FEffectArch*>::iterator, bool> const Inserted =
            G_EffectArchs.insert(std::make_pair(Arch->NicknameId, Arch));
        if (Inserted.second)
            return;

        // the key exists -- a reload merges into the registered arch
        // (the routine), otherwise the second version is refused with a warning.
        if (bReload)
        {
            EffectArchMerge(Inserted.first->second, Arch);
        }
        else
        {
            FDUMP(0x100002,
                  "%s(%d) : *** WARNING: [Effect] nickname = %s value is not unique. Aborted "
                  "second version.",
                  kEffectArchCpp, 0xfc, EffectArchNickname(Arch));
        }

        // the unused copy destroyed (inlined the routine) and freed.
        if (Arch != nullptr)
        {
            EffectArchDestruct(Arch);
            operator delete(Arch);
        }
    }

    // (cdecl) -- once (the field), with an FxRuntime: IFxRuntime +0x10 and
    // +0x0c get NeuronRuntime::ParticleFx, +0x14 gets NeuronRuntime::FxEffectLibrary.
    void VisEffectBindFxRuntime()
    {
        if (G_bFxRuntimeBound)
            return;
        G_bFxRuntimeBound = true;
        if (NeuronRuntime::FxRuntime == nullptr)
            return;
        NeuronRuntime::FxRuntime->BindUnknown(NeuronRuntime::ParticleFx);
        NeuronRuntime::FxRuntime->BindParticleFx(NeuronRuntime::ParticleFx);
        NeuronRuntime::FxRuntime->BindEffectLibrary(NeuronRuntime::FxEffectLibrary);
    }

    // (cdecl) -- NeuronRuntime::ParticleFx vt+0x24 (ClearNodeLibrary) and
    // NeuronRuntime::FxEffectLibrary vt+0x24 (ClearEffects), each when present; the bind latch drops.
    void VisEffectUnbindFxRuntime()
    {
        if (NeuronRuntime::ParticleFx != nullptr)
            NeuronRuntime::ParticleFx->ClearNodeLibrary();
        if (NeuronRuntime::FxEffectLibrary != nullptr)
            NeuronRuntime::FxEffectLibrary->ClearEffects();
        G_bFxRuntimeBound = false;
    }

    // one [VisEffect] section: "nickname" selects the entry (find-or-insert,
    // on the field); every other key goes to the routine on the selected
    // entry, and keys before the first nickname are dropped. Returns whether an entry was
    // selected (0x0042dcc1..0x0042dcc6 SETNZ); the one caller (0x0040ecdf) drops it.
    bool VisEffectLoadSection(INI_Reader* Reader)
    {
        FVisEffectArch* Entry = nullptr;

        bool bHaveValue = Reader->read_value();
        while (bHaveValue)
        {
            if (Reader->is_value("nickname"))
            {
                // StringAlloc(value, false); the routine on
                // finds the entry or inserts a default one (the node build
                // ), and its first field takes the StringAlloc'd pointer
                // either way.
                char const* const Nick = StringAlloc(Reader->get_value_string(), false);
                Entry = &G_VisEffects[std::string(Nick)];
                Entry->Nickname = Nick;
            }
            else if (Entry != nullptr)
            {
                // (thiscall, RET 4): the first matching key wins.
                if (Reader->is_value("effect_crc"))
                {
                    // get_value_int(0) into +0x04.
                    Entry->EffectCrc = static_cast<unsigned long>(Reader->get_value_int(0));
                }
                else if (Reader->is_value("has_alpha"))
                {
                    // bit 0 of +0x1c = get_value_bool(0).
                    Entry->bHasAlpha = Reader->get_value_bool(0);
                }
                else if (Reader->is_value("particlefx"))
                {
                    // StringAlloc'd name into +0x08.
                    CopyZ(Entry->ParticleFxPath, MAX_PATH, Reader->get_value_string());
                }
                else if (Reader->is_value("textures"))
                {
                    // StringAlloc'd name appended to +0x0c.
                    char const* const Texture = Reader->get_value_string();
                    Entry->Textures.push_back(std::string(Texture != nullptr ? Texture : ""));
                }
            }
            bHaveValue = Reader->read_value();
        }
        return Entry != nullptr;
    }
}

int EffectArchLoadIni(char const* Path, bool bReload)
{
    // . 0x0040eae1: the routine binds the FxRuntime (once).
    VisEffectBindFxRuntime();

    // the registry size now (the field); the answer is the growth.
    size_t const OldCount = G_EffectArchs.size();

    // an INI_Reader on the stack; a file that does not open, or one
    // without a first header, reads nothing.
    INI_Reader Reader;
    if (Reader.open(Path, false) && Reader.read_header())
    {
        do
        {
            // the section headers in baseline's test order.
            if (Reader.is_header("Effect"))
            {
                LoadEffectSection(&Reader, bReload);
            }
            else if (Reader.is_header("EffectLOD"))
            {
                LoadEffectLodSection(&Reader);
            }
            else if (Reader.is_header("EffectType"))
            {
                LoadEffectTypeSection(&Reader);
            }
            else if (Reader.is_header("VisEffect"))
            {
                VisEffectLoadSection(&Reader);
            }
            else if (Reader.is_header("BeamSpear") || Reader.is_header("BeamBolt"))
            {
                // either beam header reaches the one the routine call,
                // which reads the kind from the header itself.
                LoadBeamSection(&Reader);
            }
        } while (Reader.read_header());
    }

    // the INI_Reader goes out of scope; the growth is returned.
    return static_cast<int>(G_EffectArchs.size() - OldCount);
}

void EffectArchConstruct(FEffectArch* Arch)
{
    // (and 0x0040eb66..0x0040eba0 inline).
    Arch->EffectType = 0;          // +0x00
    Arch->NicknameId = 0;          // +0x04  ID_String()
    Arch->VisEffect = nullptr;     // +0x08
    Arch->VisEffectName = nullptr; // +0x0c  CacheString()
    Arch->VisGeneric = nullptr;    // +0x10
    Arch->VisBeam = nullptr;       // +0x14
    Arch->SndEffect = nullptr;     // +0x18
    Arch->LgtEffectLen = 0;        // +0x1c
    Arch->LgtEffect[0] = '\0';     // +0x20  (a byte store)
    Arch->LgtRangeScale = 1.0f;    // +0x40
    Arch->LgtTimeScale = 1.0f;     // +0x44
    Arch->LgtRadius = 0.0f;        // +0x48
    Arch->LgtColorR = 0xff;        // +0x4c  (one dword store of -1)
    Arch->LgtColorG = 0xff;
    Arch->LgtColorB = 0xff;
    Arch->LgtColorA = 0xff;
    Arch->Flags50 = 0;             // +0x50  (a dword store)
    Arch->Pad51[0] = 0;
    Arch->Pad51[1] = 0;
    Arch->Pad51[2] = 0;
    Arch->RefCount = 0;            // +0x54
}

void EffectArchDestruct(FEffectArch* Arch)
{
    // (and 0x0040ec51..0x0040ec8b / 0x0040ef4e..0x0040efa0 inline): with
    // references still out, warn and drop every one of them (the routine's body).
    int Dangling = Arch->RefCount;
    if (Dangling <= 0)
        return;
    FDUMP(0x100002,
          "%s(%d) : *** WARNING: Destroying effect archetype (%s) with %d dangling references. "
          "See SSTEVENS",
          kEffectArchCpp, 0x25, EffectArchNickname(Arch), Dangling);
    while (Dangling > 0)
    {
        EffectArchRelease(Arch);
        --Dangling;
    }
}

void EffectArchShutdown()
{
    // . 0x0040ef36..0x0040efe7: in key order, every non-null arch destroyed
    // (inlined the routine) and freed; the nodes stay until the clear below.
    for (std::map<unsigned, FEffectArch*>::iterator It = G_EffectArchs.begin();
         It != G_EffectArchs.end(); ++It)
    {
        FEffectArch* const Arch = It->second;
        if (Arch == nullptr)
            continue;
        EffectArchDestruct(Arch);
        operator delete(Arch);
    }

    // every node erased (the routine for the whole tree, else
    // one by one), the size back to 0.
    G_EffectArchs.clear();

    // / 0x0040f079: JMP the routine.
    VisEffectUnbindFxRuntime();
}

FVisEffectArch* FindVisEffectByNickname(char const* Nickname)
{
    // (cdecl; baseline takes the StringAlloc'd CacheString by address): the lower
    // bound of the field (the routine), a hit when it is not the head and the key is not
    // below the node's (0x0042dd3a..0x0042dd47); the VisEffect at node + 0x10, else null. Every
    // caller passes a StringAlloc result, which is never null (an empty text is a valid key).
    std::map<std::string, FVisEffectArch, FAleNameLess>::iterator Found =
        G_VisEffects.find(std::string(Nickname));
    if (Found == G_VisEffects.end())
        return nullptr;
    return &Found->second;
}

FBeamEffectArch* FindBeamEffectArchByNickname(char const* Nickname)
{
    // (cdecl, &StringAlloc'd CacheString): the lower bound of the field
    // (the routine), a hit when it is not the head (the field) and the key is not below
    // the node's (0x0042e9fa..0x0042ea07); node + 0x10, else null. The one caller (vis_beam,
    // ..) passes a StringAlloc result.
    std::map<std::string, FBeamEffectArch, FAleNameLess>::iterator Found =
        G_BeamArches.find(std::string(Nickname));
    if (Found == G_BeamArches.end())
        return nullptr;
    return &Found->second;
}

FEffectArch* FindEffectArch(unsigned const* NicknameId)
{
    // (cdecl). 0x0040f082..0x0040f08c: the routine, lower_bound on the
    // registry with the key read through the pointer.
    std::map<unsigned, FEffectArch*>::iterator const Found = G_EffectArchs.lower_bound(*NicknameId);
    // end, or a key below the node's (unsigned JC), is a miss.
    if (Found == G_EffectArchs.end() || *NicknameId < Found->first)
        return nullptr;
    // the node's value (+0x10).
    return Found->second;
}

FEffectArch* FindEffectArchByNickname(char const* Nickname)
{
    if (Nickname == nullptr || Nickname[0] == '\0')
        return nullptr;
    unsigned const Id = CreateID(Nickname);
    return FindEffectArch(&Id);
}

void EffectArchAddRef(FEffectArch* Arch, unsigned UnusedArg)
{
    // . 0x0040e963: VisEffect (+0x08) non-null -> the routine; its result is
    // not read.
    (void)UnusedArg;
    if (Arch->VisEffect != nullptr)
        VisEffectArchAddRef(Arch->VisEffect);
    // SoundArch (+0x18) non-null -> vt+0x0c (slot 3, the routine acquire).
    if (Arch->SndEffect != nullptr)
        AcquireClientSoundArch(Arch->SndEffect);
    // .
    Arch->RefCount = Arch->RefCount + 1;
}

void EffectArchRelease(FEffectArch* Arch)
{
    // . 0x0040e993: VisEffect non-null -> the routine; result not read.
    if (Arch->VisEffect != nullptr)
        VisEffectArchRelease(Arch->VisEffect);
    // SoundArch -> vt+0x10 (slot 4, the routine release).
    if (Arch->SndEffect != nullptr)
        ReleaseClientSoundArch(Arch->SndEffect);
    // .
    Arch->RefCount = Arch->RefCount - 1;
}

FVisEffectArch::~FVisEffectArch()
{
    // , listing-traced 0x0042edb0..0x0042ee6f (ESI = the VisEffect at pair + 4).
    // only a VisEffect still referenced (+0x20 > 0) gives anything back.
    if (RefCount > 0)
    {
        // the routine on every texture slot (bound re-read each step),
        // then the list emptied (end = begin) -- the loop the routine has too.
        VisEffectArchReleaseTextures(this);
        // the routine(+0x24), then +0x24 = -1.
        AleLibRelease(AleHandle);
        AleHandle = -1;
    }
    // both vector buffers freed (+0x2c texture slots, +0x10 texture
    // names) -- the std::vector members' own destructors.
}

bool VisEffectArchAddRef(FVisEffectArch* Vis)
{
    // . 0x0042e084: a live reference only counts up (and answers false).
    bool bLoaded = false;
    if (Vis->RefCount > 0)
    {
        Vis->RefCount += 1;
        return bLoaded;
    }

    // texture slots are rebuilt when their count differs from the
    // texture names (both empty vectors count 0).
    if (Vis->TextureSlots.size() != Vis->Textures.size())
    {
        VisEffectArchReleaseTextures(Vis);
        // the routine; its answer is not read.
        VisEffectArchLoadTextures(Vis);
    }

    // an empty ale name is an error; the count still goes up.
    if (Vis->ParticleFxPath[0] == '\0')
    {
        FDUMP(0x100001, "%s(%d) : *** ERROR: %s",
              "Client\\cmn\\VisEffectArch.cpp", 0x13a,
              "[VisEffect]: Missing ale file name!");
        Vis->RefCount += 1;
        return bLoaded;
    }

    // the routine(&ale name) -> +0x24.
    Vis->AleHandle = AleLibLoad(Vis->ParticleFxPath);
    if (Vis->AleHandle != -1)
        bLoaded = true;
    Vis->RefCount += 1;
    return bLoaded;
}

bool VisEffectArchRelease(FVisEffectArch* Vis)
{
    // . 0x0042e163: --count; anything but zero answers false.
    Vis->RefCount -= 1;
    if (Vis->RefCount != 0)
        return false;

    // every texture slot released, the list emptied.
    VisEffectArchReleaseTextures(Vis);
    // the routine(+0x24), then +0x24 = -1.
    AleLibRelease(Vis->AleHandle);
    Vis->AleHandle = -1;
    return true;
}

int AleLibLoad(char const* AleName)
{
    // (cdecl; baseline takes the CacheString by address and reads its char*).
    // an empty name is refused outright.
    if (AleName[0] == '\0')
        return -1;

    // a registered name gains a use and hands back its handle.
    int Handle = -1;
    std::map<std::string, FAleLibEntry, FAleNameLess>::iterator Found =
        G_AleLib.find(std::string(AleName));
    if (Found != G_AleLib.end())
    {
        Found->second.RefCount += 1;
        Handle = Found->second.Handle;
    }
    if (Handle != -1)
        return Handle;

    // nothing more without NeuronRuntime::ParticleFx.
    if (NeuronRuntime::ParticleFx == nullptr)
        return Handle;

    // the NeuronRuntime::OpenData-shaped FileSystem desc (as the routine
    // builds it), created through NeuronRuntime::Data vt+0xc.
    FFileSystemAggDesc Desc;
    memset(&Desc, 0, sizeof(Desc));
    Desc.Size = 0x34;
    Desc.ComponentName = "FileSystem";
    Desc.DataPath = AleName;
    Desc.Field10 = 0x80000000u;
    Desc.Field14 = 1;
    Desc.Field1C = 3;
    Desc.Field20 = 0x8000080u;
    Desc.Field30 = 0xffffffffu;

    void* FsOut = nullptr;
    if (static_cast<IFileSystem*>(NeuronRuntime::Data)
            ->CreateInstance(reinterpret_cast<AGGDESC*>(&Desc), &FsOut) == 0)
    {
        IFileSystem* const Filesys = static_cast<IFileSystem*>(FsOut);
        // ParticleFx vt+0xc (LoadNodeLibrary), 0x0040125e: FxEffectLibrary vt+0x20
        // (LoadAle); neither result is read.
        NeuronRuntime::ParticleFx->LoadNodeLibrary(Filesys);
        NeuronRuntime::FxEffectLibrary->LoadAle(Filesys);

        // a fresh handle, registered with one use.
        Handle = G_AleLibNextHandle;
        G_AleLibNextHandle += 1;
        FAleLibEntry Entry;
        Entry.RefCount = 1;
        Entry.Handle = Handle;
        G_AleLib.insert(std::make_pair(std::string(AleName), Entry));
    }
    else
    {
        // .
        FDUMP(0x100001, "%s(%d) : *** ERROR: Failed to load %s.",
              "Client\\cmn\\ALELib.cpp", 0x5c, AleName);
    }

    // the file system is released whichever way it went.
    if (FsOut != nullptr)
        static_cast<IFileSystem*>(FsOut)->Release();
    return Handle;
}

int AleLibRelease(int Handle)
{
    // (cdecl). 0x00401309: -1 is answered with -1.
    if (Handle == -1)
        return -1;

    // linear walk for the entry holding Handle; none -> -1.
    std::map<std::string, FAleLibEntry, FAleNameLess>::iterator It = G_AleLib.begin();
    while (It != G_AleLib.end())
    {
        if (It->second.Handle == Handle)
            break;
        ++It;
    }
    if (It == G_AleLib.end())
        return -1;

    // one use fewer; below zero is an error. Nothing is unloaded.
    It->second.RefCount -= 1;
    int const Count = It->second.RefCount;
    if (Count != 0 && Count < 0)
    {
        FDUMP(0x100001, "%s(%d) : *** ERROR: %s ref count < 0!",
              "Client\\cmn\\ALELib.cpp", 0x7d, It->first.c_str());
    }
    return Count;
}

int AleLibShutdown()
{
    // (cdecl, no arguments).
    // an in-order walk; a positive use count (node + 0x10, JLE skips
    // zero and below) is reported with the name and the count, and counted (EBX).
    int Dangling = 0;
    std::map<std::string, FAleLibEntry, FAleNameLess>::iterator It = G_AleLib.begin();
    for (; It != G_AleLib.end(); ++It)
    {
        if (It->second.RefCount > 0)
        {
            FDUMP(0x100002, "%s(%d) : *** WARNING: ALELib::Shutdown(): %s has %d dangling refs.",
                  "Client\\cmn\\ALELib.cpp", 0x96, It->first.c_str(),
                  It->second.RefCount);
            ++Dangling;
        }
    }

    // erase(begin, end) -- whole-tree free (the routine per subtree)
    // or node by node; the map ends empty. The handle counter the field is left alone.
    G_AleLib.clear();
    return Dangling;
}
