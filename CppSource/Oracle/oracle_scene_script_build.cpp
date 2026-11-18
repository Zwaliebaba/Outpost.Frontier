// P4.14: the scene-script parser's second pass, FSceneScriptLuaLoadParser::Build
// (CppSource/SceneScript/SceneScriptLua.cpp), run as the reference runs it.
//
// SceneScriptLua.cpp is compiled into this file, so its file-local state can be
// reset between cases, and SceneScriptScene.cpp beside it, so the parser's
// temporary events are the reference's own. The parser talks to recording
// stand-ins at its seams:
//   - IScriptEngine: Slot35AddEntity numbers entities from 0 per load and keeps
//     a copy of each descriptor, which Slot9Entity hands back by handle (as a
//     fresh FSceneScriptScene does); Slot34, Slot37 and Slot38 are recorded;
//   - ISceneScriptComContext: Slot3 (the USER_EVENT string) is recorded;
//   - the component manager (NEURONCOMPONENTS_Acquire, redirected below)
//     creates recording objects for the factory names SceneScript.dll
//     registers: param curves record AddKey, spline paths record Slot3Load and
//     Slot4LoadPoints. Every other name fails, as an unregistered one does.
// The Rust side is crates/neuron-client/tests/support/scene_script_build.rs.
//
// Line format, per buffer:
//   {"case":"<n>","buffer":"<m>","load":"<0|-1>"[,"entities":..,"events":..,"duration":..]}  (suite)
//   {"file":"<path>","load":...}                                                             (data)
//   {"entity":"<handle>","type":..,"flags":..,"pos":"<3 words>","orient":"<9 words>",
//    "name":"<str>","template":"<str>","groups":"<lt> <srt> <usr>","tail":"<tokens>"}
//   {"userprop":"<handle>","key":"<str>","value":"<str>"}   sorted, after their entity
//   {"update":"<handle>","pos0":"<word>"}
//   {"event":"<type>","start":..,"flags":..,"event_flags":..,"field10":..,"duration":..,
//    "primary":..,"secondary":..,"curve":"<curve>","period":..,"payload":"<tokens>"}
//   {"user_string":"<str>"}
// A word is 8 hex digits of its bits; <str> is s<hex bytes> or null. The entity tail and the
// event payload are the descriptor words after the common part, in layout order, with each
// string pointer given as <str>; the START_IK key is "self" when it is the event's address.
// A motion path tail is <path type> then null or o<factory hex> followed by l<str> for each
// IPersistString::Slot3Load and p<count> <3 words per point> for each Slot4LoadPoints. A curve
// is null or s<factory hex> followed by 4 words per AddKey. The descriptor's UniqueId is not
// written: it is the temporary event's serial number, which the engine replaces.

// The parser's only component-manager calls create curves and paths.
#define NEURONCOMPONENTS_Acquire OracleSceneScriptBuildManager

#include "../SceneScript/SceneScriptLua.cpp"

#include <algorithm>
#include <string>
#include <vector>

#include "oracle_support.h"

// SceneScript.cpp (not compiled in) defines these for SceneScriptScene.cpp; the
// parser's paths reach neither.
void SceneScriptFdump(const char* Message)
{
    if (FDUMP != nullptr)
        FDUMP(0x100002, "%s", Message != nullptr ? Message : "");
}

I3DMathEngine* SceneScriptAcquire3DMathEngine()
{
    return nullptr;
}

namespace scene_script_build
{
    std::string Word(const void* Data)
    {
        unsigned int Bits;
        std::memcpy(&Bits, Data, sizeof(Bits));
        return oracle::Format("%08x", Bits);
    }

    std::string Words(const void* Data, size_t Count)
    {
        std::string Text;
        for (size_t Index = 0; Index < Count; ++Index)
        {
            if (Index != 0)
                Text += " ";
            Text += Word(static_cast<const unsigned char*>(Data) + Index * 4);
        }
        return Text;
    }

    std::string Str(const char* Text)
    {
        return Text == nullptr ? std::string("null") : "s" + oracle::HexC(Text);
    }

    // The factories SceneScript.dll registers (SceneScript.cpp).
    const char* const CurveNames[] = {
        "CatmullRomPCurve", "SceneScriptParamCurve", "LinearPCurve", "SceneScriptLPCurve",
        "SmoothPCurve",     "StepPCurve",            "RampUpPCurve", "RampDownPCurve",
        "BumpOutPCurve",    "BumpInPCurve",          "FreeFormPCurve"};
    const char* const PathNames[] = {"CV_CRSplinePath", "CV_CROrientationSplinePath"};

