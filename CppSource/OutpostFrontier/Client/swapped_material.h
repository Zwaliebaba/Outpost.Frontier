// Author: Giperion.

#pragma once

// baseline Client\cmn\SwappedMaterial.cpp (OutpostFrontier.exe 0x0042cc10..0x0042d612): a material
// whose "Dt_name" parameter (the diffuse texture name) is swapped for a texture loaded from a
// UTF file, and put back afterwards. Held by value in NN_MissionVendor (+0x35c, the video
// window's "MV_vidmaterial") and at 0x004a4031 (+ the routine / 004a579c / 004a57d8).

// Not polymorphic, 0x14 bytes, no RTTI. The strings are Common's malloc_strcpy copies and are
// freed with malloc_strfree; the texture records are NeuronRuntime::TextureLib slot 12 lookups and are
// released with slot 14.
struct FSwappedMaterial
{
    // (thiscall, returns this) -- every field zero.
    FSwappedMaterial();

    // is also the destructor: NN_MissionVendor's EH state 2 (the routine
    // ) and its ctor's unwind run it on the member.
    ~FSwappedMaterial() { Release(); }

    FSwappedMaterial(FSwappedMaterial const&) = delete;
    FSwappedMaterial& operator=(FSwappedMaterial const&) = delete;

    // (thiscall) -- every field zero again (the same stores as the ctor).
    void Reset();

    // (thiscall) -- put the original texture back when swapped, release both
    // texture records, free both strings and zero every field.
    void Release();

    // (thiscall, RET 4) -- copy MaterialName, capture the material's current
    // Dt_name (the routine) and clear bSwapped; returns what the capture returned (false when
    // no material in NeuronRuntime::MaterialLib's group 0 starts with the name).
    bool Init(char const* NewMaterialName);

    // (thiscall, RET 8) -- undo an earlier swap, open UtfFile as a "UTF" file
    // system, load TextureName from it and set it as the material's Dt_name. The texture name
    // is the first argument ([ESP+0x18] at 0x0042d561), the file the second ([ESP+0x1c] at
    // ): NN_MissionVendor passes (offer icon texture, "...MissionVendor.txm").
    void Swap(char const* TextureName, char const* UtfFile);

    // (thiscall) -- find the first material of MaterialLib group 0 whose name
    // starts with MaterialName (_strnicmp over MaterialName's length) and keep its string
    // Dt_name (OriginalTexture, its TextureLib record in OriginalTexRecord). FDUMP when the
    // material has no string Dt_name. False only when no material matches.
    bool CaptureOriginal();

    // (thiscall, RET 4) -- set the matching material's string Dt_name parameter
    // to TextureName (slot 19), then apply-verify the material at 1.0 (slot 6). FDUMP when no
    // material matches or it has no string Dt_name.
    void SetDtName(char const* TextureName);

    char* MaterialName;        // +0x00 malloc_strcpy
    char* OriginalTexture;     // +0x04 malloc_strcpy of the captured Dt_name
    void* OriginalTexRecord;   // +0x08 TextureLib slot 12 record of OriginalTexture
    void* SwapTexRecord;       // +0x0c TextureLib slot 12 record of the swapped-in texture
    unsigned char bSwapped;    // +0x10
    unsigned char Pad11[3];
};

static_assert(sizeof(FSwappedMaterial) == 0x14, "SwappedMaterial is 0x14 bytes (NN_MissionVendor +0x35c..+0x370)");
