// Oracle suite and data dump of work package P1.7 (mesh data), Deformable2
// half: the skeleton, level fractions and mesh tables of .dfm files. The
// Rust test crates/neuron-core/tests/oracle_mesh_data.rs (dfm_files,
// data_dump_dfm) writes the same lines. The RendComp half and the shared
// helpers are in oracle_mesh_data.cpp.
//
// Deformable2.dll exports nothing but DllMain, so Deformable2.cpp and
// EyeMaterial.cpp are compiled into this file. Two of their names are also
// defined by RendComp (compiled into oracle_mesh_data.cpp) and are renamed
// here: G_Engine and FAggDesc18; DllMain is renamed as well.
//
// The dump calls the reference's LoadSkeleton (the bone walk) and
// LoadMeshTables (face groups, geometry, UV-deform rows, the vertex remap
// and the per-bone vertex lists) as ConnectInstanceParts and
// LoadDeformMesh do. What they hand to the engine and the material library
// goes to FOracleEngine and FOracleMaterialLibrary: the engine records the
// archetype names it is given (the bone names, which the skeleton does not
// keep) and the library finds every material, without an object. Around
// them the dump repeats what ConnectInstanceParts does itself: the
// MultiLevel / Fractions read and the Mesh* walk, here over every Mesh*
// entry rather than up to a part's level limit. The bound bone count of the
// LOD holder, which the engine decides, is the skeleton's bone count. The
// skin-pair count, which the mesh does not keep, is measured beside it from
// the Bone_id_chain leaf (its size / 4, as LoadGeometry computes it).

#include "oracle_support.h"

#define DllMain Deformable2DllMain
#define G_Engine G_DeformableEngine
#define FAggDesc18 FDeformableAggDesc18
#include "../Deformable2/Deformable2.cpp"
#include "../Deformable2/EyeMaterial.cpp"
#undef DllMain

using namespace oracle;

// oracle_mesh_data.cpp.
IFileSystem* OracleMeshOpenUtf(const std::string& File);
std::vector<std::string> OracleMeshUtfFiles(const std::vector<std::string>& Files, const char* const* Extensions,
                                            int ExtensionCount);

unsigned int OracleDfmFvfStride(unsigned long Fvf)
{
    return (unsigned int)ComputeFvfStride(Fvf);
}

namespace
{
    // The search path GetSearchPath hands out; LoadSkeleton releases it.
    class FOracleComponent : public INeuronComponent
    {
    public:
        GENRESULT NEURONCOMPONENTS_API QueryInterface(const char*, void** Out) override
        {
            *Out = nullptr;
            return GR_INTERFACE_UNSUPPORTED;
        }
        unsigned long NEURONCOMPONENTS_API AddRef() override { return 1; }
        unsigned long NEURONCOMPONENTS_API Release() override { return 1; }
    };

    // The engine LoadSkeleton talks to: every archetype handle it asks for
    // is made (numbered from 1) and its name recorded.
    class FOracleEngine : public IEngine
    {
    public:
        std::vector<std::string> Names;
        FOracleComponent SearchPath;