    template <size_t N>
    bool IsOneOf(const char* Name, const char* const (&Names)[N])
    {
        for (const char* Candidate : Names)
        {
            if (std::strcmp(Name, Candidate) == 0)
                return true;
        }
        return false;
    }

    // Recording objects live until the process ends: the parser releases curves
    // after the engine has copied them, and nothing here outlives a dump.
    class FRecordingCurve final : public IParamCurve
    {
    public:
        explicit FRecordingCurve(const char* Name) : Name(Name) {}

        GENRESULT NEURONCOMPONENTS_API QueryInterface(const char* Iid, void** Out) override
        {
            if (std::strcmp(Iid, Iids::ParamCurve) != 0)
            {
                *Out = nullptr;
                return GR_INTERFACE_UNSUPPORTED;
            }
            *Out = static_cast<IParamCurve*>(this);
            return GR_OK;
        }
        unsigned long NEURONCOMPONENTS_API AddRef() override { return 1; }
        unsigned long NEURONCOMPONENTS_API Release() override { return 1; }
        float NEURONCOMPONENTS_API Evaluate(float) override { return 0.0f; }
        unsigned char NEURONCOMPONENTS_API Slot4Replace(int, float, float, float, float) override { return 0; }
        unsigned char NEURONCOMPONENTS_API Slot5ReadKey(int, float*, float*, float*, float*) override { return 0; }
        int NEURONCOMPONENTS_API AddKey(float F0, float F1, float F2, float F3) override
        {
            const float Key[4] = {F0, F1, F2, F3};
            Keys += " " + Words(Key, 4);
            return 0;
        }
        unsigned char NEURONCOMPONENTS_API Slot7DeleteKey(int) override { return 0; }
        unsigned int NEURONCOMPONENTS_API GetKeyCount() override { return 0; }
        int NEURONCOMPONENTS_API Slot9IdByIndex(int) override { return -1; }
        const char* NEURONCOMPONENTS_API Slot10Clsid() override { return Name.c_str(); }
        const char* NEURONCOMPONENTS_API Slot11Display() override { return Name.c_str(); }
        float NEURONCOMPONENTS_API Slot12WrapEvaluate(int, int) override { return 0.0f; }
        void NEURONCOMPONENTS_API Slot13Rebuild() override {}
        void* NEURONCOMPONENTS_API Slot14Dtor(unsigned char) override { return this; }
        int NEURONCOMPONENTS_API Slot15PersistStub(void*) override { return 0; }
        int NEURONCOMPONENTS_API Slot16PersistStub(void*) override { return 0; }
        void NEURONCOMPONENTS_API EvaluateToExtended(const float*, FSceneScriptExtendedProgress* Output) override
        {
            std::memset(Output, 0, sizeof(*Output));
        }
        void NEURONCOMPONENTS_API WrapEvaluateToExtended(int, int, FSceneScriptExtendedProgress* Output) override
        {
            std::memset(Output, 0, sizeof(*Output));
        }

        std::string Text() const { return "s" + oracle::HexC(Name.c_str()) + Keys; }

    private:
        std::string Name;
        std::string Keys;
    };

    class FRecordingPath final : public IPersistString, public ICRSpline
    {
    public:
        explicit FRecordingPath(const char* Name) : Name(Name) {}

