// Author: Giperion.

#include "texture_shapes.h"

#include "fl_fx_mesh.h"      // FLFxMeshFactoryCreate (the routine), the field

#include "particlefx.h"         // IFxRuntime::BindMeshFactory
#include "common_ini.h"
#include "neuroncomponents.h"
#include "globals.h"
#include "readfile.h"
#include "NeuronRuntime.h"
#include "render/IRenderPipeline.h"
#include "render/shading.h"

#include <windows.h>

#include <new>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>

char const kShapeDbHeaderFull[] = "Client\\cmn\\ShapeDB.h";
char const kShapeDbHeaderLower[] = "cmn\\shapedb.h";
char const kShapeDbHeader[] = "cmn\\ShapeDB.h";

namespace
{
    // s_Client\cmn\ShapeDB.cpp (005c999c) -- every FDUMP in 004255c0 and
    // names this file.
    char const kShapeDbCpp[] = "Client\\cmn\\ShapeDB.cpp";

    // , the empty string baseline passes both to INI_Reader::log_link and as the
    // "%s" context argument of the loader's messages.
    char const kEmpty[] = "";

    // immediates.
    float const kZero = 0.0f;
    float const kOne = 1.0f;
    float const kHalf = 0.5f;     // (and the 0x3f000000 stored at 004258f3)
    float const kUvInset = 0.01f;

    // The {name, CRC32} query TextureLib slot +0x30 (Slot12) reads.
    struct FTexCrcQuery
    {
        char const* Name;  // +0
        unsigned long Crc; // +4
    };

    // The `[Data] effect_shapes =` queue, a VC6 vector<CacheString> at the field..20.
    // CacheString is one pointer; the routine overwrites it with the StringAlloc result, so the
    // element is exactly that pointer.
    std::vector<char const*> G_EffectShapeFiles;
}

FShapeDB G_MouseShapeDB;
FShapeDB G_HudShapeDB;
FShapeDB G_EffectShapeDB;
FShapeDB G_StarSphereShapeDB;
FShapeDB G_UiShapeDB;

// ---------------------------------------------------------------- texture reference

// (the static initializer of the HUD's the field): Field04 = -1, Texture = 0.
FTextureRef::FTextureRef() : Texture(nullptr), Field04(-1)
{
}


FTextureRef::FTextureRef(FTextureRef const& Other)
{
    Texture = nullptr;
    Texture = Other.Texture;
    if (Texture != nullptr)
        NeuronRuntime::TextureLib->CreatePlaybackGroup(Texture, nullptr);
    Field04 = Other.Field04;
}

// (also inlined as the branch and into the node destructor the routine).
FTextureRef::~FTextureRef()
{
    if (Texture != nullptr)
    {
        NeuronRuntime::TextureLib->ReleaseTextureRecord(Texture);
        Texture = nullptr;
    }
}

// -------------------------------------------------------------------- the routine

bool TextureRefLoad(FTextureRef* Ref, char const* Name)
{
    if (Ref->Texture != nullptr)
    {
        NeuronRuntime::TextureLib->ReleaseTextureRecord(Ref->Texture);
        Ref->Texture = nullptr;
    }

    FTexCrcQuery Query;
    Query.Name = Name;
    Query.Crc = NEURONCOMPONENTS_CRC::GetCRC32(Name);
    if (NeuronRuntime::TextureLib->FindTextureByCrc(&Query, &Ref->Texture) != GR_OK)
    {
        FDUMP(0x100001, "%s(%d) : *** ERROR: texture (%s) is not defined", kShapeDbCpp, 0x24,
              Name);
    }
    return Ref->Texture != nullptr;
}

// -------------------------------------------------------------------- the routine

void* TexShapeGetGpuHandle(FTextureRef const* Ref, int Frame)
{
    if (Ref->Texture == nullptr)
        return nullptr;

    unsigned long FrameInfo[5];
    NeuronRuntime::TextureLib->GetTextureFrame(Ref->Texture, Frame, FrameInfo);
    return reinterpret_cast<void*>(FrameInfo[0]);
}

