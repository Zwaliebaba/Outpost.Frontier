// Author: Giperion.

#include "swapped_material.h"

#include "NeuronRuntime.h"
#include "common_misc.h"   // malloc_strcpy / malloc_strfree, DAEngine_create_filesystem / DAEngine_release_filesystem
#include "neuroncomponents.h"         // FDUMP, NEURONCOMPONENTS_CRC::GetCRC32
#include "render/shading.h" // IMaterialLibrary, ITextureLibrary2, FShadingPropertyRecord

#include <string.h>

namespace
{
    char const* const kSwappedMaterialCpp = "Client\\cmn\\SwappedMaterial.cpp";
    char const* const kNoDtName = "%s(%d) : *** ERROR: Cannot find Dt_name parameter in material %s";
    char const* const kNoMaterial = "%s(%d) : *** ERROR: cannot find material name %s\n";

    // The TextureLib slot 12 query: +0x04 is the name's CRC. baseline leaves +0x00 as whatever
    // the stack held (0x0042ce9e / 0x0042d5ae / 0x0042d5ea); it is zero here.
    struct FTextureQuery
    {
        unsigned long Field_0x00; // TODO: never written by these callers
        unsigned long Crc;        // +0x04
    };

    // Slot 12 writes straight into the member (baseline passes its address), so a miss leaves the
    // member as it was.
    void FindTextureRecord(char const* Name, void** Out)
    {
        FTextureQuery Query = {};
        Query.Crc = NEURONCOMPONENTS_CRC::GetCRC32(Name);
        NeuronRuntime::TextureLib->FindTextureByCrc(&Query, Out);
    }

    // The inlined record destructor of 0x0042cee4..0x0042cf6b / 0x0042d3ec..0x0042d45f: Saved
    // (the routine), then Default and Current (the the routine / the routine switches) --
    // each drops what it owns.
    void ReleasePropertyRecord(FShadingPropertyRecord& Record)
    {
        Record.Saved.ReleaseOwnedPayload();
        Record.Default.ReleaseOwnedPayload();
        Record.Current.ReleaseOwnedPayload();
    }

    // The material search of 0x0042cd1b..0x0042cda7 / 0x0042d1eb..0x0042d279: every material of
    // MaterialLib group 0 (slots 21 / 22), its name (slot 12, 0x3ff characters) compared with
    // _strnicmp over Name's length; a non-matching material is released. Null when none matches.
    IMaterial* FindMaterial(char const* Name)
    {
        int Count = 0;
        NeuronRuntime::MaterialLib->GetGroupMaterialCount(0, &Count);
        for (unsigned long Index = 0; Index < static_cast<unsigned long>(Count); ++Index)
        {
            IMaterial* Material = nullptr;
            NeuronRuntime::MaterialLib->EnumGroupMaterial(0, Index, &Material);
            char MaterialNameBuffer[0x400];
            Material->GetName(MaterialNameBuffer, 0x3ff);
            if (_strnicmp(MaterialNameBuffer, Name, strlen(Name)) == 0)
                return Material;
            Material->Release();
        }
        return nullptr;
    }

    // `REPE CMPSB` over 8 bytes against "Dt_name" (0x005ca818), the terminator included.
    bool IsDtName(char const* ParameterName)
    {
        return memcmp(ParameterName, "Dt_name", 8) == 0;
    }
}

FSwappedMaterial::FSwappedMaterial()
{

    MaterialName = nullptr;
    OriginalTexture = nullptr;
    OriginalTexRecord = nullptr;
    SwapTexRecord = nullptr;
    bSwapped = 0;
}

void FSwappedMaterial::Reset()
{

    MaterialName = nullptr;
    OriginalTexture = nullptr;
    OriginalTexRecord = nullptr;
    SwapTexRecord = nullptr;
    bSwapped = 0;
}

void FSwappedMaterial::Release()
{
    // . 0x0042cc55..0x0042cc98: a swap is undone first.
    if (bSwapped != 0)
    {
        SetDtName(OriginalTexture);
        if (OriginalTexRecord != nullptr)
        {
            NeuronRuntime::TextureLib->ReleaseTextureRecord(OriginalTexRecord); // slot 14 (+0x38)
            OriginalTexRecord = nullptr;
        }
        if (SwapTexRecord != nullptr)
        {
            NeuronRuntime::TextureLib->ReleaseTextureRecord(SwapTexRecord);
            SwapTexRecord = nullptr;
        }
        bSwapped = 0;
    }

    // the strings, the records again, every field zero.
    malloc_strfree(&MaterialName);
    malloc_strfree(&OriginalTexture);
    if (OriginalTexRecord != nullptr)
    {
        NeuronRuntime::TextureLib->ReleaseTextureRecord(OriginalTexRecord);
        OriginalTexRecord = nullptr;
    }
    if (SwapTexRecord != nullptr)
    {
        NeuronRuntime::TextureLib->ReleaseTextureRecord(SwapTexRecord);
        SwapTexRecord = nullptr;
    }
    MaterialName = nullptr;
    OriginalTexRecord = nullptr;
    SwapTexRecord = nullptr;
    bSwapped = 0;
    OriginalTexture = nullptr;
}

