// P4.7 (second increment): the deformable parts suite (deformable_parts.jsonl, input
// deformable_parts.list and the archives in deformable/ from make_deformable_fixtures.py). The Rust
// side is crates/neuron-client/tests/support/deformable.rs (parts_lines) with the same contract.
//
// oracle_mesh_data_dfm.cpp includes this file after oracle_deformable.cpp, whose cases, fakes and
// operations it shares; oracle_deformable.cpp hands it the operations it does not know
// (RunPartsOp):
//   links <root> <inst:arch,...> [| <root> <inst:arch,...>]...: the bone links of the next
//     skeleton instances, one set per instance
//   create <file:lods:flags:limit>,...: the reference's Create (DeformableCreate,
//     ConnectInstanceParts, LoadSkeleton, LoadDeformMesh) over the part files, each opened
//     through its level 0 file system (the archive factory below), then the descriptions written
//     back and each part's holder, skeleton and buffers
//   meshes: the mesh cache by name CRC
//   eyelib <sub> ...: the EyeMaterial made by its registered factory through the component
//     manager and driven through IMaterial: map (BindEyeMaterials on the recording library),
//     refuse <type>, new <m>, info <m>, name <m> <hex>, load <m> <archive> <node>, verify, resolve,
//     flags, get <m> <i>, set <m> <i> <type> <value>, save <m> <archive> <node>, bind <m> <bone>
//     <record> | -, submit <m>, release <m>
//
// The archives are read by a small in-memory IFileSystem over the UTF bytes (FPartsFs): the calls
// Deformable2 and the EyeMaterial make (OpenNode, OpenByDesc, GetSize, Read, CloseFile,
// CreateInstance of a node or a leaf, FindFirst with UTF's wildcard match, FindNext, CreateMapping, MapFileView,
// UnmapFileView, and Write into a leaf opened for writing) with UTF's rules for well-formed
// archives (names without case, the archive's order). Each operation that opens an archive reads
// it anew, so a save is not seen by a later load. Every Write writes
//   fswrite "name","data" (data hex)
// P1.7 checks the loaders on ReadFile's own UTF code.

namespace deformable_oracle
{
    // --- the archives ---------------------------------------------------------------------------

    struct FPartsNode
    {
        std::string Name;
        bool bDir = false;
        std::vector<unsigned char> Data;
        FPartsNode* Parent = nullptr;
        std::vector<FPartsNode*> Children;
    };

    int LowerAscii(int Ch) { return Ch >= 'A' && Ch <= 'Z' ? Ch + 32 : Ch; }

    bool SamePartsName(const std::string& A, const char* B)
    {
        size_t Index = 0;
        for (; Index < A.size() && B[Index] != 0; ++Index)
            if (LowerAscii((unsigned char)A[Index]) != LowerAscii((unsigned char)B[Index]))
                return false;
        return Index == A.size() && B[Index] == 0;
    }

    // '*' any run, '?' one character, without case.
    bool WildMatch(const char* Pattern, const char* Name)
    {
        if (*Pattern == 0)
            return *Name == 0;
        if (*Pattern == '*')
            return WildMatch(Pattern + 1, Name) || (*Name != 0 && WildMatch(Pattern, Name + 1));
        if (*Name == 0)
            return false;
        if (*Pattern != '?' && LowerAscii((unsigned char)*Pattern) != LowerAscii((unsigned char)*Name))
            return false;
        return WildMatch(Pattern + 1, Name + 1);
    }

    FPartsNode* FindPartsChild(FPartsNode* Node, const char* Name, bool bDir)
    {
        for (FPartsNode* Child : Node->Children)
            if (Child->bDir == bDir && SamePartsName(Child->Name, Name))
                return Child;
        return nullptr;
    }

    // UTF: header +8 tree offset, +0x18 names, +0x24 data; 0x2C-byte entries {next, name, flags,
    // ..., +0x10 child or data offset, +0x1C size}. Leaves are copied.
    void PartsChildren(FPartsNode* Parent, unsigned long First, const unsigned char* B, unsigned long Tree,
                       unsigned long Names, unsigned long Data)
    {
        unsigned long At = First;
        while (true)
        {
            unsigned long Next, Name, Flags, Off, Size;
            memcpy(&Next, B + Tree + At, 4);
            memcpy(&Name, B + Tree + At + 4, 4);
            memcpy(&Flags, B + Tree + At + 8, 4);
            memcpy(&Off, B + Tree + At + 0x10, 4);
            memcpy(&Size, B + Tree + At + 0x1C, 4);
            FPartsNode* Node = new FPartsNode();
            Node->Name = (const char*)(B + Names + Name);
            Node->Parent = Parent;
            Node->bDir = (Flags & 0x10) != 0;
            if (Node->bDir)
            {
                if (Off != 0)
                    PartsChildren(Node, Off, B, Tree, Names, Data);
            }
            else
                Node->Data.assign(B + Data + Off, B + Data + Off + Size);
            Parent->Children.push_back(Node);
            if (Next == 0)
                return;
            At = Next;
        }
    }