// -------------------------------------------------------------------- the routine

void TexShapeSetTexture(FTextureRef* Ref, void* Texture)
{
    if (Ref->Texture != nullptr)
    {
        NeuronRuntime::TextureLib->ReleaseTextureRecord(Ref->Texture);
        Ref->Texture = nullptr;
    }
    Ref->Texture = Texture;
    if (Texture != nullptr)
        NeuronRuntime::TextureLib->CreatePlaybackGroup(Texture, nullptr);
}

// -------------------------------------------------------------------- the routine

void TexShapeSetRect(FTexShape* Shape, float X, float Y, float W, float H, float UpperLeftU,
                     float UpperLeftV, float LowerRightU, float LowerRightV)
{
    // Slot22(record, frame 0, out5) with no check of either the record or the
    // result. baseline leaves out5 uninitialised; the port zeroes it so a failed call reads 0.
    unsigned long Frame[5] = {0, 0, 0, 0, 0};
    NeuronRuntime::TextureLib->GetTextureFrame(Shape->Texture, 0, Frame);

    // Pipeline slot +0xcc with the handle and three out pointers; the two sizes are
    // zeroed first (004256d6/004256da) and the result is not checked.
    unsigned int TexW = 0;
    unsigned int TexH = 0;
    int Levels = 0;
    NeuronRuntime::Pipeline
        ->GetTextureDim(reinterpret_cast<void*>(Frame[0]), &TexW, &TexH, &Levels);

    // FILD of the {size, 0} qword: the sizes are unsigned.
    float const FTexW = static_cast<float>(TexW);
    float const FTexH = static_cast<float>(TexH);

    // a rect given in 0..1 is normalised, so scale it up to pixels. The x87 gate is
    // `TEST AH,0x41` + `JP`, i.e. the multiply runs only when both components are <= 1.
    if (X <= kOne && Y <= kOne)
    {
        X = FTexW * X;
        Y = FTexH * Y;
    }

    // a zero extent means "the whole texture"; a fractional one is normalised.
    if (W == kZero || H == kZero)
    {
        W = FTexW;
        H = FTexH;
    }
    else if (W <= kOne && H <= kOne)
    {
        W = FTexW * W;
        H = FTexH * H;
    }

    // the UV rect, inset by 0.01 of a pixel on every edge so neighbouring shapes in
    // an atlas never bleed into each other.
    float const Right = X + W;
    float const Bottom = Y + H;
    Shape->U0 = (X + kUvInset) / FTexW;
    Shape->V0 = (Y + kUvInset) / FTexH;
    Shape->U1 = (Right - kUvInset) / FTexW;
    Shape->V1 = (Bottom - kUvInset) / FTexH;
    Shape->PixelW = static_cast<int>(W);
    Shape->PixelH = static_cast<int>(H);

    // a negative corner means "take it from the rect".
    if (UpperLeftU < kZero)
        UpperLeftU = X;
    if (UpperLeftV < kZero)
        UpperLeftV = Y;
    if (LowerRightU < kZero)
        LowerRightU = Right;
    if (LowerRightV < kZero)
        LowerRightV = Bottom;

    // an odd pixel width puts the centre on a half-pixel. baseline applies the
    // correction to U only -- V gets no matching term (0042598c).
    float const SpanU = LowerRightU - UpperLeftU;
    float const SpanV = LowerRightV - UpperLeftV;
    int OddSpan = static_cast<int>(SpanU) & static_cast<int>(0x80000001);
    if (OddSpan < 0)
        OddSpan = ((OddSpan - 1) | static_cast<int>(0xfffffffe)) + 1;
    float const HalfPixelU = (OddSpan == 1) ? kHalf : kZero;

    // the divisors fall back to 1 so a degenerate rect cannot divide by zero.
    float const DivW = (W == kZero) ? kOne : W;
    float const DivH = (H == kZero) ? kOne : H;
    float const HalfW = DivW * kHalf;
    float const HalfH = DivH * kHalf;
    float const InvHalfW = kOne / HalfW;
    float const InvHalfH = kOne / HalfH;

    float const CenterX = X + HalfW;
    float const CenterY = Y + HalfH;
    float const MidU = UpperLeftU + SpanU * kHalf + HalfPixelU;
    float const MidV = UpperLeftV + SpanV * kHalf;

    Shape->ScaleU = InvHalfW * SpanU * kHalf;
    Shape->ScaleV = InvHalfH * SpanV * kHalf;
    Shape->OffsetU = (MidU - CenterX) * InvHalfW;
    Shape->OffsetV = (MidV - CenterY) * InvHalfH;
}

