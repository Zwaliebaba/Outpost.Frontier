// Oracle suites and data dump of work package P1.7 (mesh data): the mesh
// nodes RendComp.dll reads (VMeshLibrary, VMeshPart, VMeshWire, MultiLevel,
// Mesh, Sphere). The Rust test crates/neuron-core/tests/oracle_mesh_data.rs
// writes the same lines; the inputs come from
// crates/neuron-core/tests/fixtures/oracle/make_mesh_data_fixtures.py. The
// Deformable2 half (.dfm files) is oracle_mesh_data_dfm.cpp.
//
// RendComp.dll exports nothing but DllMain, so its seven source files are
// compiled into this one, as oracle_vfs.cpp does with ReadFile.cpp. Every
// object below is the reference's own: FVMeshLibrary::Load, VMeshDataLoad
// and VMeshGroupEnsureIb for a library; FRenderManager::MultiLevel and
// WalkPlugins with the VMesh, TriMesh and SphereMesh plugins (in the
// client's [RenderManager] order, TriMesh with the client's tristrips = 0)
// for a render archetype; FVMeshWire::CreateArchetype for a wireframe.
//
// Nothing here has a device. The material library, the pipeline and the
// engine stay null, which the loaders allow; the vertex and index buffer
// globals point at FOracleBuffers, which keeps index buffers in memory so
// that VMeshGroupEnsureIb fills one exactly as it fills a device buffer.
// The library is unloaded before the render plugins claim anything, so a
// part's material hook-up finds no mesh data and stops there, as it does
// for a part loaded before its library.
//
// The file systems come from ReadFile's factories through the component
// manager; RunVfsSuites and RunVfsDataDump register them and run first.
//
// What the reference does not keep is measured beside it: a VWireData
// leaf's size (for the "short" and "trailing" fields) is read through the
// same file system. Lengths of the TriMesh corner index arrays, the UV
// animation frames and the edge angles are not kept either; the dump hashes
// as many elements as the reference uses, which the inputs always hold.

#include "oracle_support.h"

#include <algorithm>

#define DllMain RendCompDllMain
#include "../RendComp/RendComp.cpp"
#include "../RendComp/RenderManager.cpp"
#include "../RendComp/Plugins.cpp"
#include "../RendComp/TriMesh.cpp"
#include "../RendComp/VMeshLibrary.cpp"
#include "../RendComp/VMeshWire.cpp"
#include "../RendComp/VMeshPart.cpp"
#undef DllMain

using namespace oracle;

// oracle_mesh_data_dfm.cpp.
unsigned int OracleDfmFvfStride(unsigned long Fvf);
void RunDfmFilesSuite();
void RunDfmDataDump(const std::vector<std::string>& Files);

// A UTF archive opened through the component manager, or null; also used by
// oracle_mesh_data_dfm.cpp.
IFileSystem* OracleMeshOpenUtf(const std::string& File)
{
    if (FileMagic(File) != "UTF ")
        return nullptr;
    FFileSystemDesc Desc;
    memset(&Desc, 0, sizeof(Desc));
    Desc.Size = 0x34;
    Desc.ComponentName = "FileSystem";
    Desc.Implementation = "UTF";
    Desc.Path = File.c_str();
    Desc.Access = 0x80000000;
    Desc.ShareMode = 1;
    Desc.Creation = 3;
    Desc.Flags = 0x08000080;
    Desc.Field30 = 0xFFFFFFFF;
    IFileSystem* Fs = nullptr;
    if (NEURONCOMPONENTS_Acquire()->CreateInstance((AGGDESC*)&Desc, (void**)&Fs) != GR_OK)
        return nullptr;
    return Fs;
}

namespace
{
    const char* const NameLibrary = "VMeshLibrary";
    const char* const NameWire = "VMeshWire";
    const char* const NameRender[4] = {"VMeshPart", "MultiLevel", "Mesh", "Sphere"};
    const int WalkDepth = 64;

    // The channel table in the reference's order.
    const unsigned int ChannelIds[6] = {1, 0x800, 8, 2, 4, 0x10};

    std::string Bits(float Value)
    {
        return Format("%08x", FloatBits(Value));
    }

    std::string BitsList(const float* Values, size_t Count)
    {
        std::string Out;
        for (size_t Index = 0; Index < Count; ++Index)
        {
            if (Index != 0)
                Out += ",";
            Out += Bits(Values[Index]);
        }
        return Out;
    }

    // --- the buffers the groups and the plugins reach --------------------------

