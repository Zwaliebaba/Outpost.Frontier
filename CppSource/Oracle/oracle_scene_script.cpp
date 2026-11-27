// P4.14: the Lua state after a scene script, through the reference's own
// copy of Lua 3.2 (CppSource/SceneScript/lua32, compiled into Oracle.exe).
//
// A case opens a fresh Lua state as SceneScriptLuaOpen does (base, math and
// I/O libraries), installs an error method and the numbers
// RegisterSceneScriptLuaConstants sets, then runs each buffer through
// lua_dobuffer and dumps the state: the globals whose value differs from
// the one they had before any buffer ran, then every table they reach. The
// Rust side is crates/neuron-client/tests/support/scene_script.rs.
//
// Line format, per buffer:
//   {"case":"<n>","buffer":"<m>","status":"<lua_dobuffer result>"}   (suite)
//   {"file":"<path>","status":"<result>"}                             (data)
//   {"global":"<hex name>","value":"<value>"}   for each changed global, by name
//   {"table":"<id>","size":"<nhash>","entries":"<k> <v> ..."}   for each table, by id
// A value is nil, n<16 hex digits of the double>, s<hex bytes>, t<id>,
// function or userdata. Tables are numbered in the order the dump first
// meets them. Entries are in node order when every key is a number;
// otherwise numbers come first by value, then strings by bytes, then any
// other key in node order (strings hash by address, divergence D200).

#include <windows.h>

#include <stdio.h>
#include <string.h>

#include <algorithm>
#include <map>
#include <string>
#include <vector>

#include "oracle_support.h"

// The Lua internals come last: lobject.h, lstate.h and ltable.h define
// macros (L, ref, val, node, nvalue, ...) that must not reach the headers
// above.
extern "C"
{
#include "lua.h"
#include "lualib.h"
#include "lobject.h"
#include "lstate.h"
#include "lstring.h"
#include "ltable.h"
}

using namespace oracle;

namespace
{
    // RegisterSceneScriptLuaConstants (CppSource/SceneScript/SceneScriptLua.cpp),
    // in its order. crates/neuron-client/tests/scene_script_constants.rs checks
    // this copy and the Rust table against that function's source.
    struct FConstant
    {
        const char* Name;
        double Value;
    };

    const FConstant Constants[] = {
        {"UNKNOWN_ENTITY", 0}, {"COMPOUND", 1}, {"DEFORMABLE", 2}, {"CAMERA", 3}, {"MONITOR", 4},
        {"LIGHT", 5}, {"SOUND", 6}, {"MARKER", 7}, {"HARDPOINT", 8}, {"SCENE", 9},
        {"SUB_SCENE", 10}, {"MOTION_PATH", 11}, {"DELETED", 12}, {"PSYS", 13},
        {"UNDEFINED_EVENT", 0}, {"START_MOTION", 10}, {"START_SOUND", 3}, {"START_IK", 11},
        {"START_PATH_ANIMATION", 6}, {"ATTACH_ENTITY", 8}, {"START_LIGHT_PROP_ANIM", 4},
        {"START_CAMERA_PROP_ANIM", 5}, {"START_SPATIAL_PROP_ANIM", 7}, {"SET_CAMERA", 2},
        {"CONNECT_HARDPOINTS", 9}, {"START_SUB_SCENE", 12}, {"USER_EVENT", 1}, {"START_PSYS", 13},
        {"START_PSYS_PROP_ANIM", 14}, {"START_AUDIO_PROP_ANIM", 15}, {"START_FOG_PROP_ANIM", 16},
        {"START_REVERB_PROP_ANIM", 17}, {"START_FLR_HEIGHT_ANIM", 18}, {"SUBTITLE", 19},
        {"X_AXIS", 0}, {"Y_AXIS", 1}, {"Z_AXIS", 2}, {"NEG_X_AXIS", 3}, {"NEG_Y_AXIS", 4},
        {"NEG_Z_AXIS", 5},
        {"REFERENCE", 1}, {"HIDDEN", 16}, {"FOG_PROPS_REMOVED", 32}, {"SPATIAL", 2}, {"STREAM", 4},
        {"LOOP", 8}, {"LIT_AMBIENT", 2}, {"LIT_DYNAMIC", 4}, {"USE_SCRIPT_DURATION", 1},
        {"POSITION", 2}, {"PATH_POSITION", 2}, {"ORIENTATION", 4}, {"LOOK_AT", 8},
        {"PARENT_CHILD", 64}, {"ENTITY_RELATIVE", 32}, {"ORIENTATION_RELATIVE", 128}, {"ROOT", 0},
        {"PART", 2}, {"HARDPOINT", 1}, {"ADD_PATH", 6}, {"LOOKAT_ENTITY", 8}, {"STOP", 21},
        {"STOP_IK", 22}, {"START", 23}, {"PROPERTY_ANIM", 0}, {"F_NONE", 0}, {"F_EXP", 1},
        {"F_EXP2", 2}, {"F_LINEAR", 3}, {"L_POINT", 1}, {"L_SPOT", 2}, {"L_DIRECT", 3}, {"Y", 1},
        {"N", 0},
    };