    FPartsNode* ParsePartsUtf(const Bytes& File)
    {
        const unsigned char* B = File.data();
        unsigned long Tree, Names, Data, First;
        memcpy(&Tree, B + 8, 4);
        memcpy(&Names, B + 0x18, 4);
        memcpy(&Data, B + 0x24, 4);
        FPartsNode* Root = new FPartsNode();
        Root->bDir = true;
        memcpy(&First, B + Tree + 0x10, 4);
        if (First != 0)
            PartsChildren(Root, First, B, Tree, Names, Data);
        return Root;
    }

    // An archive by its path in the fixture folder (backslashes as the reference writes them).
    FPartsNode* ReadPartsArchive(const char* Path)
    {
        std::string Local = Path;
        for (char& Ch : Local)
            if (Ch == '\\')
                Ch = '/';
        Bytes Data;
        if (!ReadWholeFile(Local, Data))
        {
            fprintf(stderr, "deformable_parts: cannot read %s\n", Local.c_str());
            GFailed = true;
            return nullptr;
        }
        return ParsePartsUtf(Data);
    }

    class FPartsFs final : public IFileSystem
    {
    public:
        FPartsNode* Current;
        unsigned long Refs = 1;
        struct FOpen
        {
            FPartsNode* Node;
            unsigned long Position;
        };
        std::vector<FOpen> Open;
        std::vector<FPartsNode*> Found;
        size_t FoundAt = 0;

        explicit FPartsFs(FPartsNode* Node) : Current(Node) {}

