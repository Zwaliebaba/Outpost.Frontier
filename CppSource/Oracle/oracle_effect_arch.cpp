// Oracle suite of work package P5.6 (Oracle.exe game-app): the client's effect archetypes through
// EffectArchLoadIni (vis_effect_arch.cpp) and the effect-type and LOD tables
// (effect_type_info.cpp), both compiled in, over
// crates/game-app/tests/fixtures/oracle/effect_arch.list and the files under effects/. The Rust
// test crates/game-app/tests/oracle_vectors.rs renders the same lines.
//
// Input lines: "case NAME" starts from the initial tables (the registry, the VisEffect and beam
// maps emptied, the type table back to its .data rows, the LOD rows -1); "ini FILE" is
// EffectArchLoadIni(FILE, false). The run-time parts (the .ale loads, the texture slots) are not
// reached: no VisEffect is referenced. The sounds are the process's client sound table, which
// holds none of the fixtures' names. Archetype::ResolveMaterialEntry, which only a reference
// would call, is stood in below.
//
// Oracle.exe dump runs RunEffectArchDataDump (kind "effect_arch"): the [Data] effects values of
// EXE\OutpostFrontier.ini in order through EffectArchLoadIni("DATA\<value>"), as the client's
// LoadClientDataValue does, then the same lines as case "data" into
// <output>\effect_arch\effects.jsonl, the "ini" lines naming the [Data] value. The Rust test
// data_dump_effect_arch (crates/game-app/tests/effects_data.rs) renders the same lines. The
// sound table is empty here too, so the loader keeps no sound; the harness reads each [Effect]
// section's last snd_effect value itself, the first section of a nickname winning as the
// registry keeps it, and writes {"case","sound","v"} per effect in id order after the tables.
// Nothing it writes may be committed.
//
// Output lines (floats as eight hex digits of the f32, texts in hex, "-" for none):
//   {"case","ini","r"}                 per file, the registry's growth
// then at the end of each case
//   {"case","type","v"}                per type row 0..46: priority, generic priority, lod type,
//                                      radius, the two offscreen flags, run time, fps, the two
//                                      pbubble ranges
//   {"case","lod","v"}                 per LOD row 0..7: max lod, min lod, min screen size
//   {"case","vis","v"}                 per VisEffect in map order: crc, particlefx, the textures
//                                      (':'-joined), has_alpha
//   {"case","beam","v"}                per beam in map order: the kind, the two texture modes, the
//                                      28 floats from tip_color on
//   {"case","effect","v"}              per EffectArch in id order: the type, the VisEffect's,
//                                      the generic one's and the beam's nickname, a sound found,
//                                      the light name, range scale, time scale, radius, colour
//                                      dword, flags byte

#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <string>
#include <vector>

#include "oracle_support.h"

#include "../OutpostFrontier/Client/vis_effect_arch.cpp"
#include "../OutpostFrontier/Client/effect_type_info.cpp"

namespace Archetype
{
    FCliMaterialEntry* ResolveMaterialEntry(char const*, bool, unsigned)
    {
        return nullptr;
    }
}

using namespace oracle;

namespace
{
    FEffectTypeInfo GInitialTypes[kEffectTypeCount];
    bool bGInitialSaved = false;

    std::string HexOrDash(char const* Text)
    {
        return Text != nullptr ? HexC(Text) : std::string("-");
    }

    std::string F(float Value)
    {
        return Format("%08x", FloatBits(Value));
    }

    void ResetTables()
    {
        if (!bGInitialSaved)
        {
            memcpy(GInitialTypes, G_EffectTypeTable, sizeof(GInitialTypes));
            bGInitialSaved = true;
        }
        for (std::map<unsigned, FEffectArch*>::iterator It = G_EffectArchs.begin();
             It != G_EffectArchs.end(); ++It)
        {
            EffectArchDestruct(It->second);
            operator delete(It->second);
        }
        G_EffectArchs.clear();
        G_VisEffects.clear();
        G_BeamArches.clear();
        memcpy(G_EffectTypeTable, GInitialTypes, sizeof(GInitialTypes));
        for (int Row = 0; Row < kEffectLodTableRows; ++Row)
        {
            G_EffectLodTable[Row].MaxLodScreenSize = -1.0f;
            G_EffectLodTable[Row].MinLodScreenSize = -1.0f;
            G_EffectLodTable[Row].MinScreenSize = -1.0f;
        }
    }

