// Author: Giperion.

#include "knowledge_map.h"

#include "character_select_equip.h" // GetPlayerVisitFlagsbaseline / SetPlayerVisitFlagsbaseline (the routine / the routine)
#include "globals.h"                // CopyBoundedString (the routine)

#include "common_id.h"         // CreateID
#include "common_ini.h"        // INI_Reader
#include "common_reputation.h" // Reputation::get_id / get_name
#include "common_misc.h"       // GetFileSysPath
#include "common_text.h"       // FmtStr
#include "common_tstring.h"    // TString<16>
#include "common_universe.h"   // Universe::get_system / get_base / get_zone

#include <stdint.h>

FKnowledgeMap* G_KnowledgeMap = nullptr;

namespace
{
    // The find every method inlines (the routine / the routine / the routine lower_bound,
    // then `end or Key < node key` is a miss).
    FKnowledgeMapEntry const* KnowledgeFind(FKnowledgeMap const* Map, unsigned Key)
    {
        std::map<unsigned, FKnowledgeMapEntry>::const_iterator const It = Map->Entries.find(Key);
        if (It == Map->Entries.end())
        {
            return nullptr;
        }
        return &It->second;
    }

    // One {Id, Flags} line of MarkKnown's walk (0x00459250..0x0045929e): a known id whose flags
    // miss some of the line's gets them OR-ed in; an unknown id gets the line's flags. Both
    // notify the server.
    void KnowledgeMarkLine(unsigned Id, unsigned char Flags)
    {
        unsigned char Known = 0;
        if (GetPlayerVisitFlagsbaseline(Id, &Known, 0, 0))
        {
            if (static_cast<unsigned char>(Flags & Known) == Flags)
            {
                return;
            }
            SetPlayerVisitFlagsbaseline(Id, static_cast<unsigned char>(Known | Flags), true);
            return;
        }
        SetPlayerVisitFlagsbaseline(Id, Flags, true);
    }
}

void FKnowledgeMap::Load()
{
    // .
    char Path[0x100];
    GetFileSysPath(Path, "interface\\KnowledgeMap.ini", nullptr);
    INI_Reader Reader;
    if (!Reader.open(Path, false) || !Reader.read_header())
    {
        return;
    }
    do
    {
        if (!Reader.is_header("KnowledgeMapTable") || !Reader.read_value())
        {
            continue;
        }
        do
        {
            if (!Reader.is_value("map"))
            {
                continue;
            }
            // key = ftol(value 0), Id = CreateID(value 1),
            // Flags = low byte of ftol(value 2).
            unsigned const Key = static_cast<unsigned>(static_cast<int>(Reader.get_value_float(0)));
            unsigned Id = CreateID(Reader.get_value_string(1));
            unsigned char const Flags =
                static_cast<unsigned char>(static_cast<int>(Reader.get_value_float(2)));
            if ((Flags & 0x40) != 0)
            {
                // a faction -- Reputation::get_id of the nickname as a
                // TString<16> (empty for a missing value).
                char const* const Nickname = Reader.get_value_string(1);
                TString<16> Name;
                Name.Length = 0;
                Name.Data[0] = '\0';
                if (Nickname != nullptr)
                {
                    Name.Length = CopyBoundedString(Name.Data, 0x10, Nickname);
                }
                Id = Reputation::get_id(Name);
            }

            // a new key gets a tree node; a known one a link after its
            // first line.
            std::map<unsigned, FKnowledgeMapEntry>::iterator const It = Entries.find(Key);
            if (It == Entries.end())
            {
                FKnowledgeMapEntry Entry;
                Entry.Id = Id;
                Entry.Flags = Flags;
                Entries.insert(std::pair<unsigned const, FKnowledgeMapEntry>(Key, Entry));
            }
            else
            {
                FKnowledgeMapLink Link;
                Link.Id = Id;
                Link.Flags = Flags;
                It->second.More.push_front(Link);
            }
        } while (Reader.read_value());
    } while (Reader.read_header());
}

void FKnowledgeMap::MarkKnown(unsigned Key)
{
    // (RET 4).
    FKnowledgeMapEntry const* const Entry = KnowledgeFind(this, Key);
    if (Entry == nullptr)
    {
        return;
    }
    KnowledgeMarkLine(Entry->Id, Entry->Flags);
    for (std::forward_list<FKnowledgeMapLink>::const_iterator It = Entry->More.begin();
         It != Entry->More.end(); ++It)
    {
        KnowledgeMarkLine(It->Id, It->Flags);
    }
}