        GENRESULT NEURONCOMPONENTS_API QueryInterface(const char*, void** Out) override
        {
            *Out = nullptr;
            return GR_INTERFACE_UNSUPPORTED;
        }
        unsigned long NEURONCOMPONENTS_API AddRef() override { return ++Refs; }
        unsigned long NEURONCOMPONENTS_API Release() override
        {
            if (--Refs == 0)
            {
                delete this;
                return 0;
            }
            return Refs;
        }
        GENRESULT NEURONCOMPONENTS_API CreateInstance(AGGDESC* Arg, void** Out) override
        {
            // UTF makes a file system of a leaf as well, rooted at it (nothing below).
            FFileSystemDesc* Desc = (FFileSystemDesc*)Arg;
            FPartsNode* Child = FindPartsChild(Current, Desc->Path, true);
            if (Child == nullptr)
                Child = FindPartsChild(Current, Desc->Path, false);
            if (Child == nullptr)
            {
                *Out = nullptr;
                return GR_GENERIC;
            }
            *Out = (IFileSystem*)new FPartsFs(Child);
            return GR_OK;
        }
        int NEURONCOMPONENTS_API OpenNode(const char* Name) override
        {
            if (strcmp(Name, "..") == 0)
            {
                if (Current->Parent == nullptr)
                    return 0;
                Current = Current->Parent;
                return 1;
            }
            FPartsNode* Child = FindPartsChild(Current, Name, true);
            if (Child == nullptr)
                return 0;
            Current = Child;
            return 1;
        }
        int NEURONCOMPONENTS_API OpenByDesc(FFileSystemDesc* Desc) override
        {
            FPartsNode* Leaf = FindPartsChild(Current, Desc->Path, false);
            if ((Desc->Access & 0x40000000u) != 0)
            {
                if (Leaf == nullptr)
                {
                    Leaf = new FPartsNode();
                    Leaf->Name = Desc->Path;
                    Leaf->Parent = Current;
                    Current->Children.push_back(Leaf);
                }
                Leaf->Data.clear();
            }
            if (Leaf == nullptr)
                return -1;
            Open.push_back({Leaf, 0});
            return (int)Open.size();
        }
        int NEURONCOMPONENTS_API CloseFile(unsigned long) override { return 1; }
        unsigned long NEURONCOMPONENTS_API GetSize(unsigned long Handle, unsigned long*) override
        {
            return (unsigned long)Open[Handle - 1].Node->Data.size();
        }
        int NEURONCOMPONENTS_API Read(unsigned long Handle, void* Buffer, unsigned long Size, unsigned long* Got,
                                      unsigned long) override
        {
            FOpen& F = Open[Handle - 1];
            const unsigned long Left = (unsigned long)F.Node->Data.size() - F.Position;
            const unsigned long N = Size < Left ? Size : Left;
            if (N != 0)
                memcpy(Buffer, F.Node->Data.data() + F.Position, N);
            F.Position += N;
            *Got = N;
            return N != 0 ? 1 : 0;
        }
        int NEURONCOMPONENTS_API Write(unsigned long Handle, const void* Buffer, unsigned long Size, unsigned long* BytesWritten,
                                       const void*) override
        {
            FOpen& F = Open[Handle - 1];
            const unsigned char* Raw = (const unsigned char*)Buffer;
            F.Node->Data.insert(F.Node->Data.end(), Raw, Raw + Size);
            F.Position += Size;
            *BytesWritten = Size;
            G.Event(Format("\"ev\":\"fswrite\",\"name\":\"%s\",\"data\":\"", F.Node->Name.c_str()) + Hex(Raw, Size) + "\"");
            return 1;
        }
        int NEURONCOMPONENTS_API CreateMapping(unsigned long Handle, unsigned long, unsigned long, unsigned long, unsigned long,
                                               unsigned long) override
        {
            return (int)(0x10000 + Handle);
        }
        int NEURONCOMPONENTS_API MapFileView(unsigned long Mapping, unsigned long, unsigned long, unsigned long, unsigned long) override
        {
            FPartsNode* Node = Open[Mapping - 0x10000 - 1].Node;
            return (int)(intptr_t)Node->Data.data();
        }
        int NEURONCOMPONENTS_API UnmapFileView(unsigned long) override { return 1; }
        int Fill(WIN32_FIND_DATAA* Data)
        {
            memset(Data, 0, sizeof(*Data));
            FPartsNode* Node = Found[FoundAt];
            Data->dwFileAttributes = Node->bDir ? 0x10 : 0x80;
            strncpy(Data->cFileName, Node->Name.c_str(), sizeof(Data->cFileName) - 1);
            return 1;
        }
        int NEURONCOMPONENTS_API FindFirst(const char* Pattern, WIN32_FIND_DATAA* Data) override
        {
            Found.clear();
            for (FPartsNode* Child : Current->Children)
                if (WildMatch(Pattern, Child->Name.c_str()))
                    Found.push_back(Child);
            FoundAt = 0;
            if (Found.empty())
                return -1;
            Fill(Data);
            return 0;
        }
        int NEURONCOMPONENTS_API FindNext(unsigned long, WIN32_FIND_DATAA* Data) override
        {
            if (++FoundAt >= Found.size())
                return 0;
            return Fill(Data);
        }
        int NEURONCOMPONENTS_API FindClose(unsigned long) override { return 1; }
        int NEURONCOMPONENTS_API GetCurrentPath(unsigned long, char* Out) override
        {
            Out[0] = 0;
            return 1;
        }
        int NEURONCOMPONENTS_API ResolvePath(char* Dest, const char*, unsigned long) override
        {
            Dest[0] = 0;
            return 1;
        }
        // The slots Deformable2 and the EyeMaterial do not call.
        int NEURONCOMPONENTS_API RejectOperation1C(unsigned long A1, unsigned long A2, unsigned long A3, unsigned long A4) override { return GR_GENERIC; }
        int NEURONCOMPONENTS_API SeekFile(unsigned long Handle, unsigned long OffLow, unsigned long* OffHighOut, unsigned long Origin) override { return GR_GENERIC; }
        int NEURONCOMPONENTS_API SetEndOfFile(unsigned long A1) override { return GR_GENERIC; }
        int NEURONCOMPONENTS_API ReturnOne2C(unsigned long A1, unsigned long A2, unsigned long A3, unsigned long A4, unsigned long A5) override { return GR_GENERIC; }
        int NEURONCOMPONENTS_API ReturnOne30(unsigned long A1, unsigned long A2, unsigned long A3, unsigned long A4, unsigned long A5) override { return GR_GENERIC; }
        int NEURONCOMPONENTS_API GetFileTimes(unsigned long Handle, FILETIME* CreationOut, FILETIME* AccessOut, FILETIME* WriteOut) override { return GR_GENERIC; }
        int NEURONCOMPONENTS_API RejectOperation38(unsigned long A1, unsigned long A2, unsigned long A3, unsigned long A4) override { return GR_GENERIC; }
        int NEURONCOMPONENTS_API RejectOperation54(const char* Name, unsigned long Extra) override { return GR_GENERIC; }
        int NEURONCOMPONENTS_API RejectOperation58(unsigned long A1) override { return GR_GENERIC; }
        int NEURONCOMPONENTS_API RejectOperation64(unsigned long A1) override { return GR_GENERIC; }
        int NEURONCOMPONENTS_API RejectOperation68(unsigned long A1, unsigned long A2, unsigned long A3) override { return GR_GENERIC; }
        int NEURONCOMPONENTS_API RejectOperation6C(unsigned long A1, unsigned long A2) override { return GR_GENERIC; }
        int NEURONCOMPONENTS_API GetPathAttributes(const char* Path) override { return GR_GENERIC; }
        int NEURONCOMPONENTS_API RejectOperation74(unsigned long A1, unsigned long A2) override { return GR_GENERIC; }
        int NEURONCOMPONENTS_API GetFileSystemError() override { return GR_GENERIC; }
        int NEURONCOMPONENTS_API GetFilePosition(unsigned long Handle, unsigned long* HighOut) override { return GR_GENERIC; }
        void NEURONCOMPONENTS_API GetFileSysPath(char* Out, unsigned long MaxChars) override { }
        int NEURONCOMPONENTS_API GetAccessFlags() override { return GR_GENERIC; }
        int NEURONCOMPONENTS_API GetParentFileSystem(IFileSystem** Out) override { return GR_GENERIC; }
        int NEURONCOMPONENTS_API FailOperation90(unsigned long A1, unsigned long A2) override { return GR_GENERIC; }
        int NEURONCOMPONENTS_API FailOperation94(unsigned long A1, unsigned long A2) override { return GR_GENERIC; }
        int NEURONCOMPONENTS_API FailOperation98(unsigned long A1, unsigned long A2, unsigned long A3, unsigned long A4, unsigned long A5) override { return GR_GENERIC; }
        int NEURONCOMPONENTS_API FailOperation9C(unsigned long A1, unsigned long A2, unsigned long A3, unsigned long A4, unsigned long A5) override { return GR_GENERIC; }
        int NEURONCOMPONENTS_API InvokeLockedCallback(unsigned long A1, unsigned long A2, unsigned long A3) override { return GR_GENERIC; }
    };

