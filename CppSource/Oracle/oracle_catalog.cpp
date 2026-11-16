// Oracle.exe catalog <resource DLL directory> <output directory>
// (work package P1.6, design/decisions.md T21).
//
// Loads each of the eight resource DLLs as a data file and dumps what the
// game's text lookups read from it, one JSON object per line, to
// <output directory>\<Module>.jsonl:
//
//   {"string":"<id>","len":"<units>","text":"<UTF-16LE hex>"}
//       for all 16 entries of every RT_STRING block, in ID order. The block
//       and entry are found as GetIdsStringW (OutpostFrontier/Client/
//       ids_strings.cpp) and LoadResourceString (OutpostServer/
//       ResourceStrings.cpp) find them: FindResourceExA(module, 6,
//       (id >> 4) + 1, 0), then the length-prefixed entries walked. An
//       undefined ID in an existing block has length 0.
//   {"card":"<id>","size":"<bytes>","data":"<hex>"}
//       for every resource of type 23, in ID order, as LockRdlResource
//       (ids_rdl.cpp) and LoadRawTextResource (ids_strings.cpp) find it:
//       FindResourceExA(module, 23, id, 0), SizeofResource bytes.
//
// crates/neuron-core/tests/catalog_dump.rs renders the same lines from the
// catalogs `cargo xtask catalog` builds. The text is game content: the
// output must never be committed.

#include "oracle_support.h"

#include <windows.h>

#include <stdio.h>

#include <algorithm>
#include <string>
#include <vector>

using namespace oracle;

namespace
{
    // The client's modules in the order of their index (design/facts.md F3), then the
    // server's MFC resource DLL.
    const char* const kModules[] = {
        "UiText",          "FieldGuideText",  "DialogueText",    "NameCatalog",
        "EquipmentCatalog", "NegotiationText", "InterfaceExtras", "serverinterface",
    };

    const int kStringTableType = 6;
    const int kCardType = 23;

    struct NameList
    {
        std::vector<unsigned int> Ids;
        bool bNamed;
    };

    BOOL CALLBACK CollectName(HMODULE /*Module*/, LPCSTR /*Type*/, LPSTR Name, LONG_PTR Param)
    {
        NameList* const List = reinterpret_cast<NameList*>(Param);
        if (IS_INTRESOURCE(Name))
            List->Ids.push_back((unsigned int)(ULONG_PTR)Name);
        else
            List->bNamed = true;
        return TRUE;
    }

    BOOL CALLBACK CountLanguage(HMODULE /*Module*/, LPCSTR /*Type*/, LPCSTR /*Name*/, WORD /*Language*/,
                                LONG_PTR Param)
    {
        ++*reinterpret_cast<int*>(Param);
        return TRUE;
    }

    // The integer names of every resource of Type, sorted. A missing type is an empty list.
    bool ResourceIds(HMODULE Module, int Type, const char* ModuleName, std::vector<unsigned int>& Out)
    {
        NameList List;
        List.bNamed = false;
        EnumResourceNamesA(Module, MAKEINTRESOURCEA(Type), CollectName, reinterpret_cast<LONG_PTR>(&List));
        if (List.bNamed)
        {
            fprintf(stderr, "%s: a type %d resource has a string name\n", ModuleName, Type);
            return false;
        }
        std::sort(List.Ids.begin(), List.Ids.end());
        List.Ids.erase(std::unique(List.Ids.begin(), List.Ids.end()), List.Ids.end());
        bool bOk = true;
        for (unsigned int Id : List.Ids)
        {
            // The game asks for language 0; the catalogs assume one language per resource.
            int Languages = 0;
            EnumResourceLanguagesA(Module, MAKEINTRESOURCEA(Type), MAKEINTRESOURCEA(Id), CountLanguage,
                                   reinterpret_cast<LONG_PTR>(&Languages));
            if (Languages != 1)
            {
                fprintf(stderr, "%s: type %d resource %u has %d languages\n", ModuleName, Type, Id,
                        Languages);
                bOk = false;
            }
        }
        Out = List.Ids;
        return bOk;
    }