    // The parser installs an error method that only reports; this one is silent.
    void QuietErrorMethod()
    {
    }

    typedef std::map<std::string, TObject> FGlobals;

    std::string NameOf(TaggedString* Ts)
    {
        return std::string(Ts->str, (size_t)Ts->u.s.len);
    }

    // Every global with a non-nil value.
    FGlobals CurrentGlobals()
    {
        FGlobals Out;
        for (GCnode* Node = lua_state->rootglobal.next; Node != nullptr; Node = Node->next)
        {
            TaggedString* Ts = (TaggedString*)Node;
            if (ttype(&Ts->u.s.globalval) != LUA_T_NIL)
                Out[NameOf(Ts)] = Ts->u.s.globalval;
        }
        return Out;
    }

    FGlobals GInitial;

    bool OpenState()
    {
        lua_open();
        if (lua_state == nullptr)
            return false;
        lua_mathlibopen();
        lua_iolibopen();
        // Host calls run inside a block, so the objects they leave on the
        // Lua stack are released.
        lua_beginblock();
        lua_pushcfunction(&QuietErrorMethod);
        lua_seterrormethod();
        for (const FConstant& Constant : Constants)
        {
            lua_pushnumber(Constant.Value);
            lua_setglobal(const_cast<char*>(Constant.Name));
        }
        lua_endblock();
        GInitial = CurrentGlobals();
        return true;
    }

    void CloseState()
    {
        lua_close();
        GInitial.clear();
    }

    class FDumper
    {
    public:
        explicit FDumper(Suite& Out) : Out_(Out) {}

        void Dump()
        {
            const FGlobals Now = CurrentGlobals();
            std::vector<std::string> Names;
            for (const auto& Entry : Now)
                Names.push_back(Entry.first);
            for (const auto& Entry : GInitial)
                Names.push_back(Entry.first);
            std::sort(Names.begin(), Names.end());
            Names.erase(std::unique(Names.begin(), Names.end()), Names.end());
            TObject Nil;
            ttype(&Nil) = LUA_T_NIL;
            for (const std::string& Name : Names)
            {
                const FGlobals::const_iterator A = Now.find(Name);
                const FGlobals::const_iterator B = GInitial.find(Name);
                TObject Current = A != Now.end() ? A->second : Nil;
                TObject Before = B != GInitial.end() ? B->second : Nil;
                if (luaO_equalObj(&Current, &Before))
                    continue;
                Out_.Line("{\"global\":\"" + Hex(Name.data(), Name.size()) + "\",\"value\":\"" +
                          Encode(&Current) + "\"}");
            }
            for (size_t Index = 0; Index < Queue_.size(); ++Index)
                DumpTable(Index, Queue_[Index]);
        }

    private:
        std::string Encode(TObject* Object)
        {
            switch (ttype(Object))
            {
            case LUA_T_NIL:
                return "nil";
            case LUA_T_NUMBER:
                return Format("n%016llx", DoubleBits(nvalue(Object)));
            case LUA_T_STRING:
                return "s" + Hex(svalue(Object), (size_t)tsvalue(Object)->u.s.len);
            case LUA_T_ARRAY:
            {
                Hash* Table = avalue(Object);
                std::map<Hash*, size_t>::const_iterator Found = Ids_.find(Table);
                if (Found != Ids_.end())
                    return Format("t%u", (unsigned int)Found->second);
                const size_t Id = Queue_.size();
                Ids_[Table] = Id;
                Queue_.push_back(Table);
                return Format("t%u", (unsigned int)Id);
            }
            case LUA_T_USERDATA:
                return "userdata";
            default:
                return "function";
            }
        }

        static int KeyClass(TObject* Key)
        {
            switch (ttype(Key))
            {
            case LUA_T_NUMBER:
                return 0;
            case LUA_T_STRING:
                return 1;
            default:
                return 2;
            }
        }