    // The level 0 file system of every part description: the part file as a UTF archive.
    class FPartsFactory final : public IComponentFactory
    {
    public:
        GENRESULT NEURONCOMPONENTS_API QueryInterface(const char*, void** Out) override
        {
            *Out = nullptr;
            return GR_INTERFACE_UNSUPPORTED;
        }
        unsigned long NEURONCOMPONENTS_API AddRef() override { return 1; }
        unsigned long NEURONCOMPONENTS_API Release() override { return 1; }
        GENRESULT NEURONCOMPONENTS_API CreateInstance(AGGDESC* Arg, void** Out) override
        {
            FPartsNode* Root = ReadPartsArchive(((FFileSystemDesc*)Arg)->Path);
            if (Root == nullptr)
            {
                *Out = nullptr;
                return GR_GENERIC;
            }
            *Out = (IFileSystem*)new FPartsFs(Root);
            return GR_OK;
        }
    };

    FPartsFactory GPartsFactory;

    // --- the material library -------------------------------------------------------------------

    // FindMaterialByCrc answers the case's materials by name; AddMapRule records the rule.
    class FDefLibrary final : public IMaterialLibrary
    {
    public:
        GENRESULT NEURONCOMPONENTS_API QueryInterface(const char*, void** Out) override
        {
            *Out = nullptr;
            return GR_INTERFACE_UNSUPPORTED;
        }
        unsigned long NEURONCOMPONENTS_API AddRef() override { return 1; }
        unsigned long NEURONCOMPONENTS_API Release() override { return 1; }
        GENRESULT NEURONCOMPONENTS_API AddMapRule(int Key, const char* Pattern, const char* TypeName) override
        {
            G.Event(Format("\"ev\":\"maprule\",\"key\":\"%d\",\"pattern\":\"%s\",\"type\":\"%s\"", Key, Pattern, TypeName));
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API FindMaterialByCrc(void* Query, IMaterial** Out) override
        {
            const FEngineName* Name = (const FEngineName*)Query;
            const std::string Text = Name->Text != nullptr ? Name->Text : "";
            std::map<std::string, IMaterial*>::iterator Found = G.Materials.find(Text);
            const bool bFound = Found != G.Materials.end();
            G.Event(Format("\"ev\":\"findmat\",\"name\":\"%s\",\"r\":\"%d\"", Text.c_str(), bFound ? 0 : -1));
            *Out = bFound ? Found->second : nullptr;
            if (!bFound)
                return GR_GENERIC;
            (*Out)->AddRef();
            return GR_OK;
        }
        // The slots Deformable2 does not call.
        GENRESULT NEURONCOMPONENTS_API FindMapRule(int Key, const char* Pattern, void* Unused, char* OutType) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API RemapType(const char* Name, const char* Type, void* Unused, char* OutType) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API RemoveMapRule(int Key, const char* Pattern) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API AllocateGroup(unsigned long* OutId) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API LoadFromFileSystem(void* FileSystem, void* Extra) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API UnsupportedOperation(void* Arg1, void* Arg2) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API DeleteGroup(int GroupId) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API ApplyVerifyGroup(int GroupId, void* Context, void* Extra) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API ResolveTexturesGroup(int GroupId, float Delta) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API SetGroupName(int GroupId, const char* Name) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetGroupName(int GroupId, char* Buffer, unsigned long Size) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API FindOrCreate(const char* Name, const char* Type, INeuronComponent* AggregateOuter, IMaterial** Out) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API AddMaterial(int GroupId, void* Record, IMaterial* Material) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API RemoveMaterial(unsigned long GroupId, void* Record) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetGroupCount(unsigned long* OutCount) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API EnumGroup(unsigned long Index, unsigned long* OutId) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetGroupMaterialCount(unsigned long GroupId, int* OutCount) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API EnumGroupMaterial(unsigned long GroupId, unsigned long Index, IMaterial** Out) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API SetGlobalRecord(unsigned long Index, const unsigned long* Value) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API SetGlobalRecordValue(unsigned long Index, const unsigned long* Value) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetGlobalRecordValue(unsigned long Index, unsigned long* OutValue) override { return GR_GENERIC; }
        FMaterialLibraryRecord* NEURONCOMPONENTS_API CreateAfterMap(const char* Name, const char* Type, unsigned char bRemap, INeuronComponent* AggregateOuter) override { return nullptr; }
    };