        GENRESULT NEURONCOMPONENTS_API QueryInterface(const char*, void** Out) override
        {
            *Out = nullptr;
            return GR_INTERFACE_UNSUPPORTED;
        }
        unsigned long NEURONCOMPONENTS_API AddRef() override { return 1; }
        unsigned long NEURONCOMPONENTS_API Release() override { return 1; }
        GENRESULT NEURONCOMPONENTS_API LoadEngineComponents(INeuronComponent* SystemContainer) override { return 0; }
        unsigned long NEURONCOMPONENTS_API EnumerateEngineComponents(int(__cdecl* Callback)(const char*, void*)) override { return 0; }
        GENRESULT NEURONCOMPONENTS_API NotifyEngineComponents(void* Arg) override { return 0; }
        GENRESULT NEURONCOMPONENTS_API SetSearchPath(const char* Path) override { return 0; }
        GENRESULT NEURONCOMPONENTS_API QuerySearchPath(char* Dest, unsigned long MaxChars) override { return 0; }
        GENRESULT NEURONCOMPONENTS_API SetFilesystem(INeuronComponent* NewPath) override { return 0; }
        GENRESULT NEURONCOMPONENTS_API GetSearchPath(void** Out) override
        {
            *Out = &SearchPath;
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API CreateFileSystem(const char* Path, void** Out, void* Factory) override { return 0; }
        void* NEURONCOMPONENTS_API CreateCompoundArchetype(FEngineName* Name) override { return nullptr; }
        void* NEURONCOMPONENTS_API CreateArchetypeHandle(FEngineName* Name, void*) override
        {
            Names.push_back(Name->Text != nullptr ? Name->Text : "");
            return (void*)(intptr_t)Names.size();
        }
        void* NEURONCOMPONENTS_API DuplicateArchetype(void* ArchIn, FEngineName* Name) override { return nullptr; }
        unsigned long NEURONCOMPONENTS_API AddRefHandle(void* Handle) override { return 0; }
        unsigned long NEURONCOMPONENTS_API ReleaseHandle(void* Handle) override { return 0; }
        FEngineName* NEURONCOMPONENTS_API GetArchetypeName(FEngineName* Out, void* Handle) override { return nullptr; }
        void* NEURONCOMPONENTS_API FindArchetype(FEngineName* Name) override { return nullptr; }
        GENRESULT NEURONCOMPONENTS_API QueryEngineComponent(int Id, void* Name, void** Out) override { return 0; }
        unsigned long NEURONCOMPONENTS_API IsCompoundArchetype(void* Handle) override { return 0; }
        GENRESULT NEURONCOMPONENTS_API EnumerateArchetypeConstraints(void* Handle, int(__cdecl* Callback)(void*, void*, void*, void*), void* Context) override { return 0; }
        GENRESULT NEURONCOMPONENTS_API EnumerateArchetypeChildren(void* Handle, int(__cdecl* Callback)(void*, void*, int, void*), void* Context) override { return 0; }
        void* NEURONCOMPONENTS_API CreateInstanceFromName(FEngineName* Name, void* FileSystem, void* Transform) override { return nullptr; }
        void* NEURONCOMPONENTS_API CreateInstance(void* Handle, void* Transform) override { return nullptr; }
        GENRESULT NEURONCOMPONENTS_API DestroyInstance(void* Instance) override { return 0; }
        void* NEURONCOMPONENTS_API GetInstanceArchetype(void* Instance) override { return nullptr; }
        FEngineName* NEURONCOMPONENTS_API GetInstanceName(FEngineName* Out, void* Instance) override { return nullptr; }
        GENRESULT NEURONCOMPONENTS_API UpdateInstance(void* Instance, unsigned long Flags, void* Arg) override { return 0; }
        GENRESULT NEURONCOMPONENTS_API SetInstanceTransform(void* Instance, void* Transform) override { return 0; }
        GENRESULT NEURONCOMPONENTS_API GetInstanceTransformObject(void* Instance, void** Out) override { return 0; }
        GENRESULT NEURONCOMPONENTS_API QueryInstance(void* Instance, const char* Iid, void** Out) override { return 0; }
        GENRESULT NEURONCOMPONENTS_API SetInstanceUserData(void* Instance, void* Value) override { return 0; }
        void* NEURONCOMPONENTS_API GetInstanceUserData(void* Instance) override { return nullptr; }
        GENRESULT NEURONCOMPONENTS_API SetInstanceExtent(void* Instance, unsigned long Flags, float Extent, const float* Vec) override { return 0; }
        GENRESULT NEURONCOMPONENTS_API GetInstanceExtent(void* Instance, unsigned long Flags, float* OutExtent, float* OutVec) override { return 0; }
        GENRESULT NEURONCOMPONENTS_API SetInstancePosition(void* Instance, void* Position) override { return 0; }
        float* NEURONCOMPONENTS_API GetInstancePosition(void* Instance) override { return nullptr; }
        GENRESULT NEURONCOMPONENTS_API SetInstanceOrientation(void* Instance, void* Orient) override { return 0; }
        float* NEURONCOMPONENTS_API GetInstanceOrientation(void* Instance) override { return nullptr; }
        GENRESULT NEURONCOMPONENTS_API SetInstanceMatrix(void* Instance, void* Matrix) override { return 0; }
        float* NEURONCOMPONENTS_API GetInstanceTransform(void* Instance) override { return nullptr; }
        GENRESULT NEURONCOMPONENTS_API NoOp41(void* A, void* B) override { return 0; }
        float* NEURONCOMPONENTS_API GetIdentityMatrix(void* Unused) override { return nullptr; }
        GENRESULT NEURONCOMPONENTS_API NoOp43(void* A, void* B) override { return 0; }
        float* NEURONCOMPONENTS_API GetIdentityMatrix2(void* Unused) override { return nullptr; }
        GENRESULT NEURONCOMPONENTS_API SetTransformUser(void* Instance, unsigned long User) override { return 0; }
        unsigned long NEURONCOMPONENTS_API GetTransformUser(void* Instance) override { return 0; }
        unsigned long NEURONCOMPONENTS_API CreateJoint(void* Parent, void* Child, const unsigned long* Payload) override { return 0; }
        unsigned long NEURONCOMPONENTS_API DestroyJoint(void* Parent, void* Child) override { return 0; }
        unsigned long* NEURONCOMPONENTS_API GetInstanceRelative(void* Instance) override { return nullptr; }
        unsigned long NEURONCOMPONENTS_API SetJointParams(void* Instance, unsigned long Index, const float* Values) override { return 0; }
        unsigned long NEURONCOMPONENTS_API GetJointParams(void* Instance, unsigned long Index, float* Out) override { return 0; }
        void* NEURONCOMPONENTS_API FindMappedInstance(void* Instance) override { return nullptr; }
        void* NEURONCOMPONENTS_API GetInstanceParent(void* Instance) override { return nullptr; }
        void* NEURONCOMPONENTS_API GetRelatedInstance(void* From, unsigned long Flags, void* Cursor) override { return nullptr; }
        void* NEURONCOMPONENTS_API GetInstanceRoot(void* Instance) override { return nullptr; }
        void* NEURONCOMPONENTS_API FindArchetypeChild(void* Handle, void* ChildHandle) override { return nullptr; }
        unsigned long NEURONCOMPONENTS_API RejectOperation(void* A, void* B) override { return 0; }
    };