bool FSwappedMaterial::CaptureOriginal()
{
    // .
    IMaterial* const Material = FindMaterial(MaterialName);
    if (Material == nullptr)
        return false;

    // every parameter; the string Dt_name is kept.
    unsigned long ParameterCount = 0;
    Material->GetParameterCount(&ParameterCount); // slot 16 (+0x40)
    bool bFound = false;
    for (unsigned long Parameter = 0; Parameter < ParameterCount; ++Parameter)
    {
        FShadingPropertyRecord Descriptor = {};
        Material->GetParameterDescriptor(Parameter, &Descriptor); // slot 17 (+0x44)
        if (IsDtName(Descriptor.Name))
        {
            FShadingPropertyValue Value = {};
            Material->GetParameterValue(Parameter, &Value); // slot 18 (+0x48)
            if (Value.Type == 7)
            {
                malloc_strcpy(&OriginalTexture, reinterpret_cast<char const*>(Value.Payload0));
                FindTextureRecord(OriginalTexture, &OriginalTexRecord);
                bFound = true;
            }
            Value.ReleaseOwnedPayload();
        }
        ReleasePropertyRecord(Descriptor);
    }

    // no string Dt_name.
    if (!bFound)
        FDUMP(0x100001, kNoDtName, kSwappedMaterialCpp, 0x9c, MaterialName);
    Material->Release();
    return true;
}

void FSwappedMaterial::SetDtName(char const* TextureName)
{
    // .
    IMaterial* const Material = FindMaterial(MaterialName);
    if (Material == nullptr)
    {

        FDUMP(0x100001, kNoMaterial, kSwappedMaterialCpp, 0xdf, MaterialName);
        return;
    }

    // every string Dt_name parameter becomes a ProcessHeap copy of
    // TextureName (none when TextureName is null) and is written back through slot 19.
    unsigned long ParameterCount = 0;
    Material->GetParameterCount(&ParameterCount);
    bool bFound = false;
    for (unsigned long Parameter = 0; Parameter < ParameterCount; ++Parameter)
    {
        FShadingPropertyRecord Descriptor = {};
        Material->GetParameterDescriptor(Parameter, &Descriptor);
        if (IsDtName(Descriptor.Name))
        {
            FShadingPropertyValue Value = {};
            Material->GetParameterValue(Parameter, &Value);
            if (Value.Type == 7)
            {
                if (Value.Payload0 != 0)
                {
                    HeapFree(GetProcessHeap(), 0, reinterpret_cast<void*>(Value.Payload0));
                    Value.Payload0 = 0;
                }
                if (TextureName != nullptr)
                {
                    // HeapAlloc(strlen + 1), not checked, byte copy.
                    size_t const Size = strlen(TextureName) + 1;
                    char* const Copy = static_cast<char*>(HeapAlloc(GetProcessHeap(), 0, Size));
                    memcpy(Copy, TextureName, Size);
                    Value.Payload0 = reinterpret_cast<unsigned long>(Copy);
                }
                Material->SetParameterValue(Parameter, &Value); // slot 19 (+0x4c)
                bFound = true;
            }
            Value.ReleaseOwnedPayload();
        }
        ReleasePropertyRecord(Descriptor);
    }


    if (!bFound)
        FDUMP(0x100001, kNoDtName, kSwappedMaterialCpp, 0xd4, MaterialName);

    // slot 6 (+0x18) with (0, 1.0f), then the reference goes.
    Material->ApplyVerify(nullptr, reinterpret_cast<void*>(0x3f800000u));
    Material->Release();
}

bool FSwappedMaterial::Init(char const* NewMaterialName)
{
    // AL is still CaptureOriginal's result at the RET.
    malloc_strcpy(&MaterialName, NewMaterialName);
    bool const bResult = CaptureOriginal();
    bSwapped = 0;
    return bResult;
}

void FSwappedMaterial::Swap(char const* TextureName, char const* UtfFile)
{
    // . 0x0042d4f6..0x0042d540: undo an earlier swap.
    if (bSwapped != 0)
    {
        SetDtName(OriginalTexture);
        if (OriginalTexRecord != nullptr)
        {
            NeuronRuntime::TextureLib->ReleaseTextureRecord(OriginalTexRecord);
            OriginalTexRecord = nullptr;
        }
        if (SwapTexRecord != nullptr)
        {
            NeuronRuntime::TextureLib->ReleaseTextureRecord(SwapTexRecord);
            SwapTexRecord = nullptr;
        }
        bSwapped = 0;
    }

    // the texture from the UTF file. When slot 6 does not load the
    // named child (nonzero result), the whole "Texture library" is loaded as a group (slot 5),
    // looked up, and the group is garbage-collected (slot 7).
    IFileSystem* FileSystem = nullptr;
    DAEngine_create_filesystem(UtfFile, &FileSystem, "UTF");
    if (NeuronRuntime::TextureLib->LoadTextureLibrary(FileSystem, TextureName) != 0) // slot 6 (+0x18)
    {
        int Group;
        NeuronRuntime::TextureLib->LoadTextureGroup(FileSystem, &Group); // +0x14
        FindTextureRecord(TextureName, &SwapTexRecord);
        NeuronRuntime::TextureLib->GarbageCollect(Group);     // +0x1c
    }
    else
    {
        FindTextureRecord(TextureName, &SwapTexRecord);
    }
    DAEngine_release_filesystem(&FileSystem);


    SetDtName(TextureName);
    bSwapped = 1;
}