    FDefLibrary GDefLibrary;

    // --- the operations -------------------------------------------------------------------------

    std::string PartText(FDeformPart* Part)
    {
        FLodHolder* Holder = Part->Lod;
        if (Holder == nullptr)
            return "nolod";
        std::string Bones;
        std::string Slots;
        for (int Index = 0; Index < Holder->Skeleton->BoneCount; ++Index)
        {
            const FBoneSlot& Bone = Holder->Skeleton->Bones[Index];
            Bones += Format(Index ? "|%d:" : "%d:", Bone.Parent) + Fs(Bone.BoneToRoot, 12);
            Slots += Format(Index ? "|%ld:%x" : "%ld:%x", Id(Holder->BoneSlots[Index].Instance), Holder->BoneSlots[Index].Flags);
        }
        return Format("bound=%d,lod=%d,meshes=%d,vcap=%d,icap=%d,flags=%x,skel=%ld,bones=", Holder->BindCursor,
                      Part->LodIndex, Part->MeshCount, Part->Vb.Capacity, Part->Ib.Capacity, Part->Flags,
                      Id(Holder->Skeleton->EngineHandle)) +
               Bones + ",slots=" + Slots;
    }

    std::string MeshText(unsigned long Crc, FDeformMesh* Mesh)
    {
        FMeshArchetype* Arch = dynamic_cast<FMeshArchetype*>(Mesh);
        if (Arch == nullptr)
        {
            FVmeshWrapper* Wrapper = (FVmeshWrapper*)Mesh;
            return Format("%lx:%d:vmesh:%ld", Crc, Mesh->RefCount, Id(Wrapper->EngineHandle));
        }
        std::string Groups;
        for (int Index = 0; Index < Arch->FaceGroupCount; ++Index)
        {
            const FFaceGroup& Group = Arch->FaceGroups[Index];
            Groups += Format(Index ? "|%d/%lu/%ld" : "%d/%lu/%ld", Group.Material != nullptr ? 1 : 0, Group.IndexCount,
                             (long)Group.UvBoneIndex);
        }
        std::string Lists;
        const int ListCount = ((const int*)Arch->BoneVerts)[-1];
        for (int Index = 0; Index < ListCount; ++Index)
            Lists += Format(Index ? ",%d" : "%d", (int)(Arch->BoneVerts[Index].Finish - Arch->BoneVerts[Index].Start));
        return Format("%lx:%d:dfm:%d:%d:%u:%lx:", Crc, Mesh->RefCount, Arch->GetIndexCount(), Arch->GetVertexCapacity(),
                      Arch->Flag70, Arch->ParseTag) +
               Groups + ":" + Lists;
    }

    FPartsNode* PartsNodeAt(const std::string& Archive, const std::string& Node)
    {
        FPartsNode* Root = ReadPartsArchive(Archive.c_str());
        if (Root == nullptr)
            return nullptr;
        FPartsNode* Library = FindPartsChild(Root, "Material library", true);
        return Library == nullptr ? nullptr : FindPartsChild(Library, Node.c_str(), true);
    }

