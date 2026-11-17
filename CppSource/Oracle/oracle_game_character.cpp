// Data dumps of the character databases of Common (work package P2.1): body
// parts, accessories and costumes (CostumeDescriptions), the new character
// database (NewChar::TheDB), the performance slider (RtcSlider) and the
// static reputation vibes the universe load fills (Reputation::Vibe),
// written after oracle_game_data.cpp has run the [Data] list.
// crates/game-sim/tests/support/render_character.rs renders the same lines.
//
// CostumeDescriptions and NewChar::Database keep their maps private, and
// DetailSwitchTable its table. The views below declare the same members in
// the same order, so they have the same layout (each pinned by its size);
// the dumps read the records through them and change nothing.

#include "oracle_support.h"
#include "oracle_game.h"

#include <string.h>

#include <list>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "common_ini.h"
#include "common_id.h"
#include "common_text.h"
#include "common_reputation.h"
#include "common_newchar.h"
#include "common_universe.h"

using namespace oracle;

namespace
{
    struct FSwitchTableView
    {
        std::vector<std::pair<float, float>> Table;
        float FovXHalf;
        float SizeX;
    };
    static_assert(sizeof(FSwitchTableView) == sizeof(DetailSwitchTable), "DetailSwitchTable layout");

    struct FCostumeView
    {
        std::list<std::string> AnimationNames;
        std::list<std::string> PreloadNames;
        std::list<std::list<std::string>> PetalAnimations;
        std::list<DetailSwitchTable> DetailSwitchTables;
        std::map<unsigned, bodypart> Bodyparts[4];
        std::map<unsigned, accessory> Accessories;
        std::map<unsigned, costume> Costumes;
    };
    static_assert(sizeof(FCostumeView) == sizeof(CostumeDescriptions), "CostumeDescriptions layout");

    struct FNewCharView
    {
        void* Vtable;
        std::map<unsigned, FNewCharPilot> Pilots;
        std::map<unsigned, FNewCharPackage> Packages;
        std::map<unsigned, FNewCharFaction> Factions;
        std::map<unsigned, FNewCharExtra> Extras;
    };
    static_assert(sizeof(FNewCharView) == sizeof(NewChar::Database), "NewChar::Database layout");

    std::string Texts(const std::list<std::string>& List)
    {
        std::string Out;
        for (const std::string& Text : List)
        {
            if (!Out.empty())
                Out += ";";
            Out += GameText(Text.c_str());
        }
        return Out;
    }

    std::string Texts(const std::vector<std::string>& List)
    {
        std::string Out;
        for (const std::string& Text : List)
        {
            if (!Out.empty())
                Out += ";";
            Out += GameText(Text.c_str());
        }
        return Out;
    }

    std::string Ids(const std::set<unsigned>& Set)
    {
        std::string Out;
        for (unsigned Id : Set)
        {
            if (!Out.empty())
                Out += ";";
            Out += GameId(Id);
        }
        return Out;
    }

    std::string I(int Value)
    {
        return Format("%d", Value);
    }

    std::string U(unsigned int Value)
    {
        return Format("%u", Value);
    }

    // The slider values the rtc_slider dump probes (render_character.rs
    // SLIDER_PROBES).
    const unsigned int kSliderProbes[] = {
        0xbfc00000u, 0xbf800000u, 0xbf7fffffu, 0x80000001u, 0x00000000u, 0x3e2e147au,
        0x3e2e147bu, 0x3f028f5bu, 0x3f028f5cu, 0x3f3fffffu, 0x3f400000u, 0x3f547ae0u,
        0x3f547ae1u, 0x3f800000u, 0x40000000u, 0x7f800000u, 0x7fc00000u,
    };