        GENRESULT NEURONCOMPONENTS_API QueryInterface(const char* Iid, void** Out) override
        {
            if (std::strcmp(Iid, Iids::PersistString) == 0)
                *Out = static_cast<IPersistString*>(this);
            else if (std::strcmp(Iid, Iids::CRSpline) == 0)
                *Out = static_cast<ICRSpline*>(this);
            else
            {
                *Out = nullptr;
                return GR_INTERFACE_UNSUPPORTED;
            }
            return GR_OK;
        }
        unsigned long NEURONCOMPONENTS_API AddRef() override { return 1; }
        unsigned long NEURONCOMPONENTS_API Release() override { return 1; }
        GENRESULT NEURONCOMPONENTS_API Slot3Load(const char* Text) override
        {
            Calls += " l" + Str(Text);
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API Slot4Save(char*, unsigned long) override { return GR_GENERIC; }
        unsigned long NEURONCOMPONENTS_API Slot5EstimateSize() override { return 0; }
        GENRESULT NEURONCOMPONENTS_API Slot3(unsigned int, float*) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API Slot4LoadPoints(const float* Points, unsigned long Count) override
        {
            Calls += oracle::Format(" p%lu", Count);
            if (Count != 0)
                Calls += " " + Words(Points, Count * 3);
            return GR_OK;
        }
        unsigned long NEURONCOMPONENTS_API Slot5() override { return 0; }
        GENRESULT NEURONCOMPONENTS_API Slot6(unsigned int, float*) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API Slot7(unsigned int, float*) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API Slot8(unsigned char) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API Slot9() override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API Slot10(unsigned int, float*) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API Slot11(unsigned int, float*) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API Slot12(unsigned int, float*) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API Slot13(unsigned int, const float*) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API Slot14(unsigned int, const float*) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API Slot15(unsigned int, float*) override { return GR_GENERIC; }

        std::string Text() const { return "o" + oracle::HexC(Name.c_str()) + Calls; }

    private:
        std::string Name;
        std::string Calls;
    };

    // A created component is handed out as INeuronComponent; the parser then
    // asks it for the interface it wants.
    class FRecordingManager final : public ICOManager
    {
    public:
        GENRESULT NEURONCOMPONENTS_API QueryInterface(const char*, void** Out) override
        {
            *Out = nullptr;
            return GR_INTERFACE_UNSUPPORTED;
        }
        unsigned long NEURONCOMPONENTS_API AddRef() override { return 1; }
        unsigned long NEURONCOMPONENTS_API Release() override { return 1; }
        GENRESULT NEURONCOMPONENTS_API CreateInstance(AGGDESC* Desc, void** Instance) override
        {
            const FSceneScriptAggDesc14* Extended = reinterpret_cast<const FSceneScriptAggDesc14*>(Desc);
            const char* Name = Extended->NameAt4;
            *Instance = nullptr;
            if (Desc->size != G_AggDescSizeSceneScript || Name == nullptr)
                return GR_INTERFACE_UNSUPPORTED;
            if (IsOneOf(Name, CurveNames))
                *Instance = static_cast<INeuronComponent*>(static_cast<IParamCurve*>(new FRecordingCurve(Name)));
            else if (IsOneOf(Name, PathNames))
                *Instance = static_cast<INeuronComponent*>(static_cast<IPersistString*>(new FRecordingPath(Name)));
            else
                return GR_INTERFACE_UNSUPPORTED;
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API RegisterComponent(INeuronComponent*, const char*, void*) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API UnregisterComponent(INeuronComponent*, const char*) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API EnumerateComponents(const char*, FComponentEnumCallback, void*) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API AddLibrary(const char*) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API RemoveLibrary(const char*) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API Shutdown() override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API SetINIConfig(const char*, unsigned int) override { return GR_GENERIC; }
    };

    FRecordingManager GManager;

    // The script as a file: what the parser's Load reads.
    class FBufferFileSystem final : public IFileSystem
    {
    public:
        FBufferFileSystem(const unsigned char* Data, size_t Size) : Data(Data), Size(Size) {}