bool FKnowledgeMap::Resolve(FmtStr const* Text, unsigned* OutId, unsigned char* OutFlags)
{
    // (RET 0xc).
    FKnowledgeMapEntry const* Entry = KnowledgeFind(this, Text->MadLibId);
    if (Entry == nullptr)
    {
        // the values in list order; the jump table 0x00459424 over
        // TypeTag - 0x42 (0x00459438).
        for (FmtStr::Val const* Val = Text->Head; Val != nullptr; Val = Val->Next)
        {
            unsigned Key = 0;
            switch (Val->TypeTag)
            {
            case 0x73: // 's'
                Key = Val->Value;
                // marked known through the field first.
                G_KnowledgeMap->MarkKnown(Key);
                break;
            case 0x53: // 'S'
                Key = (unsigned)Universe::get_system(Val->Value)->GetStridName();
                break;
            case 0x42: // 'B' -- BASE vtable +0x8 (strid_name)
                Key = static_cast<unsigned>(Universe::get_base(Val->Value)->StridName);
                break;
            case 0x46: // 'F'
                Key = Reputation::get_name(Val->Value);
                break;
            default:
                continue;
            }
            Entry = KnowledgeFind(this, Key);
            if (Entry != nullptr)
            {
                break;
            }
        }
        if (Entry == nullptr)
        {
            return false;
        }
    }

    *OutId = Entry->Id;
    *OutFlags = Entry->Flags;
    return true;
}

void KnowledgeMapCreate()
{
    // .
    if (G_KnowledgeMap != nullptr)
    {
        return;
    }
    FKnowledgeMap* const Map = new FKnowledgeMap();
    Map->Load();
    G_KnowledgeMap = Map;
}

void KnowledgeMapDestroy()
{
    // .
    FKnowledgeMap* const Map = G_KnowledgeMap;
    if (Map == nullptr)
    {
        return;
    }
    delete Map; // + the routine
    G_KnowledgeMap = nullptr;
}

void KnowledgeMapReveal(unsigned Key)
{
    // .
    if (G_KnowledgeMap != nullptr)
    {
        G_KnowledgeMap->MarkKnown(Key);
    }
}

void KnowledgeMapRevealFmtStr(FmtStr const* Text)
{
    // .
    if (G_KnowledgeMap == nullptr)
    {
        return;
    }
    G_KnowledgeMap->MarkKnown(Text->MadLibId);

    // the jump table 0x00459a5c over TypeTag - 0x21 (0x00459a78).
    // is re-read for every call.
    for (FmtStr::Val const* Val = Text->Head; Val != nullptr; Val = Val->Next)
    {
        switch (Val->TypeTag)
        {
        case 0x73: // 's'
            G_KnowledgeMap->MarkKnown(Val->Value);
            break;
        case 0x53: // 'S'
        {
            unsigned const Key =
                (unsigned)Universe::get_system(Val->Value)->GetStridName();
            G_KnowledgeMap->MarkKnown(Key);
            break;
        }
        case 0x42: // 'B'
        {
            unsigned const Key =
                static_cast<unsigned>(Universe::get_base(Val->Value)->StridName);
            G_KnowledgeMap->MarkKnown(Key);
            break;
        }
        case 0x46: // 'F'
        {
            unsigned const Key = Reputation::get_name(Val->Value);
            G_KnowledgeMap->MarkKnown(Key);
            break;
        }
        case 0x5a: // 'Z' -- only a zone that exists (0x00459a22)
        {
            Universe::IZone const* const Zone = Universe::get_zone(Val->Value);
            if (Zone != nullptr)
            {
                G_KnowledgeMap->MarkKnown(static_cast<unsigned>(Zone->GetName()));
            }
            break;
        }
        case 0x21: // '!' -- a nested FmtStr
            KnowledgeMapRevealFmtStr(Val->Subtree);
            break;
        default:
            break;
        }
    }
}

bool KnowledgeMapLookupFmtStr(FmtStr const* Text, unsigned* OutId, unsigned char* OutFlags)
{
    // .
    if (G_KnowledgeMap == nullptr)
    {
        return false;
    }
    return G_KnowledgeMap->Resolve(Text, OutId, OutFlags);
}