        void DumpTable(size_t Id, Hash* Table)
        {
            std::vector<Node*> Entries;
            bool bNumbersOnly = true;
            for (int Index = 0; Index < Table->nhash; ++Index)
            {
                Node* Entry = &Table->node[Index];
                if (ttype(&Entry->val) == LUA_T_NIL)
                    continue;
                Entries.push_back(Entry);
                if (ttype(&Entry->ref) != LUA_T_NUMBER)
                    bNumbersOnly = false;
            }
            if (!bNumbersOnly)
            {
                std::stable_sort(Entries.begin(), Entries.end(), [](Node* A, Node* B) {
                    const int Ka = KeyClass(&A->ref);
                    const int Kb = KeyClass(&B->ref);
                    if (Ka != Kb)
                        return Ka < Kb;
                    if (Ka == 0)
                        return nvalue(&A->ref) < nvalue(&B->ref);
                    if (Ka == 1)
                    {
                        const std::string Sa(svalue(&A->ref), (size_t)tsvalue(&A->ref)->u.s.len);
                        const std::string Sb(svalue(&B->ref), (size_t)tsvalue(&B->ref)->u.s.len);
                        return Sa < Sb;
                    }
                    return false;
                });
            }
            std::string Text;
            for (Node* Entry : Entries)
            {
                if (!Text.empty())
                    Text += " ";
                Text += Encode(&Entry->ref);
                Text += " ";
                Text += Encode(&Entry->val);
            }
            Out_.Line(Format("{\"table\":\"%u\",\"size\":\"%d\",\"entries\":\"", (unsigned int)Id,
                             Table->nhash) +
                      Text + "\"}");
        }

        Suite& Out_;
        std::map<Hash*, size_t> Ids_;
        std::vector<Hash*> Queue_;
    };

    // lua_dobuffer's result, "1" for an empty buffer (Load rejects those
    // before Lua sees them).
    int RunBuffer(const Bytes& Buffer, size_t Size)
    {
        if (Size == 0)
            return 1;
        lua_beginblock();
        const int Status = lua_dobuffer((char*)Buffer.data(), (int)Size, const_cast<char*>("oracle"));
        lua_endblock();
        return Status;
    }

    void SceneScriptSuite()
    {
        Suite Out("scene_script.jsonl");
        unsigned int Case = 0;
        for (const std::string& Line : InputLines("scene_script.list"))
        {
            if (!OpenState())
            {
                fprintf(stderr, "scene_script: lua_open failed\n");
                GFailed = true;
                return;
            }
            unsigned int BufferIndex = 0;
            size_t Start = 0;
            while (Start <= Line.size())
            {
                size_t End = Line.find(' ', Start);
                if (End == std::string::npos)
                    End = Line.size();
                const std::string HexText = Line.substr(Start, End - Start);
                const Bytes Buffer = Unhex(HexText);
                const int Status = RunBuffer(Buffer, HexText.size() / 2);
                Out.Line(Format("{\"case\":\"%u\",\"buffer\":\"%u\",\"status\":\"%d\"}", Case,
                                BufferIndex, Status));
                FDumper(Out).Dump();
                ++BufferIndex;
                Start = End + 1;
            }
            CloseState();
            ++Case;
        }
    }

    bool IsThn(const std::string& File)
    {
        if (File.compare(0, 5, "DATA/") != 0 || File.size() < 4)
            return false;
        const std::string Tail = File.substr(File.size() - 4);
        return _stricmp(Tail.c_str(), ".thn") == 0;
    }
}

void RunNeuronClientSuites()
{
    SceneScriptSuite();
    RunTextSuites();
    RunAudioSuites();
    SceneScriptBuildSuite();
    RunTextureSuites();
    SceneScriptRuntimeSuites();
    SceneScriptSaveSuite();
    RunUiSuites();
    RunParticleSuites();
    RunMaterialSuites();
    RunBeamSuite();
    RunBeamWalkSuite();
}

void RunSceneScriptDataDump(const std::vector<std::string>& Files)
{
    Suite List(GOutputDir + "\\thn\\files.txt");
    unsigned int Count = 0;
    for (const std::string& File : Files)
    {
        if (!IsThn(File))
            continue;
        Bytes Buffer;
        if (!ReadWholeFile(File, Buffer))
        {
            fprintf(stderr, "thn: cannot read %s\n", File.c_str());
            GFailed = true;
            continue;
        }
        const size_t Size = Buffer.size();
        Buffer.push_back(0);
        List.Line(File);
        ++Count;
        Suite Out(DataOutputFile("thn", File));
        if (!OpenState())
        {
            fprintf(stderr, "thn: lua_open failed\n");
            GFailed = true;
            return;
        }
        const int Status = RunBuffer(Buffer, Size);
        Out.Line(Format("{\"file\":\"%s\",\"status\":\"%d\"}", File.c_str(), Status));
        FDumper(Out).Dump();
        CloseState();
    }
    printf("dumped %u scene scripts\n", Count);
}