    // The material library LoadFaceGroups asks: every material is found
    // (the port takes them as found), none is an object.
    class FOracleMaterialLibrary : public IMaterialLibrary
    {
    public:
        GENRESULT NEURONCOMPONENTS_API QueryInterface(const char*, void** Out) override
        {
            *Out = nullptr;
            return GR_INTERFACE_UNSUPPORTED;
        }
        unsigned long NEURONCOMPONENTS_API AddRef() override { return 1; }
        unsigned long NEURONCOMPONENTS_API Release() override { return 1; }
        GENRESULT NEURONCOMPONENTS_API AddMapRule(int Key, const char* Pattern, const char* TypeName) override { return 0; }
        GENRESULT NEURONCOMPONENTS_API FindMapRule(int Key, const char* Pattern, void* Unused, char* OutType) override { return 0; }
        GENRESULT NEURONCOMPONENTS_API RemapType(const char* Name, const char* Type, void* Unused, char* OutType) override { return 0; }
        GENRESULT NEURONCOMPONENTS_API RemoveMapRule(int Key, const char* Pattern) override { return 0; }
        GENRESULT NEURONCOMPONENTS_API AllocateGroup(unsigned long* OutId) override { return 0; }
        GENRESULT NEURONCOMPONENTS_API LoadFromFileSystem(void* FileSystem, void* Extra) override { return 0; }
        GENRESULT NEURONCOMPONENTS_API UnsupportedOperation(void* Arg1, void* Arg2) override { return 0; }
        GENRESULT NEURONCOMPONENTS_API DeleteGroup(int GroupId) override { return 0; }
        GENRESULT NEURONCOMPONENTS_API ApplyVerifyGroup(int GroupId, void* Context, void* Extra) override { return 0; }
        GENRESULT NEURONCOMPONENTS_API ResolveTexturesGroup(int GroupId, float Delta) override { return 0; }
        GENRESULT NEURONCOMPONENTS_API SetGroupName(int GroupId, const char* Name) override { return 0; }
        GENRESULT NEURONCOMPONENTS_API GetGroupName(int GroupId, char* Buffer, unsigned long Size) override { return 0; }
        GENRESULT NEURONCOMPONENTS_API FindOrCreate(const char* Name, const char* Type, INeuronComponent* AggregateOuter, IMaterial** Out) override { return 0; }
        GENRESULT NEURONCOMPONENTS_API FindMaterialByCrc(void*, IMaterial** Out) override
        {
            *Out = nullptr;
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API AddMaterial(int GroupId, void* Record, IMaterial* Material) override { return 0; }
        GENRESULT NEURONCOMPONENTS_API RemoveMaterial(unsigned long GroupId, void* Record) override { return 0; }
        GENRESULT NEURONCOMPONENTS_API GetGroupCount(unsigned long* OutCount) override { return 0; }
        GENRESULT NEURONCOMPONENTS_API EnumGroup(unsigned long Index, unsigned long* OutId) override { return 0; }
        GENRESULT NEURONCOMPONENTS_API GetGroupMaterialCount(unsigned long GroupId, int* OutCount) override { return 0; }
        GENRESULT NEURONCOMPONENTS_API EnumGroupMaterial(unsigned long GroupId, unsigned long Index, IMaterial** Out) override { return 0; }
        GENRESULT NEURONCOMPONENTS_API SetGlobalRecord(unsigned long Index, const unsigned long* Value) override { return 0; }
        GENRESULT NEURONCOMPONENTS_API SetGlobalRecordValue(unsigned long Index, const unsigned long* Value) override { return 0; }
        GENRESULT NEURONCOMPONENTS_API GetGlobalRecordValue(unsigned long Index, unsigned long* OutValue) override { return 0; }
        FMaterialLibraryRecord* NEURONCOMPONENTS_API CreateAfterMap(const char* Name, const char* Type, unsigned char bRemap, INeuronComponent* AggregateOuter) override { return nullptr; }
    };