// ------------------------------------------------------------------------ ShapeDB

bool FShapeKeyLess::operator()(TString<24> const& A, TString<24> const& B) const
{
    return _stricmp(A.Data, B.Data) < 0;
}

FShapeDB::FShapeDB() : LastFound(nullptr)
{
}

unsigned FShapeDB::Count() const
{
    return static_cast<unsigned>(Shapes.size());
}

// -------------------------------------------------------------------- the routine

FTexShape* FShapeDB::FindOrCreate(TString<24> const& Key)
{
    // baseline copies Key and a value whose record is null into a pair (the routine and the MOV
    // chain at 00426e76), inserts it (the routine), destroys the temporaries and returns
    // node + 0x28. Its value is a stack local with only the record cleared; the port's value
    // is FTexShape() -- record null, Field04 -1 (FTextureRef's constructor), the rest zero.
    FShapeMap::value_type Entry(Key, FTexShape());
    std::pair<FShapeMap::iterator, bool> const Result = Shapes.insert(Entry);
    return &Result.first->second;
}

FTexShape* FShapeDB::Find(char const* Name, char const* SourceFile)
{
    TString<24> Key;
    Key = Name;

    FShapeMap::value_type* Hit = nullptr;
    if (LastFound != nullptr && _stricmp(LastFound->first.Data, Key.Data) == 0)
    {
        Hit = LastFound;
    }
    else
    {
        FShapeMap::iterator const It = Shapes.find(Key);
        Hit = (It == Shapes.end()) ? nullptr : &*It;
        LastFound = Hit;
    }

    if (Hit == nullptr)
    {
        FDUMP(0x100002, "%s(%d) : *** WARNING: ShapeDB: unable to find shape '%s'", SourceFile,
              0xf2, Name);
        return nullptr;
    }
    return &Hit->second;
}

// -------------------------------------------------------------------- the routine

void FShapeDB::Clear()
{
    for (FShapeMap::iterator It = Shapes.begin(); It != Shapes.end(); ++It)
    {
        if (It->second.Texture != nullptr)
        {
            NeuronRuntime::TextureLib->ReleaseTextureRecord(It->second.Texture);
            It->second.Texture = nullptr;
        }
    }
    Shapes.clear();
    LastFound = nullptr;
}

// -------------------------------------------------------------------- the routine