        GENRESULT NEURONCOMPONENTS_API QueryInterface(const char*, void** Out) override
        {
            *Out = nullptr;
            return GR_INTERFACE_UNSUPPORTED;
        }
        unsigned long NEURONCOMPONENTS_API AddRef() override { return 1; }
        unsigned long NEURONCOMPONENTS_API Release() override { return 1; }
        GENRESULT NEURONCOMPONENTS_API CreateInstance(AGGDESC*, void** Instance) override
        {
            *Instance = nullptr;
            return GR_GENERIC;
        }
        int NEURONCOMPONENTS_API CloseFile(unsigned long) override { return 0; }
        int NEURONCOMPONENTS_API Read(unsigned long, void* Buffer, unsigned long Count, unsigned long* BytesRead,
                                      unsigned long) override
        {
            const size_t Copy = Count < Size ? Count : Size;
            if (Copy != 0)
                std::memcpy(Buffer, Data, Copy);
            if (BytesRead != nullptr)
                *BytesRead = static_cast<unsigned long>(Copy);
            return 1;
        }
        int NEURONCOMPONENTS_API Write(unsigned long, const void*, unsigned long, unsigned long*, const void*) override { return 0; }
        int NEURONCOMPONENTS_API RejectOperation1C(unsigned long, unsigned long, unsigned long, unsigned long) override { return 0; }
        int NEURONCOMPONENTS_API SeekFile(unsigned long, unsigned long, unsigned long*, unsigned long) override { return 0; }
        int NEURONCOMPONENTS_API SetEndOfFile(unsigned long) override { return 0; }
        unsigned long NEURONCOMPONENTS_API GetSize(unsigned long, unsigned long* HighOut) override
        {
            if (HighOut != nullptr)
                *HighOut = 0;
            return static_cast<unsigned long>(Size);
        }
        int NEURONCOMPONENTS_API ReturnOne2C(unsigned long, unsigned long, unsigned long, unsigned long, unsigned long) override { return 1; }
        int NEURONCOMPONENTS_API ReturnOne30(unsigned long, unsigned long, unsigned long, unsigned long, unsigned long) override { return 1; }
        int NEURONCOMPONENTS_API GetFileTimes(unsigned long, FILETIME*, FILETIME*, FILETIME*) override { return 0; }
        int NEURONCOMPONENTS_API RejectOperation38(unsigned long, unsigned long, unsigned long, unsigned long) override { return 0; }
        int NEURONCOMPONENTS_API CreateMapping(unsigned long, unsigned long, unsigned long, unsigned long, unsigned long,
                                               unsigned long) override { return 0; }
        int NEURONCOMPONENTS_API MapFileView(unsigned long, unsigned long, unsigned long, unsigned long, unsigned long) override { return 0; }
        int NEURONCOMPONENTS_API UnmapFileView(unsigned long) override { return 0; }
        int NEURONCOMPONENTS_API FindFirst(const char*, WIN32_FIND_DATAA*) override { return -1; }
        int NEURONCOMPONENTS_API FindNext(unsigned long, WIN32_FIND_DATAA*) override { return 0; }
        int NEURONCOMPONENTS_API FindClose(unsigned long) override { return 0; }
        int NEURONCOMPONENTS_API RejectOperation54(const char*, unsigned long) override { return 0; }
        int NEURONCOMPONENTS_API RejectOperation58(unsigned long) override { return 0; }
        int NEURONCOMPONENTS_API GetCurrentPath(unsigned long, char* Out) override
        {
            if (Out != nullptr)
                Out[0] = '\0';
            return 0;
        }
        int NEURONCOMPONENTS_API OpenNode(const char*) override { return 0; }
        int NEURONCOMPONENTS_API RejectOperation64(unsigned long) override { return 0; }
        int NEURONCOMPONENTS_API RejectOperation68(unsigned long, unsigned long, unsigned long) override { return 0; }
        int NEURONCOMPONENTS_API RejectOperation6C(unsigned long, unsigned long) override { return 0; }
        int NEURONCOMPONENTS_API GetPathAttributes(const char*) override { return -1; }
        int NEURONCOMPONENTS_API RejectOperation74(unsigned long, unsigned long) override { return 0; }
        int NEURONCOMPONENTS_API GetFileSystemError() override { return 0; }
        int NEURONCOMPONENTS_API OpenByDesc(FFileSystemDesc*) override { return 0; }
        int NEURONCOMPONENTS_API GetFilePosition(unsigned long, unsigned long*) override { return 0; }
        void NEURONCOMPONENTS_API GetFileSysPath(char* Out, unsigned long MaxChars) override
        {
            if (Out != nullptr && MaxChars != 0)
                std::snprintf(Out, MaxChars, "%s", "oracle");
        }
        int NEURONCOMPONENTS_API GetAccessFlags() override { return 0; }
        int NEURONCOMPONENTS_API GetParentFileSystem(IFileSystem** Out) override
        {
            *Out = nullptr;
            return 0;
        }
        int NEURONCOMPONENTS_API FailOperation90(unsigned long, unsigned long) override { return 0; }
        int NEURONCOMPONENTS_API FailOperation94(unsigned long, unsigned long) override { return 0; }
        int NEURONCOMPONENTS_API FailOperation98(unsigned long, unsigned long, unsigned long, unsigned long,
                                                 unsigned long) override { return 0; }
        int NEURONCOMPONENTS_API FailOperation9C(unsigned long, unsigned long, unsigned long, unsigned long,
                                                 unsigned long) override { return 0; }
        int NEURONCOMPONENTS_API InvokeLockedCallback(unsigned long, unsigned long, unsigned long) override { return 0; }
        int NEURONCOMPONENTS_API ResolvePath(char* Dest, const char*, unsigned long DestChars) override
        {
            if (Dest != nullptr && DestChars != 0)
                Dest[0] = '\0';
            return 0;
        }