    // IRPVertexBuffer and IRPIndexBuffer with no device: index buffers are
    // arrays in memory, vertex buffers are never made (DestroyVb only).
    class FOracleIndexBuffer : public IRPIndexBuffer
    {
    public:
        GENRESULT NEURONCOMPONENTS_API QueryInterface(const char*, void** Out) override
        {
            *Out = nullptr;
            return GR_INTERFACE_UNSUPPORTED;
        }
        unsigned long NEURONCOMPONENTS_API AddRef() override { return 1; }
        unsigned long NEURONCOMPONENTS_API Release() override { return 1; }
        GENRESULT NEURONCOMPONENTS_API CreateIb(unsigned int Count, unsigned int** Out, unsigned int) override
        {
            std::vector<unsigned short>* Buffer = new std::vector<unsigned short>(Count + 1, 0);
            *Out = (unsigned int*)Buffer;
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API DestroyIb(void** Handle) override
        {
            delete (std::vector<unsigned short>*)*Handle;
            *Handle = nullptr;
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API ResizeIb(void*, unsigned int) override { return -1; }
        GENRESULT NEURONCOMPONENTS_API CopyIndices(void*, unsigned int*, const unsigned short*, unsigned int) override
        {
            return -1;
        }
        GENRESULT NEURONCOMPONENTS_API LockIb(void* Handle, unsigned int* Offset, void** Out, unsigned int) override
        {
            *Offset = 0;
            *Out = ((std::vector<unsigned short>*)Handle)->data();
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API UnlockIb(void*) override { return GR_OK; }
        // A buffer that does not exist yet does not select.
        GENRESULT NEURONCOMPONENTS_API SelectIb(void* Handle, unsigned int, unsigned int, unsigned int) override
        {
            return Handle == nullptr ? -1 : GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API GetIbCapacity(void*, unsigned int* Out) override
        {
            *Out = 0;
            return -1;
        }
        GENRESULT NEURONCOMPONENTS_API IsIbValid(void* Handle) override { return Handle != nullptr ? 1 : 0; }
    };

    class FOracleVertexBuffer : public IRPVertexBuffer
    {
    public:
        GENRESULT NEURONCOMPONENTS_API QueryInterface(const char*, void** Out) override
        {
            *Out = nullptr;
            return GR_INTERFACE_UNSUPPORTED;
        }
        unsigned long NEURONCOMPONENTS_API AddRef() override { return 1; }
        unsigned long NEURONCOMPONENTS_API Release() override { return 1; }
        GENRESULT NEURONCOMPONENTS_API CreateVb(unsigned int, unsigned int, unsigned int** Out, unsigned int) override
        {
            *Out = nullptr;
            return -1;
        }
        GENRESULT NEURONCOMPONENTS_API DestroyVb(void** Handle) override
        {
            *Handle = nullptr;
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API ResizeVb(void*, unsigned int, unsigned int) override { return -1; }
        GENRESULT NEURONCOMPONENTS_API CopyVertices(void*, unsigned int*, const FRpVertexSrcDesc*, unsigned int,
                                                    unsigned int) override
        {
            return -1;
        }
        GENRESULT NEURONCOMPONENTS_API LockVb(void*, unsigned int*, void** Out, unsigned int) override
        {
            *Out = nullptr;
            return -1;
        }
        GENRESULT NEURONCOMPONENTS_API UnlockVb(void*) override { return GR_OK; }
        GENRESULT NEURONCOMPONENTS_API UnsupportedVertexBufferOperation(void*) override { return -1; }
        GENRESULT NEURONCOMPONENTS_API SelectVb(void*) override { return -1; }
        GENRESULT NEURONCOMPONENTS_API GetVbInfo(void*, unsigned int*, unsigned int*) override { return -1; }
        GENRESULT NEURONCOMPONENTS_API IsVbValid(void*) override { return 0; }
    };

    void InstallBuffers()
    {
        static FOracleIndexBuffer IndexBuffer;
        static FOracleVertexBuffer VertexBuffer;
        G_IndexBuffer = &IndexBuffer;
        G_VertexBuffer = &VertexBuffer;
    }

    // --- the render manager and its plugins ------------------------------------

    // A plugin in the render manager's list: forwards every call to the real
    // plugin and keeps the FDUMP count of each claim by its handle.
    class FCountingPlugin : public IRenderComponent
    {
    public:
        FCountingPlugin(IRenderComponent* InReal, const char* InName) : Real(InReal), Name(InName) {}

        IRenderComponent* Real;
        const char* Name;
        std::map<int, int> Dumps;

        GENRESULT NEURONCOMPONENTS_API QueryInterface(const char* Iid, void** Out) override
        {
            return Real->QueryInterface(Iid, Out);
        }
        unsigned long NEURONCOMPONENTS_API AddRef() override { return 1; }
        unsigned long NEURONCOMPONENTS_API Release() override { return 1; }
        int NEURONCOMPONENTS_API Claim(void* File) override
        {
            const int Before = GDumps;
            const int Handle = Real->Claim(File);
            if (Handle != -1)
                Dumps[Handle] = GDumps - Before;
            return Handle;
        }
        int NEURONCOMPONENTS_API CloneHandle(void* Handle) override { return Real->CloneHandle(Handle); }
        int NEURONCOMPONENTS_API DestroyHandle(void* Handle) override { return Real->DestroyHandle(Handle); }
        int NEURONCOMPONENTS_API QueryHandle(void* Handle, const char* Iid, void** Out) override
        {
            return Real->QueryHandle(Handle, Iid, Out);
        }
        int NEURONCOMPONENTS_API CreateMeshInstance(void* InstId, void* Handle) override
        {
            return Real->CreateMeshInstance(InstId, Handle);
        }
        int NEURONCOMPONENTS_API DestroyMeshInstance(void* InstId, void* Handle) override
        {
            return Real->DestroyMeshInstance(InstId, Handle);
        }
        int NEURONCOMPONENTS_API UpdateMeshInstance(void* InstId, void* Arg) override
        {
            return Real->UpdateMeshInstance(InstId, Arg);
        }
        int NEURONCOMPONENTS_API QueryMeshInstance(void* InstId, void* Handle, const char* Iid, void** Out) override
        {
            return Real->QueryMeshInstance(InstId, Handle, Iid, Out);
        }
        int NEURONCOMPONENTS_API UpdateMeshInstances(void* InstId) override { return Real->UpdateMeshInstances(InstId); }
        int NEURONCOMPONENTS_API QueryGeometry(void* Handle, int Mode, void* A, void* B, void* C, void* D,
                                               void* E) override
        {
            return Real->QueryGeometry(Handle, Mode, A, B, C, D, E);
        }
        int NEURONCOMPONENTS_API GetBounds(void* Handle, float Lod, void* Out) override
        {
            return Real->GetBounds(Handle, Lod, Out);
        }
        int NEURONCOMPONENTS_API GetCentroid(void* Handle, float Lod, void* Out) override
        {
            return Real->GetCentroid(Handle, Lod, Out);
        }
        int NEURONCOMPONENTS_API SplitByPlane(void* Handle, void* A, void* B, void* C, void* D, void* E,
                                              void* F) override
        {
            return Real->SplitByPlane(Handle, A, B, C, D, E, F);
        }
        int NEURONCOMPONENTS_API RenderInstance(ICamera* Camera, void* Handle, void* Inst, unsigned long Flags,
                                                void* Matrix, void* Extra) override
        {
            return Real->RenderInstance(Camera, Handle, Inst, Flags, Matrix, Extra);
        }
        int NEURONCOMPONENTS_API BindMeshInstance(void* Inst, void* Handle) override
        {
            return Real->BindMeshInstance(Inst, Handle);
        }
    };

    // A non-null pointer for the descriptor fields the plugin factories test.
    INeuronComponent* const Present = (INeuronComponent*)(intptr_t)1;

    // The render manager with the client's plugins, made once. The VMesh
    // plugin comes from its factory, the SphereMesh plugin from its create
    // function; the TriMesh plugin is made directly so that it does not read
    // [TriMesh] from whatever profile the harness has loaded, and gets the
    // client's tristrips = 0.
    FRenderManager* Manager()
    {
        static FRenderManager* Made = nullptr;
        if (Made != nullptr)
            return Made;
        Made = new FRenderManager();
        IRenderComponent* Plugins[3] = {nullptr, nullptr, nullptr};

        FNeuronComponentsFactory VMeshFactory(&VMeshPluginFactoryCreate);
        VMeshFactory.Name = G_NameVMesh;
        FRenderPluginDesc Desc;
        Desc.Size = 0x10;
        Desc.Name = G_NameVMesh;
        Desc.Parent = Present;
        Desc.Engine = (IEngine*)Present;
        VMeshPluginFactoryCreate(&VMeshFactory, (AGGDESC*)&Desc, (void**)&Plugins[0]);

        FTriMeshPlugin* TriMesh = new FTriMeshPlugin();
        TriMesh->Tristrips = 0;
        Plugins[1] = TriMesh;

        Desc.Name = G_NameSphereMesh;
        CreateSphereMeshPlugin(&Desc, (void**)&Plugins[2]);

        const char* const Names[3] = {"VMesh", "TriMesh", "SphereMesh"};
        for (int Index = 0; Index < 3; ++Index)
        {
            if (Plugins[Index] == nullptr)
            {
                fprintf(stderr, "mesh data: cannot make the %s plugin\n", Names[Index]);
                GFailed = true;
                continue;
            }
            FPluginNode* Node = new FPluginNode();
            Node->Plugin = new FCountingPlugin(Plugins[Index], Names[Index]);
            Node->Next = &Made->PluginSentinel;
            Node->Prev = Made->PluginSentinel.Prev;
            Made->PluginSentinel.Prev->Next = Node;
            Made->PluginSentinel.Prev = Node;
            Made->PluginCount += 1;
        }
        Made->bPluginsLoaded = 1;
        return Made;
    }

    FVMeshLibrary* Library()
    {
        static FVMeshLibrary* Made = nullptr;
        if (Made == nullptr)
        {
            Made = new FVMeshLibrary();
            Made->Inner.RefCount = 1;
            Made->Inner.Owner = Made;
            Made->Outer = &Made->Inner;
        }
        return Made;
    }

    FVMeshWire* Wire()
    {
        static FVMeshWire* Made = nullptr;
        if (Made == nullptr)
        {
            Made = new FVMeshWire();
            Made->Inner.RefCount = 1;
            Made->Inner.Owner = Made;
            Made->Outer = &Made->Inner;
        }
        return Made;
    }

    // --- mesh_fvf ---------------------------------------------------------------

    std::string ChannelsText(unsigned long Fvf)
    {
        std::string Out;
        for (int Index = 0; Index < 6; ++Index)
        {
            const unsigned int Id = ChannelIds[Index];
            if (Index != 0)
                Out += ",";
            if (VMeshChannelPresent(Fvf, Id))
                Out += Format("%x:%x:%x", Id, (unsigned int)VMeshChannelByteOffset(Fvf, Id),
                              (unsigned int)VMeshChannelElemBytes(Id));
            else
                Out += Format("%x:-", Id);
        }
        return Out;
    }

    void MeshFvf()
    {
        Suite Out("mesh_fvf.jsonl");
        const unsigned int Ids[8] = {1, 0x800, 8, 2, 4, 0x10, 0x20, 0};
        for (const std::string& Line : InputLines("mesh_data/fvf.list"))
        {
            const unsigned long Fvf = strtoul(Line.c_str(), nullptr, 16);
            std::string Flags;
            for (int Index = 0; Index < 8; ++Index)
                Flags += VMeshChannelPresent(Fvf, Ids[Index]) ? "1" : "0";
            Out.Line(Format("{\"fvf\":\"%lx\",\"stride\":\"%x\",\"dfm_stride\":\"%x\",\"present\":\"%s\",\"channels\":\"%s\"}",
                            Fvf, VMeshFvfStride(Fvf), OracleDfmFvfStride(Fvf), Flags.c_str(),
                            ChannelsText(Fvf).c_str()));
        }
    }

    // --- the walk ---------------------------------------------------------------

    struct FArchetypeDir
    {
        std::string Path;
        bool bLibrary;
        bool bRender;
        bool bWire;
    };

    std::string JoinPath(const std::string& Path, const std::string& Name)
    {
        return Path.empty() ? Name : Path + "\\" + Name;
    }

    bool IsRenderName(const std::string& Name)
    {
        for (int Index = 0; Index < 4; ++Index)
        {
            if (_stricmp(Name.c_str(), NameRender[Index]) == 0)
                return true;
        }
        return false;
    }

    // Every directory with a trigger child, depth first in archive order.
    void ArchetypeDirs(IFileSystem* Fs, const std::string& Path, int Depth, std::vector<FArchetypeDir>& Out)
    {
        const std::string Pattern = JoinPath(Path, "*");
        WIN32_FIND_DATAA Data;
        const int Find = Fs->FindFirst(Pattern.c_str(), &Data);
        if (Find == -1)
            return;
        std::vector<std::string> Dirs;
        do
        {
            if ((Data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
                Dirs.push_back(Data.cFileName);
        } while (Fs->FindNext((unsigned long)Find, &Data) != 0);
        Fs->FindClose((unsigned long)Find);

        FArchetypeDir Found = {Path, false, false, false};
        for (const std::string& Name : Dirs)
        {
            Found.bLibrary = Found.bLibrary || _stricmp(Name.c_str(), NameLibrary) == 0;
            Found.bRender = Found.bRender || IsRenderName(Name);
            Found.bWire = Found.bWire || _stricmp(Name.c_str(), NameWire) == 0;
        }
        if (Found.bLibrary || Found.bRender || Found.bWire)
            Out.push_back(Found);
        if (Depth >= WalkDepth)
            return;
        for (const std::string& Name : Dirs)
        {
            const bool bOwned = _stricmp(Name.c_str(), NameLibrary) == 0 || _stricmp(Name.c_str(), NameWire) == 0 ||
                                IsRenderName(Name);
            if (bOwned || Name.empty() || Name.find_first_of("\\/") != std::string::npos)
                continue;
            ArchetypeDirs(Fs, JoinPath(Path, Name), Depth + 1, Out);
        }
    }

    // A fresh file system for a directory: the archive opened again for the
    // root, a child of the root otherwise.
    IFileSystem* Fresh(IFileSystem* Root, const std::string& File, const std::string& Path)
    {
        if (Path.empty())
            return OracleMeshOpenUtf(File);
        FFileSystemDesc Desc;
        memset(&Desc, 0, sizeof(Desc));
        Desc.Size = 0x34;
        Desc.ComponentName = "FileSystem";
        Desc.Path = Path.c_str();
        Desc.Access = 0x80000000;
        Desc.ShareMode = 1;
        Desc.Creation = 3;
        Desc.Flags = 0x08000080;
        Desc.Field30 = 0xFFFFFFFF;
        IFileSystem* Fs = nullptr;
        if (Root->CreateInstance((AGGDESC*)&Desc, (void**)&Fs) != GR_OK)
            return nullptr;
        return Fs;
    }

    // --- a library ----------------------------------------------------------------

    const unsigned char* VerticesOf(const FVmeshDataHeader* Header)
    {
        return (const unsigned char*)Header + 0x10 + (unsigned long)Header->FaceGroups * 0xC +
               (unsigned long)Header->Indices * 2;
    }

    void MeshDataLines(const FVmeshData* Data, std::vector<std::string>& Lines)
    {
        const FVmeshDataHeader* H = Data->Header;
        const unsigned int Stride = VMeshFvfStride(H->Fvf);
        Lines.push_back(Format("{\"flags\":\"%02x\",\"u01\":\"%s\",\"prim\":\"%lx\",\"fg\":\"%x\",\"ni\":\"%x\",\"fvf\":\"%x\",\"nv\":\"%x\",\"stride\":\"%x\",\"kib\":\"%s\"}",
                               H->Flags, Hex(H->Unread01, 3).c_str(), H->PrimitiveType, H->FaceGroups, H->Indices,
                               H->Fvf, H->Vertices, Stride, Bits(Data->SizeKib).c_str()));
        const FVmeshFacegroup* Groups = (const FVmeshFacegroup*)((const unsigned char*)H + 0x10);
        for (unsigned int Index = 0; Index < H->FaceGroups; ++Index)
        {
            const FVmeshFacegroup& G = Groups[Index];
            Lines.push_back(Format("{\"fgi\":\"%u\",\"mat\":\"%08lx\",\"vb\":\"%x\",\"ve\":\"%x\",\"ic\":\"%x\",\"u0a\":\"%04x\"}",
                                   Index, G.MaterialCrc, G.VertexBegin, G.VertexEnd, G.IndexCount, G.Unread0A));
        }
        const unsigned char* Vertices = VerticesOf(H);
        const size_t VertexBytes = (size_t)H->Vertices * Stride;
        Lines.push_back(Format("{\"indices\":\"%s\",\"vertices\":\"%s\"}",
                               Fnv64(Groups + H->FaceGroups, (size_t)H->Indices * 2).c_str(),
                               Fnv64(Vertices, VertexBytes).c_str()));
        for (int Index = 0; Index < 6; ++Index)
        {
            FVmeshChannelRecord Record;
            if (!VMeshFillChannelRecord(H, 0, H->Vertices, ChannelIds[Index], &Record))
                continue;
            const size_t Offset = (size_t)((const unsigned char*)Record.Pointer - Vertices);
            const size_t Count = (size_t)Record.VertexCount;
            std::string Gathered;
            if (Count != 0 && Offset + (Count - 1) * (size_t)Record.Stride + (size_t)Record.ElemBytes > VertexBytes)
                Gathered = "oob";
            else
            {
                Bytes Elements;
                for (size_t Vertex = 0; Vertex < Count; ++Vertex)
                {
                    const unsigned char* At = (const unsigned char*)Record.Pointer + Vertex * (size_t)Record.Stride;
                    Elements.insert(Elements.end(), At, At + Record.ElemBytes);
                }
                Gathered = Fnv64(Elements.data(), Elements.size());
            }
            Lines.push_back(Format("{\"ch\":\"%x\",\"offset\":\"%x\",\"elem\":\"%x\",\"stride\":\"%x\",\"data\":\"%s\"}",
                                   ChannelIds[Index], (unsigned int)Offset, (unsigned int)Record.ElemBytes,
                                   (unsigned int)Record.Stride, Gathered.c_str()));
        }
    }

    // The face groups of each member name no more indices than the member
    // has, and all of them fit the group's total: VMeshGroupEnsureIb stays
    // inside both buffers.
    bool IndexBufferFits(const FVmeshGroup* Group)
    {
        unsigned long long Written = 0;
        for (const FVmeshData* Data = Group->First; Data != nullptr; Data = Data->Next)
        {
            const FVmeshDataHeader* H = Data->Header;
            const FVmeshFacegroup* Groups = (const FVmeshFacegroup*)((const unsigned char*)H + 0x10);
            unsigned long long Member = 0;
            for (unsigned int Index = 0; Index < H->FaceGroups; ++Index)
                Member += Groups[Index].IndexCount;
            if (Member > H->Indices)
                return false;
            Written += Member;
            if (Written > Group->IndexTotal)
                return false;
        }
        return true;
    }

    void LibraryLines(IFileSystem* Fs, std::vector<std::string>& Lines)
    {
        GDumps = 0;
        FVmeshDataList* List = (FVmeshDataList*)Library()->Load(Fs, -1);
        const int Dumps = GDumps;
        // A null handle: the trigger guarantees the node, so no mesh loaded.
        const size_t EntryCount = List != nullptr ? List->size() : 0;
        Lines.push_back(Format("{\"library\":\"%u\",\"dumps\":\"%d\"}", (unsigned int)EntryCount, Dumps));
        if (List == nullptr)
            return;
        std::vector<FVmeshData*> Meshes;
        std::vector<FVmeshGroup*> Groups;
        unsigned int Entry = 0;
        for (FVmeshData* Data : *List)
        {
            size_t Mesh = std::find(Meshes.begin(), Meshes.end(), Data) - Meshes.begin();
            const bool bNew = Mesh == Meshes.size();
            if (bNew)
                Meshes.push_back(Data);
            Lines.push_back(Format("{\"entry\":\"%u\",\"name\":\"%s\",\"crc\":\"%08lx\",\"mesh\":\"%u\"}", Entry,
                                   HexC(Data->Name).c_str(), Data->Crc, (unsigned int)Mesh));
            if (bNew)
                MeshDataLines(Data, Lines);
            ++Entry;
        }
        for (FVmeshData* Data : *List)
        {
            if (Data->Group != nullptr && std::find(Groups.begin(), Groups.end(), Data->Group) == Groups.end())
                Groups.push_back(Data->Group);
        }
        for (size_t Ordinal = 0; Ordinal < Groups.size(); ++Ordinal)
        {
            FVmeshGroup* Group = Groups[Ordinal];
            std::string Members;
            for (const FVmeshData* Data = Group->First; Data != nullptr; Data = Data->Next)
            {
                if (!Members.empty())
                    Members += ",";
                Members += Format("%u", (unsigned int)(std::find(Meshes.begin(), Meshes.end(), Data) - Meshes.begin()));
            }
            std::string Ib = "oob";
            std::string Starts;
            if (IndexBufferFits(Group))
            {
                VMeshGroupEnsureIb(Group, 0);
                const std::vector<unsigned short>* Buffer = (const std::vector<unsigned short>*)Group->IndexBuffer;
                Ib = Fnv64(Buffer->data(), (size_t)Group->IndexTotal * 2);
                for (const FVmeshData* Data = Group->First; Data != nullptr; Data = Data->Next)
                {
                    if (!Starts.empty())
                        Starts += ",";
                    Starts += Format("%lx", Data->StartIndex);
                }
                VMeshGroupDestroyIb(Group);
            }
            Lines.push_back(Format("{\"group\":\"%u\",\"fvf\":\"%lx\",\"vt\":\"%lx\",\"it\":\"%lx\",\"members\":\"%s\",\"ib\":\"%s\",\"starts\":\"%s\"}",
                                   (unsigned int)Ordinal, Group->Fvf, Group->VertexTotal, Group->IndexTotal,
                                   Members.c_str(), Ib.c_str(), Starts.c_str()));
        }
        Library()->Unload(List);
    }

    // --- a render archetype -------------------------------------------------------

    std::string StreamLine(const char* Name, const FRpVertexStream& Stream, unsigned long SrcCount)
    {
        const std::string Idx =
            Stream.IndexArray == nullptr ? std::string("-") : Fnv64(Stream.IndexArray, (size_t)SrcCount * 4);
        const size_t Bytes = (size_t)Stream.Count * Stream.ByteSize;
        return Format("{\"stream\":\"%s\",\"n\":\"%x\",\"data\":\"%s\",\"idx\":\"%s\"}", Name, Stream.Count,
                      Fnv64(Bytes != 0 ? Stream.Base : nullptr, Bytes).c_str(), Idx.c_str());
    }

    void TriMeshLines(const FTriMeshArch* Mesh, int Dumps, std::vector<std::string>& Lines)
    {
        const FRpVertexSrcDesc& V = Mesh->Vertices;
        Lines.push_back(Format("{\"trimesh\":\"%x\",\"fvf\":\"%x\",\"count\":\"%x\",\"dumps\":\"%d\"}", V.Flags,
                               V.SrcFvf, V.SrcCount, Dumps));
        Lines.push_back(StreamLine("pos", V.Position, V.SrcCount));
        if (V.Normal.Count != 0)
            Lines.push_back(StreamLine("normal", V.Normal, V.SrcCount));
        if (V.Diffuse.Count != 0)
            Lines.push_back(StreamLine("diffuse", V.Diffuse, V.SrcCount));
        if (V.Tex[0].Count != 0)
            Lines.push_back(StreamLine("tex0", V.Tex[0], V.SrcCount));
        if (V.Tex[1].Count != 0)
            Lines.push_back(StreamLine("tex1", V.Tex[1], V.SrcCount));
        const FTriMeshUvAnim& A = Mesh->UvAnim;
        if (A.LookupCount != 0)
        {
            const std::string Frames =
                A.Frames == nullptr ? std::string("-")
                                    : Fnv64(A.Frames, (size_t)A.LookupCount * (size_t)A.FrameCount * 8);
            Lines.push_back(Format("{\"uvanim\":\"%lx\",\"lookup\":\"%s\",\"frames_n\":\"%lx\",\"frames\":\"%s\",\"fps\":\"%s\",\"interp\":\"%lx\",\"period\":\"%s\"}",
                                   A.LookupCount, Fnv64(A.Lookup, (size_t)A.LookupCount * 4).c_str(), A.FrameCount,
                                   Frames.c_str(), Bits(A.Fps).c_str(), A.Interpolate, Bits(A.Period).c_str()));
        }
        Lines.push_back(Format("{\"groups\":\"%u\"}", (unsigned int)Mesh->Groups.size()));
        for (size_t Index = 0; Index < Mesh->Groups.size(); ++Index)
        {
            const FTriMeshGroup* G = Mesh->Groups[Index];
            const std::string N = G->Indices == nullptr ? std::string("-") : Format("%lx", G->IndexCount);
            const std::string Idx =
                G->Indices == nullptr ? std::string("-") : Fnv64(G->Indices, (size_t)G->IndexCount * 2);
            const std::string Ei =
                G->EdgeIndices == nullptr ? std::string("-") : Fnv64(G->EdgeIndices, (size_t)G->EdgeCount * 2);
            const std::string Ea =
                G->EdgeAngles == nullptr ? std::string("-") : Fnv64(G->EdgeAngles, (size_t)(G->EdgeCount >> 1) * 4);
            Lines.push_back(Format("{\"tg\":\"%u\",\"type\":\"%lx\",\"crc\":\"%08lx\",\"n\":\"%s\",\"idx\":\"%s\",\"min\":\"%lx\",\"max\":\"%lx\",\"edges\":\"%lx\",\"ei\":\"%s\",\"ea\":\"%s\",\"bucket\":\"%lx\"}",
                                   (unsigned int)Index, G->Type, G->MaterialCrc, N.c_str(), Idx.c_str(), G->MinIndex,
                                   G->MaxIndex, G->EdgeCount, Ei.c_str(), Ea.c_str(), G->Bucket));
        }
    }

    std::string SphereLine(const FSphereHandle* Sphere, int Dumps)
    {
        std::string Names;
        for (unsigned long Index = 0; Index < Sphere->Sides; ++Index)
        {
            if (Sphere->MaterialNames == nullptr || Sphere->MaterialNames[Index] == nullptr)
                continue;
            if (!Names.empty())
                Names += ";";
            Names += Format("%lx:%s", Index, HexC(Sphere->MaterialNames[Index]).c_str());
        }
        return Format("{\"sphere\":\"%s\",\"sides\":\"%lx\",\"scale\":\"%s\",\"names\":\"%s\",\"dumps\":\"%d\"}",
                      Bits(Sphere->Radius).c_str(), Sphere->Sides, Bits(Sphere->Scale).c_str(), Names.c_str(),
                      Dumps);
    }

    void RenderLines(IFileSystem* Fs, std::vector<std::string>& Lines)
    {
        static int NextId = 1;
        FRenderManager* Render = Manager();
        const int Id = NextId++;
        // FRenderManager::CreateArchetype, step by step so that the dump
        // knows which of the two succeeded.
        Render->Archetypes.insert(FArchetypeMap::value_type(Id, FLodSet()));
        bool bMulti = true;
        if (Render->MultiLevel(Id, Fs) < 0)
        {
            bMulti = false;
            if (Render->WalkPlugins(Id, Fs) < 0)
            {
                Render->Archetypes.erase(Id);
                Lines.push_back("{\"render\":\"0\"}");
                return;
            }
        }
        FLodSet* Set = Render->FindSet(Id);
        const size_t SwitchCount = Set->LodCount > 0 ? Set->Switch.size() : 0;
        Lines.push_back(Format("{\"render\":\"1\",\"multilevel\":\"%d\",\"levels\":\"%d\",\"switch\":\"%s\"}",
                               bMulti ? 1 : 0, Set->LodCount,
                               BitsList(Set->Switch.data(), SwitchCount).c_str()));
        for (int Level = 0; Level < Set->LodCount; ++Level)
        {
            FCountingPlugin* Plugin = (FCountingPlugin*)Set->Entries[Level].Plugin;
            void* Handle = Set->Entries[Level].Handle;
            const int Dumps = Plugin->Dumps[(int)(intptr_t)Handle];
            Lines.push_back(Format("{\"level\":\"%d\",\"plugin\":\"%s\"}", Level, Plugin->Name));
            if (strcmp(Plugin->Name, "VMesh") == 0)
            {
                const FVmeshRef* R = ((FVMeshPart*)Handle)->Ref;
                std::string Bounds;
                for (int Index = 0; Index < 6; ++Index)
                    Bounds += Format(Index == 0 ? "%08lx" : ",%08lx", R->Box14[Index]);
                Lines.push_back(Format("{\"ref\":\"%lx\",\"crc\":\"%08lx\",\"vs\":\"%x\",\"vc\":\"%x\",\"is\":\"%x\",\"ic\":\"%x\",\"fs\":\"%x\",\"fc\":\"%x\",\"bounds\":\"%s\",\"center\":\"%s\",\"radius\":\"%s\"}",
                                       R->Unread00, R->Crc, R->VertexStart, R->VertexCount, R->IndexStart,
                                       R->IndexCount, R->FacegroupStart, R->FacegroupCount, Bounds.c_str(),
                                       BitsList(R->Extra2C, 3).c_str(), Bits(R->Extra38).c_str()));
            }
            else if (strcmp(Plugin->Name, "TriMesh") == 0)
                TriMeshLines((const FTriMeshArch*)Handle, Dumps, Lines);
            else
                Lines.push_back(SphereLine((const FSphereHandle*)Handle, Dumps));
        }
        Render->DestroyArchetype((void*)(intptr_t)Id);
    }

    // --- a wireframe ----------------------------------------------------------------

    // The size of VMeshWire\VWireData, measured beside the reference (which
    // keeps the leaf but not its size); -1 when it does not open.
    long WireLeafSize(IFileSystem* Fs)
    {
        if (Fs->OpenNode(NameWire) == 0)
            return -1;
        FFileSystemDesc Desc;
        FileDescInit(&Desc, "VWireData", "FileSystem");
        long Size = -1;
        const int Handle = Fs->OpenByDesc(&Desc);
        if (Handle != -1)
        {
            Size = (long)Fs->GetSize((unsigned long)Handle, 0);
            Fs->CloseFile((unsigned long)Handle);
        }
        Fs->OpenNode("..");
        return Size;
    }

    void WireLines(IFileSystem* Fs, long LeafSize, std::vector<std::string>& Lines)
    {
        static int NextKey = 1;
        const int Key = NextKey++;
        GDumps = 0;
        if (Wire()->CreateArchetype((void*)(intptr_t)Key, Fs) == 0)
        {
            Lines.push_back(Format("{\"wire\":\"0\",\"dumps\":\"%d\"}", GDumps));
            return;
        }
        std::map<int, FVmeshWireInstance*>::iterator Found = Wire()->Archetypes.find(Key);
        const FVwireDataHeader* W = Found->second->Blob;
        if (LeafSize < 0x10 || (unsigned long)LeafSize < 0x10ul + (unsigned long)W->Count * 2)
            Lines.push_back("{\"wire\":\"short\"}");
        else
            Lines.push_back(Format("{\"wire\":\"1\",\"u00\":\"%x\",\"u02\":\"%x\",\"crc\":\"%08lx\",\"bias\":\"%x\",\"remap\":\"%x\",\"count\":\"%x\",\"nv\":\"%x\",\"idx\":\"%s\",\"trailing\":\"%lx\"}",
                                   W->Unread00, W->Unread02, W->Crc, W->IbBias, W->RemapCount, W->Count,
                                   W->NumVertices, Fnv64(W + 1, (size_t)W->Count * 2).c_str(),
                                   (unsigned long)LeafSize - 0x10ul - (unsigned long)W->Count * 2));
        Wire()->DestroyArchetype((void*)(intptr_t)Key);
    }

    // --- one archive ------------------------------------------------------------------

    std::vector<std::string> MeshFileLines(const std::string& File)
    {
        InstallBuffers();
        std::vector<std::string> Lines;
        IFileSystem* Root = OracleMeshOpenUtf(File);
        Lines.push_back(Format("{\"file\":\"%s\",\"open\":\"%d\"}", File.c_str(), Root != nullptr ? 1 : 0));
        if (Root == nullptr)
            return Lines;
        std::vector<FArchetypeDir> Dirs;
        ArchetypeDirs(Root, "", 0, Dirs);
        for (const FArchetypeDir& Dir : Dirs)
        {
            Lines.push_back(Format("{\"node\":\"%s\"}", Hex(Dir.Path.data(), Dir.Path.size()).c_str()));
            if (Dir.bLibrary)
            {
                IFileSystem* Fs = Fresh(Root, File, Dir.Path);
                if (Fs == nullptr)
                    Lines.push_back("{\"library\":\"nofs\"}");
                else
                {
                    LibraryLines(Fs, Lines);
                    Fs->Release();
                }
            }
            if (Dir.bRender)
            {
                IFileSystem* Fs = Fresh(Root, File, Dir.Path);
                if (Fs == nullptr)
                    Lines.push_back("{\"render\":\"nofs\"}");
                else
                {
                    RenderLines(Fs, Lines);
                    Fs->Release();
                }
            }
            if (Dir.bWire)
            {
                IFileSystem* Measure = Fresh(Root, File, Dir.Path);
                IFileSystem* Fs = Fresh(Root, File, Dir.Path);
                if (Fs == nullptr || Measure == nullptr)
                    Lines.push_back("{\"wire\":\"nofs\"}");
                else
                    WireLines(Fs, WireLeafSize(Measure), Lines);
                if (Measure != nullptr)
                    Measure->Release();
                if (Fs != nullptr)
                    Fs->Release();
            }
        }
        Root->Release();
        return Lines;
    }

    void MeshFiles()
    {
        Suite Out("mesh_files.jsonl");
        for (const std::string& File : InputLines("mesh_data/mesh.list"))
        {
            for (const std::string& Line : MeshFileLines(File))
                Out.Line(Line);
        }
    }

    std::string LowerExtension(const std::string& File)
    {
        const size_t Dot = File.find_last_of('.');
        if (Dot == std::string::npos)
            return std::string();
        std::string Out = File.substr(Dot + 1);
        for (char& Ch : Out)
            Ch = (char)tolower((unsigned char)Ch);
        return Out;
    }
}

// Also used by oracle_mesh_data_dfm.cpp: the files with one of the
// extensions (lower case) whose first bytes are "UTF ".
std::vector<std::string> OracleMeshUtfFiles(const std::vector<std::string>& Files, const char* const* Extensions,
                                            int ExtensionCount)
{
    std::vector<std::string> Out;
    for (const std::string& File : Files)
    {
        const std::string Extension = LowerExtension(File);
        bool bWanted = false;
        for (int Index = 0; Index < ExtensionCount; ++Index)
            bWanted = bWanted || Extension == Extensions[Index];
        if (bWanted && FileMagic(File) == "UTF ")
            Out.push_back(File);
    }
    return Out;
}

void RunMeshDataSuites()
{
    MeshFvf();
    MeshFiles();
    RunDfmFilesSuite();
}

void RunMeshDataDump(const std::vector<std::string>& Files)
{
    const char* const Extensions[4] = {"vms", "3db", "cmp", "sph"};
    const std::vector<std::string> Mesh = OracleMeshUtfFiles(Files, Extensions, 4);
    {
        Suite List(GOutputDir + "\\mesh\\files.txt");
        for (const std::string& File : Mesh)
            List.Line(File);
    }
    for (const std::string& File : Mesh)
    {
        Suite Out(DataOutputFile("mesh", File));
        for (const std::string& Line : MeshFileLines(File))
            Out.Line(Line);
    }
    printf("dumped %u mesh archives\n", (unsigned int)Mesh.size());
    RunDfmDataDump(Files);
}

// P4.6: the mesh render suite (mesh_render.jsonl) reaches the reference's RendComp classes, so it
// is compiled here, after the sources it uses.
#include "oracle_mesh_render.cpp"