int FShapeDB::Load(char const* Path, IFileSystem* BaseDir)
{
    // a function-local static vector of the texture groups this call loaded (guard
    // , storage the field..18, freed at exit by the branch).
    static std::vector<int> LoadedGroups;

    INI_Reader Reader;
    ITextureLibrary2* const Library = NeuronRuntime::TextureLib;

    // generic slot 4 is saved, cleared for the load and restored at the end.
    unsigned long SavedSlot4 = 0;
    Library->GetGenericSlot(4, &SavedSlot4);
    Library->SetGenericSlot(4, 0);

    char FullPath[0x104];
    if (BaseDir != nullptr)
    {
        char BaseDirPath[0x104];
        BaseDir->GetFileSysPath(BaseDirPath, 0x104);
        _makepath(FullPath, nullptr, BaseDirPath, Path, nullptr);
    }
    else
    {
        strcpy(FullPath, Path);
    }

    if (!Reader.open(FullPath, false))
    {
        FDUMP(0x100001, "%s(%d) : *** ERROR: unable to load ShapeDB file '%s'", kShapeDbCpp,
              0x1eb, Path);
    }
    else
    {
        TString<260> CurrentFile; // ESP+0x4a8
        CurrentFile.Length = 0;
        CurrentFile.Data[0] = '\0';
        void* CurrentTexture = nullptr;    // ESP+0x18
        FTexShape* CurrentShape = nullptr; // ESP+0x1c
        // ESP+0x13: set by `file`, `shape_name` and [Shape]; cleared by `name`/`texture_name`,
        // which complain when it is already clear.
        bool bExpectTexture = true;

        while (Reader.read_header())
        {
            CurrentShape = nullptr; // , on every header

            if (Reader.is_header("Texture"))
            {
                while (Reader.read_value())
                {
                    if (Reader.is_value("file"))
                    {
                        CurrentTexture = nullptr;
                        bExpectTexture = true;
                        CurrentShape = nullptr;

                        TString<260> Value;
                        Value.Length = 0;
                        Value.Data[0] = '\0';
                        Value = Reader.get_value_string(0);
                        if (Value.Length == 0 || _stricmp(CurrentFile.Data, Value.Data) == 0)
                            continue;

                        CurrentFile.Length = CopyBoundedString(CurrentFile.Data, 0x104, Value.Data);

                        // a DOS "FileSystem" descriptor for the archive, created
                        // under NeuronRuntime::Data.
                        IFileSystem* Archive = nullptr;
                        FFileSystemDesc Desc;
                        memset(&Desc, 0, sizeof(Desc));
                        Desc.Size = 0x34;
                        Desc.ComponentName = "FileSystem";
                        Desc.Path = CurrentFile.Data;
                        Desc.Access = 0x80000000u;
                        Desc.ShareMode = 1;
                        Desc.Creation = 3;
                        Desc.Flags = 0x8000080u;
                        Desc.Field30 = 0xffffffffu;
                        if (NeuronRuntime::Data->CreateInstance(reinterpret_cast<AGGDESC*>(&Desc),
                                                        reinterpret_cast<void**>(&Archive)) == GR_OK)
                        {
                            // slot +0x14 loads the archive and names its group; the
                            // group is queued whatever the result. baseline's group local is
                            // uninitialised; the port starts it at -1.
                            int Group = -1;
                            Library->LoadTextureGroup(Archive, &Group);
                            LoadedGroups.push_back(Group);
                        }
                        else
                        {
                            Reader.log_link(kEmpty);
                            FDUMP(0x100001, "%s(%d) : *** ERROR: %s\nfile not found (%s)",
                                  kShapeDbCpp, 0x14c, kEmpty, CurrentFile.Data);
                        }
                        if (Archive != nullptr)
                        {
                            Archive->Release();
                            Archive = nullptr;
                        }
                    }
                    else if (Reader.is_value("name") || Reader.is_value("texture_name"))
                    {
                        if (!bExpectTexture)
                        {
                            Reader.log_link(kEmpty);
                            FDUMP(0x100001,
                                  "%s(%d) : *** ERROR: %s\nshape definition never followed texture",
                                  kShapeDbCpp, 0x155, kEmpty);
                        }
                        CurrentShape = nullptr;
                        bExpectTexture = false;

                        TString<260> Name;
                        Name.Length = 0;
                        Name.Data[0] = '\0';
                        char const* const Value = Reader.get_value_string(0);
                        if (Value != nullptr)
                            Name.Length = CopyBoundedString(Name.Data, 0x104, Value);

                        void* Texture = nullptr;
                        FTexCrcQuery Query;
                        Query.Name = Name.Data;
                        Query.Crc = NEURONCOMPONENTS_CRC::GetCRC32(Name.Data);
                        if (Library->FindTextureByCrc(&Query, &Texture) != GR_OK)
                        {
                            Reader.log_link(kEmpty);
                            FDUMP(0x100002, "%s(%d) : *** WARNING: '%s'\n\nmissing texture (%s)",
                                  kShapeDbCpp, 0x110, kEmpty, Name.Data);
                        }
                        else
                        {
                            // Slot12's reference is dropped at once; shapes take
                            // their own through slot +0x34.
                            Library->ReleaseTextureRecord(Texture);
                        }
                        CurrentTexture = Texture;
                    }
                    else if (Reader.is_value("shape_name"))
                    {
                        bExpectTexture = true;
                        if (CurrentTexture == nullptr)
                        {
                            Reader.log_link(kEmpty);
                            FDUMP(0x100001,
                                  "%s(%d) : *** ERROR: %s\n'shape_name' field must be preceeded by "
                                  "valid 'texture_name' field",
                                  kShapeDbCpp, 0x164, kEmpty);
                        }

                        TString<260> ShapeName;
                        ShapeName.Length = 0;
                        ShapeName.Data[0] = '\0';
                        char const* const Value = Reader.get_value_string(0);
                        if (Value != nullptr)
                            ShapeName.Length = CopyBoundedString(ShapeName.Data, 0x104, Value);

                        TString<24> Key;
                        Key = ShapeName.Data;
                        FTexShape* const Shape = FindOrCreate(Key);
                        if (Shape->Texture != nullptr)
                        {
                            CurrentShape = nullptr;
                            Reader.log_link(kEmpty);
                            FDUMP(0x100001, "%s(%d) : *** ERROR: %s\nshape name (%s) already found",
                                  kShapeDbCpp, 0x16b, kEmpty, ShapeName.Data);
                        }
                        else
                        {
                            TexShapeSetTexture(Shape, CurrentTexture);
                            if (Shape->Texture != nullptr)
                                TexShapeSetRect(Shape, 0.f, 0.f, 0.f, 0.f, -1.f, -1.f, -1.f, -1.f);
                            CurrentShape = Shape;
                        }
                    }
                    else if (Reader.is_value("tex_shape"))
                    {
                        CurrentShape = nullptr;
                        CurrentTexture = nullptr;
                        if (!bExpectTexture)
                        {
                            Reader.log_link(kEmpty);
                            FDUMP(0x100001,
                                  "%s(%d) : *** ERROR: %s\nshape definition never followed texture",
                                  kShapeDbCpp, 0x175, kEmpty);
                        }

                        char const* const Value = Reader.get_value_string(0);
                        void* Texture = nullptr;
                        FTexCrcQuery Query;
                        Query.Name = Value;
                        Query.Crc = NEURONCOMPONENTS_CRC::GetCRC32(Value);
                        if (Library->FindTextureByCrc(&Query, &Texture) != GR_OK)
                        {
                            Reader.log_link(kEmpty);
                            FDUMP(0x100002, "%s(%d) : *** WARNING: '%s'\n\nmissing texture (%s)",
                                  kShapeDbCpp, 0x110, kEmpty, Value);
                        }
                        else
                        {
                            Library->ReleaseTextureRecord(Texture);
                        }
                        if (Texture == nullptr)
                            continue;

                        TString<24> Key;
                        Key = Value;
                        FTexShape* const Shape = FindOrCreate(Key);
                        if (Shape->Texture == nullptr)
                        {
                            TexShapeSetTexture(Shape, Texture);
                            if (Shape->Texture != nullptr)
                                TexShapeSetRect(Shape, 0.f, 0.f, 0.f, 0.f, -1.f, -1.f, -1.f, -1.f);
                        }
                        else
                        {
                            Reader.log_link(kEmpty);
                            FDUMP(0x100001, "%s(%d) : *** ERROR: %s\nshape name (%s) already found",
                                  kShapeDbCpp, 0x17f, kEmpty, Value);
                        }
                    }
                    else if (Reader.is_value("dim"))
                    {
                        if (CurrentTexture == nullptr)
                        {
                            Reader.log_link(kEmpty);
                            FDUMP(0x100001,
                                  "%s(%d) : *** ERROR: %s\n'dim' field must be preceeded by "
                                  "'texture_name' field",
                                  kShapeDbCpp, 0x186, kEmpty);
                        }
                        else if (CurrentShape == nullptr)
                        {
                            Reader.log_link(kEmpty);
                            FDUMP(0x100001,
                                  "%s(%d) : *** ERROR: %s\n'dim' field must be preceeded by "
                                  "'shape_name' field",
                                  kShapeDbCpp, 0x18b, kEmpty);
                        }
                        else
                        {
                            // the first four are required, the last four read 0.0
                            // (the field) when empty.
                            float Values[8];
                            for (unsigned Index = 0; Index < 4; ++Index)
                                Values[Index] = Reader.get_value_float(Index);
                            for (unsigned Index = 4; Index < 8; ++Index)
                            {
                                Values[Index] =
                                    Reader.is_value_empty(Index) ? 0.f : Reader.get_value_float(Index);
                            }
                            TexShapeSetRect(CurrentShape, Values[0], Values[1], Values[2], Values[3],
                                            Values[4], Values[5], Values[6], Values[7]);
                            CurrentShape = nullptr;
                        }
                    }
                    else
                    {
                        // the message says [Shape] even inside [Texture].
                        char Message[0x200];
                        sprintf(Message, "unknown [Shape] parameter %s", Reader.get_name_ptr());
                        Reader.log_link(Message);
                        FDUMP(0x100002, "%s(%d) : *** WARNING: %s", kShapeDbCpp, 0x1a2, Message);
                    }
                }
            }
            else if (Reader.is_header("Shape"))
            {
                bExpectTexture = true;
                TString<260> ShapeName; // ESP+0x24c
                ShapeName.Length = 0;
                ShapeName.Data[0] = '\0';
                float X = 0.f;
                float Y = 0.f;
                float W = 0.f;
                float H = 0.f;
                float UpperLeftU = -1.f;
                float UpperLeftV = -1.f;
                float LowerRightU = -1.f;
                float LowerRightV = -1.f;

                while (Reader.read_value())
                {
                    if (Reader.is_value("name") || Reader.is_value("shape_name"))
                    {
                        char const* const Value = Reader.get_value_string(0);
                        if (Value == nullptr)
                        {
                            ShapeName.Length = 0;
                            ShapeName.Data[0] = '\0';
                        }
                        else
                        {
                            ShapeName.Length = CopyBoundedString(ShapeName.Data, 0x104, Value);
                        }
                    }
                    else if (Reader.is_value("x"))
                    {
                        X = Reader.get_value_float(0);
                    }
                    else if (Reader.is_value("y"))
                    {
                        Y = Reader.get_value_float(0);
                    }
                    else if (Reader.is_value("w"))
                    {
                        W = Reader.get_value_float(0);
                    }
                    else if (Reader.is_value("h"))
                    {
                        H = Reader.get_value_float(0);
                    }
                    else if (Reader.is_value("upper_left"))
                    {
                        UpperLeftU = Reader.get_value_float(0);
                        UpperLeftV = Reader.get_value_float(1);
                    }
                    else if (Reader.is_value("lower_right"))
                    {
                        // / 004266b2: both components + 1.0 (the field), stored to
                        // the lower-right pair ([ESP+0x2c] / [ESP+0x40]).
                        LowerRightU = Reader.get_value_float(0) + 1.f;
                        LowerRightV = Reader.get_value_float(1) + 1.f;
                    }
                    else if (Reader.is_value("dim"))
                    {
                        X = Reader.get_value_float(0);
                        Y = Reader.get_value_float(1);
                        W = Reader.get_value_float(2);
                        H = Reader.get_value_float(3);
                        UpperLeftU = Reader.get_value_float(4);
                        UpperLeftV = Reader.get_value_float(5);
                        LowerRightU = Reader.get_value_float(6);
                        LowerRightV = Reader.get_value_float(7);
                    }
                    else
                    {
                        Reader.log_link(kEmpty);
                        FDUMP(0x100002, "%s(%d) : *** WARNING: %s\nunknown shape parameter",
                              kShapeDbCpp, 0x1cf, kEmpty);
                    }
                }

                if (ShapeName.Data[0] == '\0')
                {
                    Reader.log_link(kEmpty);
                    FDUMP(0x100001, "%s(%d) : *** ERROR: %s\nno shape name was specified",
                          kShapeDbCpp, 0x1d4, kEmpty);
                }
                else if (CurrentTexture == nullptr)
                {
                    Reader.log_link(kEmpty);
                    FDUMP(0x100001, "%s(%d) : *** ERROR: %s", kShapeDbCpp, 0x1da, "bad texture id");
                }
                else
                {
                    // key via the routine, a {record 0, Field04 -1} value, map insert
                    // (the routine + the routine) -- the same find-or-insert as the routine.
                    TString<24> Key;
                    Key = ShapeName.Data;
                    FTexShape* const Shape = FindOrCreate(Key);
                    if (Shape->Texture != nullptr)
                    {
                        Reader.log_link(kEmpty);
                        FDUMP(0x100001, "%s(%d) : *** ERROR: %s\nshape name (%s) already found",
                              kShapeDbCpp, 0x1e0, kEmpty, ShapeName.Data);
                    }
                    else
                    {
                        // the record goes in directly and takes its reference through
                        // slot +0x34, then a full-texture rect, then the section's own rect.
                        Shape->Texture = CurrentTexture;
                        Library->CreatePlaybackGroup(CurrentTexture, nullptr);
                        if (Shape->Texture != nullptr)
                            TexShapeSetRect(Shape, 0.f, 0.f, 0.f, 0.f, -1.f, -1.f, -1.f, -1.f);
                        TexShapeSetRect(Shape, X, Y, W, H, UpperLeftU, UpperLeftV, LowerRightU,
                                        LowerRightV);
                    }
                }
            }
        }
    }

    // every group this call loaded is garbage-collected, the list emptied, slot 4
    // restored; the result is the map size.
    for (std::vector<int>::iterator It = LoadedGroups.begin(); It != LoadedGroups.end(); ++It)
        Library->GarbageCollect(*It);
    LoadedGroups.clear();
    Library->SetGenericSlot(4, SavedSlot4);
    return static_cast<int>(Count());
}