    void PrintTables(Suite& Out, const std::string& Case)
    {
        const std::string Head = "{\"case\":\"" + Case + "\",";
        for (int Index = 0; Index < kEffectTypeCount; ++Index)
        {
            FEffectTypeInfo const& T = G_EffectTypeTable[Index];
            Out.Line(Head + Format("\"type\":\"%d\",\"v\":\"", Index) + F(T.Priority) + "," +
                     F(T.GenericPriority) + "," + Format("%d", T.LodType) + "," + F(T.Radius) + "," +
                     Format("%d,%d", T.VisibilityOffscreen, T.UpdateOffscreen) + "," + F(T.RunTime) +
                     "," + F(T.Fps) + "," + F(T.PbubbleNear) + "," + F(T.PbubbleFar) + "\"}");
        }
        for (int Row = 0; Row < kEffectLodTableRows; ++Row)
        {
            FEffectLodSizes const& L = G_EffectLodTable[Row];
            Out.Line(Head + Format("\"lod\":\"%d\",\"v\":\"", Row) + F(L.MaxLodScreenSize) + "," +
                     F(L.MinLodScreenSize) + "," + F(L.MinScreenSize) + "\"}");
        }
        for (std::map<std::string, FVisEffectArch, FAleNameLess>::const_iterator It =
                 G_VisEffects.begin();
             It != G_VisEffects.end(); ++It)
        {
            FVisEffectArch const& V = It->second;
            std::string Textures;
            for (size_t Index = 0; Index < V.Textures.size(); ++Index)
            {
                Textures += Index == 0 ? "" : ":";
                Textures += HexC(V.Textures[Index].c_str());
            }
            Out.Line(Head + "\"vis\":\"" + HexC(V.Nickname) + "\",\"v\":\"" +
                     Format("%08x", static_cast<unsigned>(V.EffectCrc)) + "," + HexC(V.ParticleFxPath) +
                     "," + Textures + "," + (V.bHasAlpha ? "1" : "0") + "\"}");
        }
        for (std::map<std::string, FBeamEffectArch, FAleNameLess>::const_iterator It =
                 G_BeamArches.begin();
             It != G_BeamArches.end(); ++It)
        {
            FBeamEffectArch const& B = It->second;
            std::string Floats;
            float const* Values = &B.ColorCoreR;
            for (int Index = 0; Index < 28; ++Index)
                Floats += "," + F(Values[Index]);
            Out.Line(Head + "\"beam\":\"" + HexOrDash(B.Nickname) + "\",\"v\":\"" +
                     Format("%d,%d,%d", B.SegmentMode, B.UvModeV, B.UvModeU) + Floats + "\"}");
        }
        for (std::map<unsigned, FEffectArch*>::const_iterator It = G_EffectArchs.begin();
             It != G_EffectArchs.end(); ++It)
        {
            FEffectArch const* E = It->second;
            const unsigned Color = (static_cast<unsigned>(E->LgtColorA) << 24) |
                                   (static_cast<unsigned>(E->LgtColorB) << 16) |
                                   (static_cast<unsigned>(E->LgtColorG) << 8) |
                                   static_cast<unsigned>(E->LgtColorR);
            Out.Line(Head + Format("\"effect\":\"%08x\",\"v\":\"%d,", E->NicknameId, E->EffectType) +
                     HexOrDash(E->VisEffect != nullptr ? E->VisEffect->Nickname : nullptr) + "," +
                     HexOrDash(E->VisGeneric != nullptr ? E->VisGeneric->Nickname : nullptr) + "," +
                     HexOrDash(E->VisBeam != nullptr ? E->VisBeam->Nickname : nullptr) + "," +
                     (E->SndEffect != nullptr ? "1" : "0") + "," + HexC(E->LgtEffect) + "," +
                     F(E->LgtRangeScale) + "," + F(E->LgtTimeScale) + "," + F(E->LgtRadius) + "," +
                     Format("%08x,%d", Color, E->Flags50) + "\"}");
        }
    }
}

