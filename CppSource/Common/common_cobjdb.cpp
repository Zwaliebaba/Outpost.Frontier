// Author: Giperion.

#include "common_cobjdb.h"

#include "common_archetype.h"
#include "common_id.h"
#include "common_ini.h"
#include "common_reputation.h"
#include "common_text.h"
#include "common_universe.h"

#include <cstring>

// ============================================================================
// SolarReps — baseline Common.dll the routine / the routine / the routine
// (0x58a80 FindSolarRep). Called from Universe system-file load after open,
// before Zone/Object parse (LoadUniverseIni opens each System file= and runs
// this pass, then reset). Seeds Reputation::Vibe entries for every [Object]
// nickname so CSolar::ReadObj's Vibe::Verify gate can pass.
// ============================================================================

namespace
{
    // (module storage-062b8c16). cdecl, five arguments; returns the vibe handle,
    // which is the nickname id copied out of *Nick (062b8ac8-062b8ad5) -- Vibe::Alloc does not
    // write it back.
    int AllocObjectVibe(ID_String const* Nick, unsigned IdsName, TString<16> const* Group,
                        unsigned Infocard, Archetype::Solar const* Arch)
    {
        int Handle = (int)Nick->Id;
        Reputation::Vibe::Alloc(Handle);

        // get_id(Group); SetAffiliation(&Handle, id, false). The false is
        // the EBX pushed before get_id's argument and left on the stack after ADD ESP,4.
        Reputation::Vibe::SetAffiliation(Handle, Reputation::get_id(*Group), false);

        // two FmtStr locals built inline (Head, MadLibId, 14 index bytes
        // zeroed). 062b8b91 begin_mad_lib(IdsName) (folded setter 06309b30) and 062b8b9a
        // end_mad_lib (empty 062a8920), both on the first one.
        FmtStr Name1;
        FmtStr Name2;
        Name1.begin_mad_lib(IdsName);
        Name1.end_mad_lib();
        Reputation::Vibe::SetName(Handle, Name1, Name2, nullptr);

        Reputation::Vibe::SetInfocard(Handle, Infocard);

        // SetRank(&Handle, Arch->Toughness) only when an archetype resolved.
        if (Arch != nullptr)
            Reputation::Vibe::SetRank(Handle, (unsigned)Arch->Toughness);

        return Handle;                                               // / 062b8c08
    }

    // (module storage-062b8dd7). One [Object] section -> the routine.
    int LoadOneObjectSection(INI_Reader& Reader)
    {
        // Nick.clear(), Group {Length 0, Data[0] 0}, IdsName / Infocard /
        // Arch 0.
        ID_String Nick;
        Nick.clear();
        TString<16> Group;
        Group.Length = 0;
        Group.Data[0] = '\0';
        unsigned IdsName = 0;
        unsigned Infocard = 0;
        Archetype::Solar const* Arch = nullptr;

        if (Reader.read_value())
        {
            do
            {
                // the section stops being read once a name, a nickname and
                // a group are all known (IdsName != 0, !Nick.IsEmpty(), Group.Length != 0).
                if (IdsName != 0 && !Nick.IsEmpty() && Group.Length != 0)
                    break;

                if (Reader.is_value("nickname"))
                {
                    Nick.Id = CreateID(Reader.get_value_string());
                }
                else if (Reader.is_value("ids_name"))
                {
                    IdsName = (unsigned)Reader.get_value_int(0);
                }
                else if (Reader.is_value("infocard_ids") || Reader.is_value("ids_info"))
                {
                    Infocard = (unsigned)Reader.get_value_int(0);
                }
                else if (Reader.is_value("group"))
                {
                    char const* Text = Reader.get_value_string();
                    if (Text == nullptr)
                    {
                        Group.Length = 0;
                        Group.Data[0] = '\0';
                    }
                    else
                    {
                        // strlen, capped at 15, copied, terminated.
                        unsigned Len = (unsigned)std::strlen(Text);
                        if (Len >= 16u)
                            Len = 15u;
                        std::memcpy(Group.Data, Text, Len);
                        Group.Data[Len] = '\0';
                        Group.Length = Len;
                    }
                }
                else if (Reader.is_value("archetype"))
                {
                    ID_String ArchName;
                    ArchName.Id = CreateID(Reader.get_value_string());
                    Arch = Archetype::GetSolar(Archetype::GetAsteroidID(ArchName));
                }
            } while (Reader.read_value());
        }

        // no nickname -> -1, otherwise the routine.
        if (Nick.IsEmpty())
            return -1;
        return AllocObjectVibe(&Nick, IdsName, &Group, Infocard, Arch);
    }
}

int __cdecl SolarReps::FindSolarRep(ID_String const& Id)
{
    // Verify(Id.Id) ? Id.Id : -1.
    if (Reputation::Vibe::Verify((int)Id.Id))
        return (int)Id.Id;
    return -1;
}

void __cdecl Reputation::SolarReps::LoadObjectVibesFromSystemIni(INI_Reader& Reader)
{
    // (module storage-062b8e15): every "Object" header -> the routine; its
    // return value is dropped (062b8e06).
    while (Reader.read_header())
    {
        if (Reader.is_header("Object"))
            LoadOneObjectSection(Reader);
    }
}