    std::string PartsCellText(const FMaterialParamValue& Cell)
    {
        if (Cell.Type == 5 || Cell.Type == 7)
            return Format("%d:", Cell.Type) + (Cell.Value[0] == 0 ? std::string("-") : HexC((const char*)Ptr((long)Cell.Value[0])));
        return Format("%d:%lx,%lx,%lx,%lx", Cell.Type, Cell.Value[0], Cell.Type >= 3 && Cell.Type <= 4 ? Cell.Value[1] : 0,
                      Cell.Type >= 3 && Cell.Type <= 4 ? Cell.Value[2] : 0, Cell.Type == 4 ? Cell.Value[3] : 0);
    }

    std::string EyeValues(IMaterial* Eye)
    {
        std::string Out;
        for (unsigned long Index = 0; Index < 10; ++Index)
        {
            FMaterialParamValue Cell;
            Eye->GetParameterValue(Index, &Cell);
            Out += (Index ? ";" : "") + PartsCellText(Cell);
        }
        return Out;
    }

    bool GEyeFactoryRegistered = false;

    void RunEyeLib(FDefCase& Case, const std::vector<std::string>& T)
    {
        const std::string& Sub = T[1];
        if (Sub == "map")
        {
            // BindEyeMaterials: RegisterEyeMaterialFactory, then the rule.
            if (!GEyeFactoryRegistered)
            {
                BindEyeMaterials(&GDefLibrary);
                GEyeFactoryRegistered = true;
            }
            else
                GDefLibrary.AddMapRule(0, G_EyeMaterialPattern, G_EyeMaterialName);
            Op("\"op\":\"eyelib\",\"sub\":\"map\"");
            return;
        }
        if (Sub == "refuse" || Sub == "new")
        {
            const std::string TypeName = Sub == "new" ? std::string(G_EyeMaterialName) : T[2];
            FAGGDESC14 Desc;
            memset(&Desc, 0, sizeof(Desc));
            Desc.Size = sizeof(Desc);
            Desc.NameAt4 = TypeName.c_str();
            IMaterial* Out = nullptr;
            const GENRESULT R = NEURONCOMPONENTS_Acquire()->CreateInstance((AGGDESC*)&Desc, (void**)&Out);
            if (Sub == "refuse")
            {
                Op(Format("\"op\":\"eyelib\",\"sub\":\"refuse\",\"r\":\"%d\"", R));
                return;
            }
            const GENRESULT Init = Out != nullptr ? Out->Initialize(&GSystem) : GR_GENERIC;
            if (Out != nullptr)
                G.Materials[T[2]] = Out;
            Op(Format("\"op\":\"eyelib\",\"sub\":\"new\",\"r\":\"%d\",\"init\":\"%d\"", R, Init));
            return;
        }
        IMaterial* Eye = G.Materials[T[2]];
        if (Sub == "info")
        {
            char Class[0x40];
            memset(Class, 0, sizeof(Class));
            const GENRESULT R1 = Eye->CopyClassName(Class, sizeof(Class));
            int Passes = -9;
            const GENRESULT R2 = Eye->GetPassCount(&Passes);
            unsigned long Count = 0;
            const GENRESULT R3 = Eye->GetParameterCount(&Count);
            std::string Keys;
            for (unsigned long Index = 0; Index <= Count; ++Index)
            {
                FMaterialParamDesc Desc;
                const GENRESULT R = Eye->GetParameterDescriptor(Index, &Desc);
                Keys += Format(Index ? ";%d:" : "%d:", R) + (R >= 0 && Desc.Key != nullptr ? HexC(Desc.Key) : std::string("-"));
            }
            Op(Format("\"op\":\"eyelib\",\"sub\":\"info\",\"class\":\"%d:%s\",\"passes\":\"%d:%d\",\"count\":\"%d:%lu\",\"keys\":\"",
                      R1, Class, R2, Passes, R3, Count) +
               Keys + "\"");
        }
        else if (Sub == "name")
        {
            Eye->SetName((const char*)Unhex(T[3]).data());
            char Out[0x80];
            memset(Out, 0, sizeof(Out));
            const GENRESULT R = Eye->GetName(Out, sizeof(Out));
            Op(Format("\"op\":\"eyelib\",\"sub\":\"name\",\"r\":\"%d\",\"v\":\"", R) + HexC(Out) + "\"");
        }
        else if (Sub == "load" || Sub == "save")
        {
            FPartsNode* Node = PartsNodeAt(T[3], T[4]);
            if (Node == nullptr)
            {
                fprintf(stderr, "deformable_parts: no material %s in %s\n", T[4].c_str(), T[3].c_str());
                GFailed = true;
                return;
            }
            FPartsFs* Fs = new FPartsFs(Node);
            const GENRESULT R = Sub == "load" ? Eye->LoadFromFileSystem(Fs) : Eye->SaveProperties(Fs);
            Fs->Release();
            Op(Format("\"op\":\"eyelib\",\"sub\":\"%s\",\"r\":\"%d\",\"v\":\"", Sub.c_str(), R) + EyeValues(Eye) + "\"");
        }
        else if (Sub == "verify")
        {
            const GENRESULT R = Eye->ApplyVerify(nullptr, nullptr);
            Op(Format("\"op\":\"eyelib\",\"sub\":\"verify\",\"r\":\"%d\"", R));
        }
        else if (Sub == "resolve")
        {
            const GENRESULT R = Eye->ResolveTextures(0.0f);
            Op(Format("\"op\":\"eyelib\",\"sub\":\"resolve\",\"r\":\"%d\"", R));
        }
        else if (Sub == "flags")
        {
            unsigned long Out = 0;
            const GENRESULT R = Eye->GetRenderFlags(&Out);
            Op(Format("\"op\":\"eyelib\",\"sub\":\"flags\",\"r\":\"%d\",\"v\":\"%lx\"", R, Out));
        }
        else if (Sub == "get")
        {
            FMaterialParamValue Cell;
            const GENRESULT R = Eye->GetParameterValue((unsigned long)Dec(T[3]), &Cell);
            Op(Format("\"op\":\"eyelib\",\"sub\":\"get\",\"r\":\"%d\",\"cell\":\"", R) + PartsCellText(Cell) + "\"");
        }
        else if (Sub == "set")
        {
            FMaterialParamValue Cell((int)Dec(T[4]));
            if (Cell.Type == 5 || Cell.Type == 7)
                Cell.DupString((const char*)Unhex(T[5]).data());
            else
            {
                const std::vector<std::string> W = Words(T[5], ",");
                for (size_t I = 0; I < W.size() && I < 4; ++I)
                    Cell.Value[I] = HexWord(W[I]);
            }
            const GENRESULT R = Eye->SetParameterValue((unsigned long)Dec(T[3]), &Cell);
            Op(Format("\"op\":\"eyelib\",\"sub\":\"set\",\"r\":\"%d\",\"v\":\"", R) + EyeValues(Eye) + "\"");
        }
        else if (Sub == "bind")
        {
            FMaterialSubmitContext Ctx;
            memset(&Ctx, 0, sizeof(Ctx));
            Ctx.World = (void*)Identity12;
            Ctx.View = (void*)Identity12;
            if (T[3] == "-")
            {
                Eye->Bind(&Ctx);
                Eye->Teardown(&Ctx);
                Op("\"op\":\"eyelib\",\"sub\":\"bind\"");
                return;
            }
            FBoneRuntime* Bone = Case.BoneAt(T[3]);
            const std::vector<float> R = HexFloats(T[4]);
            FUvDeformRecord Record;
            Record.BoneId = (int)R[0];
            Record.VertexCount = (int)R[1];
            Record.PlaneDistance = R[2];
            Record.BoneXToU = R[3];
            Record.BoneYToV = R[4];
            Record.MinDu = R[5];
            Record.MaxDu = R[6];
            Record.MinDv = R[7];
            Record.MaxDv = R[8];
            FUvDeformBinding Binding;
            Binding.Record = &Record;
            Binding.Bone = Bone;
            Binding.EyeUvTransformCount = 0;
            Ctx.Inst = Bone->Instance;
            Ctx.Field20 = &Binding;
            Eye->Bind(&Ctx);
            Eye->Teardown(&Ctx);
            Op(Format("\"op\":\"eyelib\",\"sub\":\"bind\",\"n\":\"%lu\"", Binding.EyeUvTransformCount));
        }
        else if (Sub == "submit")
        {
            FVbListing Listing;
            Listing.VertexBuffer = Ptr(0x123);
            Listing.Fvf = 0x112;
            Listing.BaseVertex = 5;
            FIbScratch Scratch;
            Scratch.IndexBuffer = Ptr(0x456);
            Scratch.Bias = 7;
            FMaterialSubmitContext Ctx;
            memset(&Ctx, 0, sizeof(Ctx));
            Ctx.World = (void*)Identity12;
            Ctx.View = (void*)Identity12;
            Ctx.VbListing = &Listing;
            Ctx.IbScratch = &Scratch;
            const GENRESULT R = Eye->Submit(&Ctx, 4, 2, 9, 3, 12, 0x100);
            Op(Format("\"op\":\"eyelib\",\"sub\":\"submit\",\"r\":\"%d\"", R));
        }
        else if (Sub == "release")
        {
            Eye->Release();
            G.Materials.erase(T[2]);
            Op("\"op\":\"eyelib\",\"sub\":\"release\"");
        }
        else
        {
            fprintf(stderr, "deformable_parts: unknown eyelib operation %s\n", Sub.c_str());
            GFailed = true;
        }
    }