    void DumpBodyparts(Suite& Out, const FCostumeView* View)
    {
        if (View == nullptr)
            return;
        for (const std::string& Name : View->AnimationNames)
        {
            FLine L;
            L.Add("anim", GameText(Name.c_str()));
            Out.Line(L.Done());
        }
        for (const std::string& Name : View->PreloadNames)
        {
            FLine L;
            L.Add("preload", GameText(Name.c_str()));
            Out.Line(L.Done());
        }
        unsigned int Index = 0;
        for (const std::list<std::string>& List : View->PetalAnimations)
        {
            FLine L;
            L.Add("petal_list", U(Index++));
            L.Add("anims", Texts(List));
            Out.Line(L.Done());
        }
        Index = 0;
        for (const DetailSwitchTable& Table : View->DetailSwitchTables)
        {
            const FSwitchTableView* T = reinterpret_cast<const FSwitchTableView*>(&Table);
            std::string Pairs;
            for (const std::pair<float, float>& Pair : T->Table)
            {
                if (!Pairs.empty())
                    Pairs += ";";
                Pairs += GameF(Pair.first) + "," + GameF(Pair.second);
            }
            FLine L;
            L.Add("switch_table", U(Index++));
            L.Add("fov_x_half", GameF(T->FovXHalf));
            L.Add("size_x", GameF(T->SizeX));
            L.Add("table", Pairs);
            Out.Line(L.Done());
        }
        for (int Type = 0; Type < 4; ++Type)
        {
            for (const std::pair<const unsigned, bodypart>& Entry : View->Bodyparts[Type])
            {
                const bodypart& Part = Entry.second;
                int Switch = -1;
                int Position = 0;
                for (const DetailSwitchTable& Table : View->DetailSwitchTables)
                {
                    if (&Table == Part.Switch)
                    {
                        Switch = Position;
                        break;
                    }
                    ++Position;
                }
                int Petal = -1;
                Position = 0;
                for (const std::list<std::string>& List : View->PetalAnimations)
                {
                    if (&List == Part.PetalAnimations)
                    {
                        Petal = Position;
                        break;
                    }
                    ++Position;
                }
                FLine L;
                L.Add("part_type", I(Type));
                L.Add("part", GameId((unsigned int)Part.Id));
                L.Add("mesh", GameText(Part.Mesh.c_str()));
                L.Add("sex", I(Part.Sex));
                L.Add("switch", I(Switch));
                L.Add("petal", I(Petal));
                Out.Line(L.Done());
            }
        }
    }

    void DumpAccessories(Suite& Out, const FCostumeView* View)
    {
        if (View == nullptr)
            return;
        for (const std::pair<const unsigned, accessory>& Entry : View->Accessories)
        {
            const accessory& A = Entry.second;
            FLine L;
            L.Add("accessory", GameId((unsigned int)A.Id));
            L.Add("mesh", GameText(A.Mesh.c_str()));
            L.Add("hardpoint", GameText(A.AccessoryHardpoint.c_str()));
            L.Add("body_hardpoint", GameText(A.CharacterHardpoint.c_str()));
            L.Add("materials", Texts(A.Materials));
            Out.Line(L.Done());
        }
    }

    void DumpCostumes(Suite& Out, const FCostumeView* View)
    {
        if (View == nullptr)
            return;
        for (const std::pair<const unsigned, costume>& Entry : View->Costumes)
        {
            const costume& C = Entry.second;
            std::string Parts;
            for (int Type = 0; Type < 4; ++Type)
            {
                if (Type != 0)
                    Parts += ",";
                Parts += GameId(C.Parts[Type] != nullptr ? (unsigned int)C.Parts[Type]->Id : 0u);
            }
            std::string Accessories;
            for (unsigned int Slot = 0; Slot < C.AccessoryCount; ++Slot)
            {
                if (!Accessories.empty())
                    Accessories += ";";
                Accessories += GameId((unsigned int)C.Accessories[Slot]->Id);
            }
            FLine L;
            L.Add("costume", GameId(C.Id));
            L.Add("gender", I(C.Gender));
            L.Add("parts", Parts);
            L.Add("accessories", Accessories);
            Out.Line(L.Done());
        }
    }

    void DumpNewChar(Suite& Out)
    {
        const FNewCharView* View =
            reinterpret_cast<const FNewCharView*>(static_cast<NewChar::Database*>(NewChar::TheDB));
        for (const std::pair<const unsigned, FNewCharFaction>& Entry : View->Factions)
        {
            const FNewCharFaction& X = Entry.second;
            FLine L;
            L.Add("faction", GameId(X.NicknameId));
            L.Add("rep_group", GameText(X.RepGroup.c_str()));
            L.Add("bases", Ids(X.Bases));
            L.Add("packages", Ids(X.Packages));
            L.Add("pilots", Ids(X.Pilots));
            Out.Line(L.Done());
        }
        for (const std::pair<const unsigned, FNewCharPackage>& Entry : View->Packages)
        {
            const FNewCharPackage& X = Entry.second;
            FLine L;
            L.Add("package", GameId(X.NicknameId));
            L.Add("strid_name", I(X.StridName));
            L.Add("strid_desc", I(X.StridDesc));
            L.Add("ship", GameText(X.Ship.c_str()));
            L.Add("loadout", GameText(X.Loadout.c_str()));
            L.Add("money", I(X.Money));
            Out.Line(L.Done());
        }
        for (const std::pair<const unsigned, FNewCharPilot>& Entry : View->Pilots)
        {
            const FNewCharPilot& X = Entry.second;
            FLine L;
            L.Add("pilot", GameId(X.NicknameId));
            L.Add("body", GameText(X.Body.c_str()));
            L.Add("body_anims", Texts(X.BodyAnims));
            L.Add("comm", GameText(X.Comm.c_str()));
            L.Add("comm_anims", Texts(X.CommAnims));
            L.Add("voice", GameText(X.Voice.c_str()));
            L.Add("thumb", GameText(X.Thumb.c_str()));
            Out.Line(L.Done());
        }
        for (const std::pair<const unsigned, FNewCharExtra>& Entry : View->Extras)
        {
            FLine L;
            L.Add("extra", GameId(Entry.first));
            L.Add("value", GameText(Entry.second.Value.c_str()));
            Out.Line(L.Done());
        }
    }