// ---------------------------------------------------------------- in-place storage

static_assert(sizeof(FShapeDB) <= kShapeDbStorageBytes,
              "FShapeDB must fit the 0x18 bytes baseline's owners reserve for it");

FShapeDB* ShapeDBConstructAt(void* Storage)
{
    return new (Storage) FShapeDB();
}

void ShapeDBDestructAt(void* Storage)
{
    static_cast<FShapeDB*>(Storage)->~FShapeDB();
}

FShapeDB* ShapeDBAt(void* Storage)
{
    return static_cast<FShapeDB*>(Storage);
}

// -------------------------------------------------------------------- the routine

void AddEffectShapesFile(char const* Path)
{
    G_EffectShapeFiles.push_back(StringAlloc(Path, true));
}

// -------------------------------------------------------------------- the routine

void ClearEffectShapesFiles()
{
    // `the field = the field`: the vector's end is set back to its begin. The queued
    // StringAlloc'd paths are not freed, and the capacity stays.
    G_EffectShapeFiles.clear();
}

// -------------------------------------------------------------------- the routine

int WarmUpEffectShapes()
{
    int Result = 0;
    for (std::vector<char const*>::iterator It = G_EffectShapeFiles.begin();
         It != G_EffectShapeFiles.end(); ++It)
    {
        Result = G_EffectShapeDB.Load(*It, nullptr);
    }

    // with an FxRuntime and no factory yet (the field), the routine
    // builds the FLFxMeshFactory; on its true result the pointer (null when its allocation
    // failed) goes to IFxRuntime slot +0x24 BindMeshFactory.
    if (NeuronRuntime::FxRuntime != nullptr && G_FLFxMeshFactory == nullptr &&
        FLFxMeshFactoryCreate(&G_FLFxMeshFactory))
    {
        NeuronRuntime::FxRuntime->BindMeshFactory(G_FLFxMeshFactory);
    }
    return Result;
}