    const unsigned char* LockById(HMODULE Module, int Type, unsigned int Id, DWORD& OutBytes)
    {
        OutBytes = 0;
        HRSRC const Info = FindResourceExA(Module, MAKEINTRESOURCEA(Type), MAKEINTRESOURCEA(Id), 0);
        if (Info == nullptr)
            return nullptr;
        OutBytes = SizeofResource(Module, Info);
        HGLOBAL const Data = LoadResource(Module, Info);
        if (Data == nullptr)
            return nullptr;
        return static_cast<const unsigned char*>(LockResource(Data));
    }

    bool DumpStrings(HMODULE Module, const char* ModuleName, Suite& Out)
    {
        std::vector<unsigned int> Blocks;
        bool bOk = ResourceIds(Module, kStringTableType, ModuleName, Blocks);
        for (unsigned int Block : Blocks)
        {
            DWORD Bytes = 0;
            const unsigned char* const Data = LockById(Module, kStringTableType, Block, Bytes);
            if (Data == nullptr || Block == 0)
            {
                fprintf(stderr, "%s: string block %u does not load\n", ModuleName, Block);
                bOk = false;
                continue;
            }
            // Walk the 16 length-prefixed entries; unlike the game, stay inside the resource.
            DWORD Offset = 0;
            for (unsigned int Entry = 0; Entry < 16; ++Entry)
            {
                const unsigned int Id = (Block - 1) * 16 + Entry;
                if (Offset + 2 > Bytes)
                {
                    fprintf(stderr, "%s: string block %u ends at entry %u\n", ModuleName, Block, Entry);
                    bOk = false;
                    break;
                }
                const unsigned int Length = Data[Offset] | (Data[Offset + 1] << 8);
                Offset += 2;
                if (Offset + Length * 2 > Bytes)
                {
                    fprintf(stderr, "%s: string %u runs past its block\n", ModuleName, Id);
                    bOk = false;
                    break;
                }
                Out.Line(Format("{\"string\":\"%u\",\"len\":\"%u\",\"text\":\"", Id, Length) +
                         Hex(Data + Offset, Length * 2) + "\"}");
                Offset += Length * 2;
            }
        }
        return bOk;
    }

    bool DumpCards(HMODULE Module, const char* ModuleName, Suite& Out)
    {
        std::vector<unsigned int> Ids;
        bool bOk = ResourceIds(Module, kCardType, ModuleName, Ids);
        for (unsigned int Id : Ids)
        {
            DWORD Bytes = 0;
            const unsigned char* const Data = LockById(Module, kCardType, Id, Bytes);
            if (Data == nullptr)
            {
                fprintf(stderr, "%s: card %u does not load\n", ModuleName, Id);
                bOk = false;
                continue;
            }
            Out.Line(Format("{\"card\":\"%u\",\"size\":\"%lu\",\"data\":\"", Id, (unsigned long)Bytes) +
                     Hex(Data, Bytes) + "\"}");
        }
        return bOk;
    }
}

void RunCatalogDump()
{
    unsigned int Loaded = 0;
    for (const char* const Name : kModules)
    {
        const std::string Path = GFixtureDir + "\\" + Name + ".dll";
        HMODULE const Module = LoadLibraryExA(Path.c_str(), nullptr, LOAD_LIBRARY_AS_DATAFILE);
        if (Module == nullptr)
        {
            fprintf(stderr, "cannot load %s (error %lu)\n", Path.c_str(), GetLastError());
            GFailed = true;
            continue;
        }
        {
            Suite Out(GOutputDir + "\\" + Name + ".jsonl");
            if (!DumpStrings(Module, Name, Out))
                GFailed = true;
            if (!DumpCards(Module, Name, Out))
                GFailed = true;
        }
        FreeLibrary(Module);
        ++Loaded;
    }
    printf("dumped %u resource DLLs\n", Loaded);
}