    FOracleEngine GEngine;
    FOracleMaterialLibrary GLibrary;

    std::string Bits(float Value)
    {
        return Format("%08x", FloatBits(Value));
    }

    void AppendU32(Bytes& Out, unsigned long Value)
    {
        for (int Shift = 0; Shift < 32; Shift += 8)
            Out.push_back((unsigned char)(Value >> Shift));
    }

    void AppendU16(Bytes& Out, unsigned short Value)
    {
        Out.push_back((unsigned char)Value);
        Out.push_back((unsigned char)(Value >> 8));
    }

    // Bone_id_chain's size, read beside the reference: Geometry is opened
    // and left again, as LoadGeometry does.
    unsigned long BoneIdChainSize(IFileSystem* Fs)
    {
        if (Fs->OpenNode("Geometry") == 0)
            return 0;
        FFileSystemDesc Desc;
        FillFileDesc(&Desc, "Bone_id_chain");
        unsigned long Size = 0;
        const int Handle = Fs->OpenByDesc(&Desc);
        if (Handle != -1)
        {
            Size = Fs->GetSize((unsigned long)Handle, 0);
            Fs->CloseFile((unsigned long)Handle);
        }
        Fs->OpenNode(G_ParentNode);
        return Size;
    }

    void MeshLines(const FMeshArchetype* Mesh, const FLodHolder& Holder, unsigned long PairCount,
                   std::vector<std::string>& Lines)
    {
        const int Count = Mesh->FaceGroups != nullptr ? ((const int*)Mesh->FaceGroups)[-1] : 0;
        Lines.push_back(Format("{\"fgcount\":\"%x\",\"groups\":\"%d\"}", Count, Mesh->FaceGroupCount));
        for (int Index = 0; Index < Mesh->FaceGroupCount; ++Index)
        {
            const FFaceGroup& G = Mesh->FaceGroups[Index];
            Lines.push_back(Format("{\"fg\":\"%d\",\"kind\":\"%lx\",\"crc\":\"%08lx\",\"n\":\"%lx\",\"idx\":\"%s\",\"min\":\"%lx\",\"max\":\"%lx\"}",
                                   Index, G.Kind, G.MaterialCrc, G.IndexCount,
                                   Fnv64(G.Indices, (size_t)G.IndexCount * 2).c_str(), G.IndexMin, G.IndexMax));
        }
        const size_t Points = (size_t)(unsigned int)Mesh->PointIndexCount;
        const size_t Vertices = (size_t)(unsigned int)Mesh->VertexCount;
        const std::string Uvi =
            Mesh->UvIndices == nullptr ? std::string("-") : Fnv64(Mesh->UvIndices, Points * 4);
        Bytes Skin;
        for (size_t Vertex = 0; Vertex < Vertices; ++Vertex)
        {
            AppendU32(Skin, (unsigned long)Mesh->SkinTable[Vertex].BoneCount);
            AppendU32(Skin, (unsigned long)(Mesh->SkinTable[Vertex].Pairs - Mesh->SkinPairs));
        }
        Lines.push_back(Format("{\"points\":\"%x\",\"pi\":\"%s\",\"vertices\":\"%x\",\"v\":\"%s\",\"uvi\":\"%s\",\"uvs\":\"%x\",\"uv\":\"%s\",\"pairs\":\"%lx\",\"p\":\"%s\",\"skin\":\"%s\"}",
                               (unsigned int)Points, Fnv64(Mesh->PointIndices, Points * 4).c_str(),
                               (unsigned int)Vertices, Fnv64(Mesh->Vertices, Vertices * 0x18).c_str(), Uvi.c_str(),
                               (unsigned int)Mesh->UvCount,
                               Fnv64(Mesh->UvData, (size_t)(unsigned int)Mesh->UvCount * 8).c_str(), PairCount,
                               Fnv64(Mesh->SkinPairs, (size_t)PairCount * 8).c_str(),
                               Fnv64(Skin.data(), Skin.size()).c_str()));
        Bytes Remap;
        for (const FVertexRemapList* List = Mesh->RemapStart; List != Mesh->RemapFinish; ++List)
        {
            const size_t Used = List->Start != nullptr ? (size_t)(List->Finish - List->Start) : 0;
            AppendU32(Remap, (unsigned long)Used);
            for (size_t Index = 0; Index < Used; ++Index)
                AppendU32(Remap, List->Start[Index]);
        }
        Bytes BoneLists;
        for (int Bone = 0; Bone < Holder.BindCursor; ++Bone)
        {
            const FBoneVertexList& List = Mesh->BoneVerts[Bone];
            const size_t Used = List.Start != nullptr ? (size_t)(List.Finish - List.Start) : 0;
            AppendU32(BoneLists, (unsigned long)Used);
            for (size_t Index = 0; Index < Used; ++Index)
                AppendU16(BoneLists, List.Start[Index]);
        }
        Lines.push_back(Format("{\"uvd\":\"%x\",\"u\":\"%s\",\"remap\":\"%s\",\"bonelists\":\"%s\"}",
                               (unsigned int)Mesh->UvDeformCount,
                               Fnv64(Mesh->UvDeform, (size_t)(unsigned int)Mesh->UvDeformCount * 0x24).c_str(),
                               Fnv64(Remap.data(), Remap.size()).c_str(),
                               Fnv64(BoneLists.data(), BoneLists.size()).c_str()));
    }