void RunEffectArchSuite()
{
    Suite Out("effect_arch.jsonl");
    std::string Case;
    for (const std::string& Line : InputLines("effect_arch.list"))
    {
        if (Line.compare(0, 5, "case ") == 0)
        {
            if (!Case.empty())
                PrintTables(Out, Case);
            Case = Line.substr(5);
            ResetTables();
        }
        else if (Line.compare(0, 4, "ini ") == 0)
        {
            const std::string File = Line.substr(4);
            const int Grown = EffectArchLoadIni(File.c_str(), false);
            Out.Line("{\"case\":\"" + Case + "\",\"ini\":\"" + File + "\",\"r\":\"" +
                     Format("%d", Grown) + "\"}");
        }
        else
        {
            fprintf(stderr, "effect_arch.list: bad line %s\n", Line.c_str());
            GFailed = true;
        }
    }
    if (!Case.empty())
        PrintTables(Out, Case);
    ResetTables();
}

void RunEffectArchDataDump(const std::vector<std::string>& Files)
{
    (void)Files;
    ResetTables();
    Suite Out("effect_arch\\effects.jsonl");
    INI_Reader Reader;
    if (!Reader.open("EXE\\OutpostFrontier.ini", false))
    {
        // No engine INI under the dump root (the synthetic tree of build.yml): an empty dump, as
        // the Rust test reads it.
        printf("effect_arch: no EXE\\OutpostFrontier.ini, nothing loaded\n");
        return;
    }
    std::vector<std::string> Values;
    while (Reader.read_header())
    {
        if (!Reader.is_header("Data"))
            continue;
        while (Reader.read_value())
        {
            if (Reader.is_value("effects"))
                Values.push_back(Reader.get_value_string());
        }
    }
    Reader.close();
    for (const std::string& Value : Values)
    {
        const std::string Path = "DATA\\" + Value;
        const int Grown = EffectArchLoadIni(Path.c_str(), false);
        Out.Line("{\"case\":\"data\",\"ini\":\"" + HexC(Value.c_str()) + "\",\"r\":\"" +
                 Format("%d", Grown) + "\"}");
    }
    PrintTables(Out, "data");
    // The snd_effect names, which the loader resolves against the empty sound table and drops.
    std::map<unsigned, std::string> Sounds;
    for (const std::string& Value : Values)
    {
        const std::string Path = "DATA\\" + Value;
        INI_Reader File;
        if (!File.open(Path.c_str(), false))
            continue;
        while (File.read_header())
        {
            if (!File.is_header("Effect"))
                continue;
            unsigned Nickname = 0;
            bool bHasSound = false;
            std::string Sound;
            while (File.read_value())
            {
                if (File.is_value("nickname"))
                    Nickname = CreateID(File.get_value_string());
                else if (File.is_value("snd_effect"))
                {
                    char const* const Name = File.get_value_string();
                    Sound = Name != nullptr ? Name : "";
                    bHasSound = true;
                }
            }
            if (Sounds.find(Nickname) == Sounds.end())
                Sounds[Nickname] = bHasSound ? HexC(Sound.c_str()) : std::string("-");
        }
        File.close();
    }
    for (std::map<unsigned, FEffectArch*>::const_iterator It = G_EffectArchs.begin();
         It != G_EffectArchs.end(); ++It)
    {
        std::map<unsigned, std::string>::const_iterator Found = Sounds.find(It->first);
        Out.Line(Format("{\"case\":\"data\",\"sound\":\"%08x\",\"v\":\"", It->first) +
                 (Found != Sounds.end() ? Found->second : std::string("-")) + "\"}");
    }
    printf("effect_arch: %u files, %u effects, %u visual effects, %u beams\n",
           (unsigned int)Values.size(), (unsigned int)G_EffectArchs.size(),
           (unsigned int)G_VisEffects.size(), (unsigned int)G_BeamArches.size());
    ResetTables();
}