    private:
        const unsigned char* Data;
        size_t Size;
    };

    // The offset of the START_IK descriptor in its event, for the key the
    // descriptor carries (PackIkDesc: the event's own address).
    size_t IkDescOffset()
    {
        static size_t Offset = 0;
        if (Offset == 0)
        {
            FSceneScriptEvent* Probe = new FSceneScriptEvent(kSceneScriptTypeStartIk);
            Offset = static_cast<size_t>(reinterpret_cast<const unsigned char*>(&Probe->IkDesc.Common) -
                                         reinterpret_cast<const unsigned char*>(Probe));
            FSceneScriptEvent::DestroyNative(Probe);
        }
        return Offset;
    }

    std::string EntityTail(const FSceneScriptEntitySnap* Desc)
    {
        switch (Desc->Type)
        {
        case kSceneScriptEntityCompound:
        case kSceneScriptEntityDeformable:
        case kSceneScriptEntityPsys:
            return Words(Desc->Extra, 1);
        case kSceneScriptEntityCamera:
        case kSceneScriptEntityMonitor:
            return Words(Desc->Extra, 4);
        case kSceneScriptEntityLight:
            return Words(Desc->Extra, 36);
        case kSceneScriptEntitySound:
            return Words(Desc->Extra, 8);
        case kSceneScriptEntityScene:
        case kSceneScriptEntitySubScene:
            return Words(Desc->Extra, 15);
        case kSceneScriptEntityMotionPath:
        {
            std::string Text = Str(Desc->MotionPath.PathType) + " ";
            if (Desc->MotionPath.MotionPath == nullptr)
                return Text + "null";
            return Text + static_cast<const FRecordingPath*>(Desc->MotionPath.MotionPath)->Text();
        }
        default:
            return std::string();
        }
    }

    // Payload words after the common eleven, by event type, and which of them
    // are string pointers.
    std::string EventPayload(const void* Desc)
    {
        const unsigned char* Base = static_cast<const unsigned char*>(Desc);
        int Type;
        std::memcpy(&Type, Base, sizeof(Type));
        size_t Count = 0;
        int Strings[2] = {-1, -1};
        int Key = -1;
        switch (Type)
        {
        case kSceneScriptTypeUserEvent: Count = 1; break;
        case kSceneScriptTypeSetCamera: Count = 0; break;
        case kSceneScriptTypeStartSound: Count = 1; break;
        case kSceneScriptTypeLightPropAnim: Count = 37; break;
        case kSceneScriptTypeCameraPropAnim: Count = 5; break;
        case kSceneScriptTypePathAnim: Count = 8; break;
        case kSceneScriptTypeSpatialPropAnim: Count = 21; Strings[0] = 19; break;
        case kSceneScriptTypeAttachEntity: Count = 7; Strings[0] = 6; break;
        case kSceneScriptTypeConnectHardpoints: Count = 2; Strings[0] = 0; Strings[1] = 1; break;
        case kSceneScriptTypeStartMotion: Count = 10; Strings[0] = 4; Strings[1] = 7; break;
        case kSceneScriptTypeStartIk: Count = 10; Key = 0; Strings[0] = 7; Strings[1] = 8; break;
        case kSceneScriptTypeStartSubScene: Count = 1; break;
        case kSceneScriptTypeStartPsys: Count = 0; break;
        case kSceneScriptTypePsysPropAnim: Count = 2; break;
        case kSceneScriptTypeAudioPropAnim: Count = 9; break;
        case kSceneScriptTypeFogPropAnim: Count = 9; break;
        case kSceneScriptTypeReverbPropAnim: Count = 5; break;
        case kSceneScriptTypeFloorHeightAnim: Count = 3; Strings[0] = 1; break;
        case kSceneScriptTypeSubtitle: Count = 1; break;
        default: break;
        }
        std::string Text;
        for (size_t Index = 0; Index < Count; ++Index)
        {
            if (Index != 0)
                Text += " ";
            const unsigned char* At = Base + sizeof(FSceneScriptEventDesc) + Index * 4;
            if (static_cast<int>(Index) == Strings[0] || static_cast<int>(Index) == Strings[1])
            {
                const char* Pointer;
                std::memcpy(&Pointer, At, sizeof(Pointer));
                Text += Str(Pointer);
            }
            else if (static_cast<int>(Index) == Key &&
                     reinterpret_cast<const unsigned char*>(
                         *reinterpret_cast<const void* const*>(At)) == Base - IkDescOffset())
                Text += "self";
            else
                Text += Word(At);
        }
        return Text;
    }