    bool RunPartsOp(FDefCase& Case, const std::vector<std::string>& T)
    {
        const std::string& K = T[0];
        if (K == "links")
        {
            size_t At = 1;
            while (At + 1 < T.size())
            {
                std::vector<std::pair<long, long>> Links;
                for (const std::string& Pair : Words(T[At + 1], ","))
                {
                    const std::vector<std::string> P = Words(Pair, ":");
                    Links.push_back(std::make_pair(Dec(P[0]), Dec(P[1])));
                }
                G.LinkQueue.push_back(std::make_pair(Dec(T[At]), Links));
                At += 3;
            }
        }
        else if (K == "create")
        {
            const std::vector<std::string> Specs = Words(T[1], ",");
            const size_t Count = Specs.size();
            std::vector<std::string> Names(Count);
            std::vector<FDeformLodDesc> Lods(Count * 8);
            std::vector<FDeformPartDesc> Descs(Count);
            memset(Lods.data(), 0, Lods.size() * sizeof(FDeformLodDesc));
            memset(Descs.data(), 0, Descs.size() * sizeof(FDeformPartDesc));
            for (size_t Index = 0; Index < Count; ++Index)
            {
                const std::vector<std::string> Fields = Words(Specs[Index], ":");
                Names[Index] = Fields[0];
                FDeformLodDesc* L = &Lods[Index * 8];
                L[0].FileSystem = &GPartsFactory;
                L[0].Name = Names[Index].c_str();
                Descs[Index].LodCount = (int)Dec(Fields[1]);
                Descs[Index].Lods = L;
                Descs[Index].Path = nullptr;
                Descs[Index].Flags = (unsigned long)Dec(Fields[2]);
                Descs[Index].LodLimit = (unsigned long)Dec(Fields[3]);
            }
            int Parts[1 + sizeof(void*) / sizeof(int)];
            Parts[0] = (int)Count;
            FDeformPartDesc* First = Descs.data();
            memcpy(&Parts[1], &First, sizeof(First));
            G_DeformRate = 0.0f;
            void* Out = nullptr;
            const GENRESULT R = GComponent->Create(&Out, Parts, nullptr, Ptr(0), nullptr);
            G.Instance = (FDeformInstance*)Out;
            G.Parts.clear();
            std::string DescText;
            std::string PartsText;
            for (size_t Index = 0; Index < Count; ++Index)
            {
                std::string Fractions;
                for (int L = 0; L < Descs[Index].LodCount; ++L)
                    Fractions += (L ? "," : "") + F(Lods[Index * 8 + (size_t)L].Fraction);
                DescText += Format(Index ? ";%d/" : "%d/", Descs[Index].LodCount) + (Fractions.empty() ? std::string("-") : Fractions);
                if (G.Instance != nullptr)
                {
                    FDeformPart* Part = &G.Instance->PartArray[Index];
                    G.Parts.push_back(Part);
                    PartsText += (Index ? ";" : "") + PartText(Part);
                }
            }
            if (G.Instance != nullptr && G.Instance->PartCount > 0)
                G.Holder = G.Instance->PartArray[0].Lod;
            Op(Format("\"op\":\"create\",\"r\":\"%d\",\"descs\":\"", R) + DescText + "\",\"parts\":\"" + PartsText + "\"");
        }
        else if (K == "meshes")
        {
            std::string Text;
            for (const std::pair<const unsigned long, FDeformMesh*>& Entry : G_MeshCache)
                Text += (Text.empty() ? "" : ";") + MeshText(Entry.first, Entry.second);
            Op("\"op\":\"meshes\",\"v\":\"" + Text + "\"");
        }
        else if (K == "eyelib")
            RunEyeLib(Case, T);
        else
            return false;
        return true;
    }

    // The suite (deformable_parts.jsonl): the deformable suite's cases with the material library
    // answering the face groups' materials.
    void RunPartsSuite()
    {
        Suite Out("deformable_parts.jsonl");
        GListLibrary = &GDefLibrary;
        RunDeformableList(Out, "deformable_parts.list");
        GListLibrary = nullptr;
    }
}