    void DumpRtcSlider(Suite& Out, const std::set<unsigned int>& Names)
    {
        const float Saved = RtcSlider::get_slider();
        for (unsigned int Bits : kSliderProbes)
        {
            RtcSlider::set_slider(FloatFromBits(Bits));
            FLine L;
            L.Add("slider", Format("%08x", Bits));
            L.Add("fps_cap", GameF(RtcSlider::get_fps_cap()));
            Out.Line(L.Done());
        }
        for (unsigned int Name : Names)
        {
            std::string Answers;
            for (unsigned int Bits : kSliderProbes)
            {
                RtcSlider::set_slider(FloatFromBits(Bits));
                Answers += RtcSlider::is_enabled((unsigned long)Name) ? "1" : "0";
            }
            FLine L;
            L.Add("setting", GameId(Name));
            L.Add("enabled", Answers);
            Out.Line(L.Done());
        }
        RtcSlider::set_slider(Saved);
    }
}

void DumpCharacters(FGameKind& K, const std::set<unsigned int>& RtcNames)
{
    const FCostumeView* View = reinterpret_cast<const FCostumeView*>(GetCostumeDescriptions());
    {
        Suite Out(K.Path("bodyparts"));
        DumpBodyparts(Out, View);
    }
    {
        Suite Out(K.Path("accessories"));
        DumpAccessories(Out, View);
    }
    {
        Suite Out(K.Path("costumes"));
        DumpCostumes(Out, View);
    }
    {
        Suite Out(K.Path("new_char"));
        DumpNewChar(Out);
    }
    {
        Suite Out(K.Path("rtc_slider"));
        DumpRtcSlider(Out, RtcNames);
    }
}

void DumpVibes(FGameKind& K)
{
    // The probes: the nickname id of every object of every system file.
    std::set<unsigned int> Probes;
    for (Universe::ISystem* System = Universe::GetFirstSystem(); System != nullptr;
         System = Universe::GetNextSystem())
    {
        const int Count = System->GetObjectCount();
        for (int Index = 0; Index < Count; ++Index)
        {
            Universe::FSystemObject O;
            memset(&O, 0, sizeof(O));
            if (!System->GetSystemObject((unsigned int)Index, &O))
                continue;
            Probes.insert(CreateID(O.Nickname.Data));
        }
    }
    Suite Out(K.Path("vibes"));
    {
        FLine L;
        L.Add("count", U(Reputation::Vibe::Count()));
        Out.Line(L.Done());
    }
    for (unsigned int Probe : Probes)
    {
        const int Key = (int)Probe;
        FLine L;
        L.Add("vibe", GameId(Probe));
        const bool bFound = Reputation::Vibe::Verify(Key);
        L.Add("found", bFound ? "1" : "0");
        if (bFound)
        {
            unsigned int Affiliation = 0;
            unsigned int Rank = 0;
            unsigned int Infocard = 0;
            FmtStr Name1;
            FmtStr Name2;
            const wchar_t* Display = nullptr;
            Reputation::Vibe::GetAffiliation(Key, Affiliation, false);
            Reputation::Vibe::GetRank(Key, Rank);
            Reputation::Vibe::GetInfocard(Key, Infocard);
            Reputation::Vibe::GetName(Key, Name1, Name2, Display);
            L.Add("affiliation", GameId(Affiliation));
            L.Add("rank", U(Rank));
            L.Add("infocard", U(Infocard));
            L.Add("name", U(Name1.MadLibId) + "," + U(Name2.MadLibId));
        }
        Out.Line(L.Done());
    }
}