    class FRecordingEngine;

    class FRecordingContext final : public ISceneScriptComContext
    {
    public:
        explicit FRecordingContext(FRecordingEngine& Engine) : Engine(Engine) {}

        GENRESULT NEURONCOMPONENTS_API QueryInterface(const char*, void** Out) override
        {
            *Out = nullptr;
            return GR_INTERFACE_UNSUPPORTED;
        }
        unsigned long NEURONCOMPONENTS_API AddRef() override { return 1; }
        unsigned long NEURONCOMPONENTS_API Release() override { return 1; }
        GENRESULT NEURONCOMPONENTS_API Slot3(FSceneScriptUserEventDesc*, const char* String) override;
        GENRESULT NEURONCOMPONENTS_API Slot4(FSceneScriptUserEventDesc*, char* Buffer, unsigned int Size) override
        {
            if (Buffer != nullptr && Size != 0)
                Buffer[0] = '\0';
            return GR_GENERIC;
        }
        unsigned int NEURONCOMPONENTS_API Slot5(FSceneScriptUserEventDesc*) override { return 0; }

    private:
        FRecordingEngine& Engine;
    };

    class FRecordingEngine final : public IScriptEngine
    {
    public:
        explicit FRecordingEngine(oracle::Suite& Out) : Out(Out) {}

        // Lines go out in call order, except that the user properties of an
        // entity are sorted: the reference visits string keys in the order of
        // their addresses (divergence D200).
        void Line(const std::string& Text)
        {
            Flush();
            Out.Line(Text);
        }

        void Flush()
        {
            std::sort(UserProps.begin(), UserProps.end());
            for (const std::string& Text : UserProps)
                Out.Line(Text);
            UserProps.clear();
        }