    std::vector<std::string> DfmFileLines(const std::string& File)
    {
        G_Engine = &GEngine;
        G_MaterialLibrary = &GLibrary;
        std::vector<std::string> Lines;
        IFileSystem* Root = OracleMeshOpenUtf(File);
        Lines.push_back(Format("{\"file\":\"%s\",\"open\":\"%d\"}", File.c_str(), Root != nullptr ? 1 : 0));
        if (Root == nullptr)
            return Lines;

        // ConnectInstanceParts: the skeleton, then the holder.
        GEngine.Names.clear();
        FSkeleton* Skel = ConstructSkeleton();
        LoadSkeleton(Skel, "skel_oracle", Root);
        Lines.push_back(Format("{\"bones\":\"%d\"}", Skel->BoneCount));
        for (int Bone = 0; Bone < Skel->BoneCount; ++Bone)
        {
            std::string Matrix;
            for (int Index = 0; Index < 12; ++Index)
                Matrix += Bits(Skel->Bones[Bone].BoneToRoot[Index]);
            const std::string& Name = GEngine.Names[(size_t)Bone];
            Lines.push_back(Format("{\"bone\":\"%s\",\"m\":\"%s\"}", Hex(Name.data(), Name.size()).c_str(),
                                   Matrix.c_str()));
        }
        FLodHolder Holder;
        Holder.Skeleton = nullptr;
        Holder.BoneSlots = nullptr;
        Holder.BindCursor = Skel->BoneCount;
        Holder.RefCount = 1;

        // ConnectInstanceParts: MultiLevel and Fractions.
        float Fractions[8] = {0, 0, 0, 0, 0, 0, 0, 0};
        int LodCap = 0;
        const bool bMulti = Root->OpenNode("MultiLevel") != 0;
        if (bMulti)
        {
            unsigned int Wanted = 8;
            FFileSystemDesc FractionDesc;
            FillFileDesc(&FractionDesc, "Fractions");
            const int Handle = Root->OpenByDesc(&FractionDesc);
            if (Handle != -1)
            {
                const unsigned int Available = Root->GetSize((unsigned long)Handle, 0) >> 2;
                if (Available < Wanted)
                    Wanted = Available;
                unsigned long Got = 0;
                Root->Read((unsigned long)Handle, Fractions, Wanted * 4, &Got, 0);
                LodCap = (int)Wanted;
                Root->CloseFile((unsigned long)Handle);
            }
        }
        else
        {
            LodCap = 1;
            Fractions[0] = 1.0f;
        }
        std::string FractionText;
        for (int Index = 0; Index < LodCap; ++Index)
            FractionText += (Index == 0 ? "" : ",") + Bits(Fractions[Index]);
        Lines.push_back(Format("{\"multilevel\":\"%d\",\"lodcap\":\"%d\",\"fractions\":\"%s\"}", bMulti ? 1 : 0, LodCap,
                               FractionText.c_str()));

        // ConnectInstanceParts and LoadDeformMesh: every Mesh* entry.
        WIN32_FIND_DATAA FindData;
        const int Finder = FsFindFirst(Root, "Mesh*", &FindData);
        if (Finder != -1)
        {
            do
            {
                FFileSystemDesc MeshDesc;
                FillFileDesc(&MeshDesc, FindData.cFileName);
                MeshDesc.Field30 = (unsigned long)Finder;
                IFileSystem* MeshFs = nullptr;
                const std::string Name = FindData.cFileName;
                const std::string HexName = Hex(Name.data(), Name.size());
                if (Root->CreateInstance((AGGDESC*)(&MeshDesc), (void**)(&MeshFs)) < 0)
                {
                    if (MeshFs != nullptr)
                        MeshFs->Release();
                    Lines.push_back(Format("{\"mesh\":\"%s\",\"child\":\"0\",\"ok\":\"0\"}", HexName.c_str()));
                    continue;
                }
                FMeshArchetype* Mesh = new FMeshArchetype(Name.c_str());
                const bool bLoaded = LoadMeshTables(Mesh, &Holder, MeshFs);
                Lines.push_back(Format("{\"mesh\":\"%s\",\"child\":\"1\",\"ok\":\"%d\"}", HexName.c_str(),
                                       bLoaded ? 1 : 0));
                if (bLoaded)
                    MeshLines(Mesh, Holder, BoneIdChainSize(MeshFs) >> 2, Lines);
                delete Mesh;
                MeshFs->Release();
            } while (FsFindNext(Root, Finder, &FindData) != 0);
            FsFindClose(Root, Finder);
        }
        delete Skel;
        Root->Release();
        return Lines;
    }
}

// dfm_files.jsonl, called by RunMeshDataSuites.
void RunDfmFilesSuite()
{
    Suite Out("dfm_files.jsonl");
    for (const std::string& File : InputLines("mesh_data/dfm.list"))
    {
        for (const std::string& Line : DfmFileLines(File))
            Out.Line(Line);
    }
}

// Kind "dfm", called by RunMeshDataDump.
void RunDfmDataDump(const std::vector<std::string>& Files)
{
    const char* const Extensions[1] = {"dfm"};
    const std::vector<std::string> Dfm = OracleMeshUtfFiles(Files, Extensions, 1);
    {
        Suite List(GOutputDir + "\\dfm\\files.txt");
        for (const std::string& File : Dfm)
            List.Line(File);
    }
    for (const std::string& File : Dfm)
    {
        Suite Out(DataOutputFile("dfm", File));
        for (const std::string& Line : DfmFileLines(File))
            Out.Line(Line);
    }
    printf("dumped %u dfm files\n", (unsigned int)Dfm.size());
}