        GENRESULT NEURONCOMPONENTS_API QueryInterface(const char*, void** Out) override
        {
            *Out = nullptr;
            return GR_INTERFACE_UNSUPPORTED;
        }
        unsigned long NEURONCOMPONENTS_API AddRef() override { return 1; }
        unsigned long NEURONCOMPONENTS_API Release() override { return 1; }
        GENRESULT NEURONCOMPONENTS_API Slot3(void*, void*) override { return GR_OK; }
        GENRESULT NEURONCOMPONENTS_API Slot4Load(void*) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API Slot5Save(void*) override { return GR_GENERIC; }
        unsigned long NEURONCOMPONENTS_API Slot6EntityCount() override { return static_cast<unsigned long>(Entities.size()); }
        void* NEURONCOMPONENTS_API Slot7EntityHandle(int) override { return nullptr; }
        void* NEURONCOMPONENTS_API Slot8(int) override { return nullptr; }
        void* NEURONCOMPONENTS_API Slot9Entity(int Index) override
        {
            if (Index < 0 || static_cast<size_t>(Index) >= Entities.size())
                return nullptr;
            return &Entities[static_cast<size_t>(Index)];
        }
        GENRESULT NEURONCOMPONENTS_API Slot10GetPos(int, float*) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API Slot11GetOrient(int, float*) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API Slot12Attach(int, void*) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API Slot13Detach(int) override { return GR_GENERIC; }
        unsigned long NEURONCOMPONENTS_API Slot14EventCount() override { return 0; }
        void* NEURONCOMPONENTS_API Slot15EventByIndex(int) override { return nullptr; }
        void* NEURONCOMPONENTS_API Slot16EventById(int) override { return nullptr; }
        int NEURONCOMPONENTS_API Slot17GetDuration() override { return 0; }
        GENRESULT NEURONCOMPONENTS_API Slot18SetDuration(int) override { return GR_OK; }
        int NEURONCOMPONENTS_API Slot19GetClock() override { return 0; }
        GENRESULT __thiscall Slot20SetLoop(unsigned char) override { return GR_OK; }
        GENRESULT NEURONCOMPONENTS_API Slot21GetState() override { return 0; }
        GENRESULT NEURONCOMPONENTS_API Slot22Start(int) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API Slot23Stop() override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API Slot24Pause() override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API Slot25Start(int) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API Slot26Tick(int) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API Slot27(void*) override { return GR_OK; }
        GENRESULT NEURONCOMPONENTS_API Slot28(void*, void*) override { return GR_OK; }
        GENRESULT NEURONCOMPONENTS_API Slot29(void*, void*, void*) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API Slot30(void*, void*, void*) override { return GR_GENERIC; }
        unsigned long NEURONCOMPONENTS_API Slot31(int) override { return 0; }
        const char* NEURONCOMPONENTS_API Slot32(int, int) override { return nullptr; }
        GENRESULT NEURONCOMPONENTS_API Slot33UserProp(void*, const char*, char**) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API Slot34SetUserProp(int Index, const char* Key, const char* Value) override
        {
            UserProps.push_back(oracle::Format("{\"userprop\":\"%d\",\"key\":\"", Index) + Str(Key) +
                                "\",\"value\":\"" + Str(Value) + "\"}");
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API Slot35AddEntity(void* DescIn, int* OutIndex) override
        {
            const FSceneScriptEntitySnap* Desc = static_cast<const FSceneScriptEntitySnap*>(DescIn);
            const int Handle = static_cast<int>(Entities.size());
            Entities.push_back(*Desc);
            *OutIndex = Handle;
            Line(oracle::Format("{\"entity\":\"%d\",\"type\":\"%d\",\"flags\":\"%d\",\"pos\":\"", Handle, Desc->Type,
                                Desc->Flags) +
                 Words(Desc->Pos, 3) + "\",\"orient\":\"" + Words(Desc->Orient, 9) + "\",\"name\":\"" +
                 Str(Desc->Name) + "\",\"template\":\"" + Str(Desc->TemplateName) +
                 oracle::Format("\",\"groups\":\"%u %u %u\",\"tail\":\"", Desc->LtGrp, Desc->SrtGrp, Desc->UsrFlg) +
                 EntityTail(Desc) + "\"}");
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API Slot36RemoveEntity(int) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API Slot37UpdateEntity(int Index, void* DescIn) override
        {
            const FSceneScriptEntitySnap* Desc = static_cast<const FSceneScriptEntitySnap*>(DescIn);
            Line(oracle::Format("{\"update\":\"%d\",\"pos0\":\"", Index) + Word(&Desc->Pos[0]) + "\"}");
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API Slot38AddEvent(const void* DescIn, int* OutId) override
        {
            const FSceneScriptEventDesc* Desc = static_cast<const FSceneScriptEventDesc*>(DescIn);
            const std::string Curve = Desc->ParamCurve == nullptr
                ? std::string("null")
                : static_cast<const FRecordingCurve*>(Desc->ParamCurve)->Text();
            Line(oracle::Format("{\"event\":\"%d\",\"start\":\"%d\",\"flags\":\"%d\",\"event_flags\":\"%d\","
                                "\"field10\":\"%d\",\"duration\":\"%d\",\"primary\":\"%d\",\"secondary\":\"%d\","
                                "\"curve\":\"",
                                Desc->Type, Desc->StartMs, Desc->Flags, Desc->EventFlags, Desc->Field10,
                                Desc->DurationMs, Desc->PrimaryIndex, Desc->SecondaryIndex) +
                 Curve + oracle::Format("\",\"period\":\"%d\",\"payload\":\"", Desc->Field28) + EventPayload(DescIn) +
                 "\"}");
            *OutId = NextEventId++;
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API Slot39RemoveEvent(int) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API Slot40SetCom(ISceneScriptComContext*) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API Slot41SetLoadParser(ISceneScriptLoadParser*) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API Slot42SetSaveParser(ISceneScriptSaveParser*) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API Slot43LoadPath(const char*) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API Slot44SavePath(const char*) override { return GR_GENERIC; }

    private:
        oracle::Suite& Out;
        std::vector<FSceneScriptEntitySnap> Entities;
        std::vector<std::string> UserProps;
        int NextEventId = 0;
    };

    GENRESULT NEURONCOMPONENTS_API FRecordingContext::Slot3(FSceneScriptUserEventDesc*, const char* String)
    {
        Engine.Line("{\"user_string\":\"" + Str(String) + "\"}");
        return GR_OK;
    }

    // A fresh module state: Lua state, constants and the entity-name map.
    void OpenModule()
    {
        SceneScriptLuaOpen();
        G_bLuaConstantsRegistered = false;
        G_LuaEntityHandles.clear();
    }

    void CloseModule()
    {
        SceneScriptLuaClose();
        G_bLuaConstantsRegistered = false;
        G_LuaEntityHandles.clear();
    }

    // One load as FScriptEngine::Slot4Load runs it: a new parser, Load, then
    // Build when Load succeeded. Head is the start of the status line.
    void RunBuffer(oracle::Suite& Out, const std::string& Head, const unsigned char* Data, size_t Size,
                   bool bContext)
    {
        FSceneScriptScene Scene;
        FBufferFileSystem FileSystem(Data, Size);
        ISceneScriptLoadParser* Parser = SceneScriptCreateLuaLoadParser(&Scene);
        const GENRESULT Status = Parser->Load(&FileSystem);
        if (Status < 0)
            Out.Line(Head + "\"load\":\"-1\"}");
        else
        {
            Out.Line(Head + oracle::Format("\"load\":\"0\",\"entities\":\"%u\",\"events\":\"%u\",\"duration\":\"%u\"}",
                                           Parser->GetEntityCount(), Parser->GetEventCount(),
                                           Parser->GetDuration()));
            FRecordingEngine Engine(Out);
            FRecordingContext Context(Engine);
            Parser->Build(&FileSystem, &Engine, bContext ? &Context : nullptr);
            Engine.Flush();
        }
        Parser->Destroy(1);
    }

    bool IsThn(const std::string& File)
    {
        if (File.compare(0, 5, "DATA/") != 0 || File.size() < 4)
            return false;
        const std::string Tail = File.substr(File.size() - 4);
        return _stricmp(Tail.c_str(), ".thn") == 0;
    }
}

extern "C" ICOManager* OracleSceneScriptBuildManager()
{
    return &scene_script_build::GManager;
}

// One case per line of scene_script_build.list: "c" (a COM context is given
// to Build) or "-" (none), then buffers in hex, loaded one after the other
// into one module state.
void SceneScriptBuildSuite()
{
    oracle::Suite Out("scene_script_build.jsonl");
    unsigned int Case = 0;
    for (const std::string& Line : oracle::InputLines("scene_script_build.list"))
    {
        scene_script_build::OpenModule();
        const bool bContext = Line.compare(0, 1, "c") == 0;
        unsigned int BufferIndex = 0;
        size_t Start = Line.find(' ');
        while (Start != std::string::npos && Start < Line.size())
        {
            ++Start;
            size_t End = Line.find(' ', Start);
            if (End == std::string::npos)
                End = Line.size();
            const std::string HexText = Line.substr(Start, End - Start);
            const oracle::Bytes Buffer = oracle::Unhex(HexText);
            scene_script_build::RunBuffer(
                Out, oracle::Format("{\"case\":\"%u\",\"buffer\":\"%u\",", Case, BufferIndex), Buffer.data(),
                HexText.size() / 2, bContext);
            ++BufferIndex;
            Start = End;
        }
        scene_script_build::CloseModule();
        ++Case;
    }
}

void RunSceneScriptBuildDataDump(const std::vector<std::string>& Files)
{
    oracle::Suite List(oracle::GOutputDir + "\\thn_build\\files.txt");
    unsigned int Count = 0;
    for (const std::string& File : Files)
    {
        if (!scene_script_build::IsThn(File))
            continue;
        oracle::Bytes Buffer;
        if (!oracle::ReadWholeFile(File, Buffer))
        {
            fprintf(stderr, "thn_build: cannot read %s\n", File.c_str());
            oracle::GFailed = true;
            continue;
        }
        List.Line(File);
        ++Count;
        oracle::Suite Out(oracle::DataOutputFile("thn_build", File));
        scene_script_build::OpenModule();
        scene_script_build::RunBuffer(Out, "{\"file\":\"" + File + "\",", Buffer.data(), Buffer.size(), true);
        scene_script_build::CloseModule();
    }
    printf("built %u scene scripts\n", Count);
}
