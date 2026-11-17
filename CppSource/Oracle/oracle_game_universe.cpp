// Data dumps of the universe, its zones and the base data of Common (work
// package P2.1), written after oracle_game_data.cpp has run the [Data] list.
// crates/game-sim/tests/support/render_universe.rs renders the same lines.

#include "oracle_support.h"
#include "oracle_game.h"

#include <string.h>

#include <list>
#include <map>
#include <string>
#include <vector>

#include "common_ini.h"
#include "common_id.h"
#include "common_universe.h"
#include "common_goods.h"
#include "common_fuses.h"
#include "common_misc.h"
#include "common_archetype.h"
#include "common_casteroid.h"
#include "common_net.h"

using namespace oracle;

namespace
{
    std::string B(bool Value)
    {
        return Value ? "1" : "0";
    }

    std::string I(int Value)
    {
        return Format("%d", Value);
    }

    std::string U(unsigned int Value)
    {
        return Format("%u", Value);
    }

    std::string V3(const Vector& Value)
    {
        return GameF(Value.x) + "," + GameF(Value.y) + "," + GameF(Value.z);
    }

    std::vector<const Universe::ISystem*> Systems()
    {
        std::vector<const Universe::ISystem*> Out;
        for (Universe::ISystem* System = Universe::GetFirstSystem(); System != nullptr;
             System = Universe::GetNextSystem())
            Out.push_back(System);
        return Out;
    }

    void DumpZone(Suite& Out, const Universe::IZone& Z)
    {
        const Matrix& M = Z.GetOrientation();
        std::string Orient;
        for (int Row = 0; Row < 3; ++Row)
        {
            for (int Col = 0; Col < 3; ++Col)
            {
                if (!Orient.empty())
                    Orient += ",";
                Orient += GameF(M.E[Row][Col]);
            }
        }
        FLine L;
        L.Add("zone", GameId(Z.GetId()));
        L.Add("system", GameId(Z.GetSystem()));
        L.Add("orient", Orient);
        L.Add("pos", V3(Z.GetPos()));
        L.Add("shape", U(Z.GetShape()));
        L.Add("flags", GameId((unsigned int)Z.GetProperties()));
        L.Add("size", V3(Z.GetSize()));
        L.Add("physics", GameF(Z.GetEdgeFraction()) + "," + GameF(Z.GetDragModifier()) + "," +
                             GameF(Z.GetDamage()) + "," + GameF(Z.GetInterference()));
        L.Add("music", GameId(Z.MusicId));
        L.Add("dust", GameText(Z.Spacedust) + "," + I(Z.SpacedustMaxParticles));
        L.Add("fog", V3(Z.GetFogColor()));
        L.Add("path_label", B(Z.HasPathLabel()));
        L.Add("visit", U(Z.GetVisit()));
        L.Add("ids", I(Z.GetName()) + "," + I(Z.GetIdsInfo()));
        L.Add("radius", GameF(Z.GetRadius()));
        L.Add("physical", B(Z.HasPhysicalEffect()));
        Out.Line(L.Done());
        const int Count = Z.GetPopulationCount();
        for (int Index = 0; Index < Count; ++Index)
        {
            unsigned int Faction = 0;
            float Weight = 0.0f;
            Z.GetPopulationEntry(Index, Faction, Weight);
            FLine P;
            P.Add("faction", Format("%04x", Faction));
            P.Add("weight", GameF(Weight));
            Out.Line(P.Done());
        }
    }
}

void DumpUniverse(FGameKind& K)
{
    const std::vector<const Universe::ISystem*> All = Systems();
    {
        Suite Out(K.Path("systems"));
        FLine Days;
        Days.Add("days_per_second", Format("%016llx", DoubleBits(DaysPerSecond)));
        Out.Line(Days.Done());
        for (const Universe::ISystem* S : All)
        {
            const std::list<Universe::FSystemZone>* Zones = S->GetZones();
            FLine L;
            L.Add("system", GameId(S->GetId()));
            L.Add("nickname", GameText(S->GetNickname()));
            L.Add("prefix", GameText(S->GetMsgIdPrefix().Data));
            L.Add("file", GameText(S->GetFile()));
            L.Add("strid", I(S->GetStridName()));
            L.Add("ids_info", I(S->GetIdsInfo()));
            L.Add("visit", U(S->GetVisit()));
            L.Add("scale", GameF(S->GetNavMapScale()));
            L.Add("pos", V3(*S->GetPos()));
            L.Add("music", GameId(S->GetSpaceMusic()));
            L.Add("zones", U((unsigned int)Zones->size()));
            L.Add("conns", U((unsigned int)S->GetConnectionCount()));
            Out.Line(L.Done());
            const int Count = S->GetConnectionCount();
            for (int Index = 0; Index < Count; ++Index)
            {
                Universe::FSystemConnection C;
                memset(&C, 0, sizeof(C));
                S->GetConnection((unsigned int)Index, &C);
                FLine M;
                M.Add("conn", GameId(C.GotoSystemId));
                M.Add("object", GameId(C.Object.Id));
                M.Add("object_id", GameId(C.ObjectId));
                M.Add("pos", V3(C.Pos));
                M.Add("visit", U(C.Visit));
                M.Add("gate", U(C.GateType));
                M.Add("gate_system", GameId(Universe::get_gate_system(C.ObjectId)));
                Out.Line(M.Done());
            }
            for (const Universe::FSystemZone& Entry : *Zones)
            {
                FLine M;
                M.Add("zone_ref", GameId(Entry.Zone->GetId()));
                Out.Line(M.Done());
            }
        }
    }
    {
        Suite Out(K.Path("objects"));
        for (const Universe::ISystem* S : All)
        {
            const int Count = S->GetObjectCount();
            for (int Index = 0; Index < Count; ++Index)
            {
                Universe::FSystemObject O;
                memset(&O, 0, sizeof(O));
                if (!S->GetSystemObject((unsigned int)Index, &O))
                    continue;
                FLine L;
                L.Add("object", GameText(O.Nickname.Data));
                L.Add("system", GameId(S->GetId()));
                L.Add("pos", V3(O.Pos));
                L.Add("arch", GameId(O.ArchetypeId));
                L.Add("ids", I(O.IdsName) + "," + I(O.IdsInfo));
                L.Add("rep", GameText(O.Reputation.Data));
                L.Add("dock_with", GameId(O.DockWith));
                L.Add("goto", GameId(O.Goto));
                L.Add("base", GameId(O.BaseId));
                L.Add("next", GameText(O.NextRing.Data));
                L.Add("prev", GameText(O.PrevRing.Data));
                L.Add("visit", U(O.Visit));
                Out.Line(L.Done());
            }
        }
    }
    {
        Suite Out(K.Path("bases"));
        for (Universe::IBase* Base = Universe::GetFirstBase(); Base != nullptr; Base = Universe::GetNextBase())
        {
            std::string Ships = GameId(Base->GetShipSml01()) + "," + GameId(Base->GetShipSml02()) + "," +
                                GameId(Base->GetShipSml03()) + "," + GameId(Base->GetShipMdm01()) + "," +
                                GameId(Base->GetShipMdm02()) + "," + GameId(Base->GetShipMdm03()) + "," +
                                GameId(Base->GetShipLrg01()) + "," + GameId(Base->GetShipLrg02()) + "," +
                                GameId(Base->GetShipLrg03());
            std::string Terrain = GameId(Base->GetTerrainTiny()) + "," + GameId(Base->GetTerrainSml()) + "," +
                                  GameId(Base->GetTerrainMdm()) + "," + GameId(Base->GetTerrainLrg()) + "," +
                                  GameId(Base->GetTerrainDyna01()) + "," + GameId(Base->GetTerrainDyna02());
            FLine L;
            L.Add("base", GameId(Base->GetId()));
            L.Add("nickname", GameText(Base->GetNickname()));
            L.Add("strid", I(Base->GetStridName()));
            L.Add("file", GameText(Base->GetFile()));
            L.Add("system", GameId(Base->GetSystem()));
            L.Add("bgcs", GameText(Base->GetBgcsBaseRunBy()));
            L.Add("space_object", GameId(Base->GetSpaceObjectId()) + "," + U(Base->GetSpaceObjectVisit()));
            L.Add("ships", Ships);
            L.Add("terrain", Terrain);
            L.Add("autosave", B(Base->IsAutosaveForbidden()));
            Out.Line(L.Done());
        }
    }
    {
        Suite Out(K.Path("zones"));
        for (const Universe::ISystem* S : All)
        {
            for (const Universe::FSystemZone& Entry : *S->GetZones())
                DumpZone(Out, *Entry.Zone);
        }
    }
}

void DumpBaseData(FGameKind& K)
{
    Suite Out(K.Path("base_data"));
    BaseDataList* List = BaseDataList_get();
    if (List == nullptr)
        return;
    for (const BaseData* D : *List->get_const_base_data_list())
    {
        FLine L;
        L.Add("base_data", GameId(D->BaseId));
        L.Add("start", GameId(D->StartRoomId));
        L.Add("variance", GameF(D->PriceVariance));
        L.Add("repair", GameF(D->ShipRepairCost));
        L.Add("rooms", U((unsigned int)D->Rooms.size()));
        L.Add("market", U((unsigned int)D->Market.size()));
        Out.Line(L.Done());
        for (const RoomData* Room : D->Rooms)
        {
            FLine R;
            R.Add("room", GameId(Room->RoomId));
            R.Add("base", GameId(Room->BaseId));
            R.Add("nickname", GameText(Room->Nickname));
            R.Add("file", GameText(Room->File));
            Out.Line(R.Done());
        }
        for (const std::pair<const unsigned int, MarketGoodInfo>& Entry : D->Market)
        {
            const MarketGoodInfo& M = Entry.second;
            FLine G;
            G.Add("good", GameId(M.GoodId));
            G.Add("price", GameF(M.Price));
            G.Add("inventory", I(M.MinInventory) + "," + I(M.MaxInventory));
            G.Add("type", I((int)M.Type));
            G.Add("level", GameF(M.MinLevel));
            G.Add("rep", GameF(M.MinRep));
            Out.Line(G.Done());
        }
    }
}

namespace
{
    // FuseDB keeps its list protected; a derived type may read it.
    struct FFuseFiles : FuseDB
    {
        static const std::list<TString<260>>& Files()
        {
            return m_FuseINIFiles;
        }
    };

    // The data root as GetFileSysPath prefixes it.
    std::string DataPrefix()
    {
        char Path[0x104];
        GetFileSysPath(Path, "x", nullptr);
        std::string Prefix = Path;
        return Prefix.substr(0, Prefix.size() - 1);
    }
}

void DumpFuses(FGameKind& K, const std::set<unsigned int> (&PetalIds)[7])
{
    {
        Suite Out(K.Path("fuse_files"));
        const std::string Prefix = DataPrefix();
        for (const TString<260>& File : FFuseFiles::Files())
        {
            std::string Path = File.Data;
            if (Path.size() >= Prefix.size() && _strnicmp(Path.c_str(), Prefix.c_str(), Prefix.size()) == 0)
                Path = Path.substr(Prefix.size());
            FLine L;
            L.Add("fuse_file", GameText(Path.c_str()));
            Out.Line(L.Done());
        }
    }
    {
        Suite Out(K.Path("petals"));
        const PetalInterfaceDatabase* Db = PetalDB_get();
        for (int Category = 0; Category < 7; ++Category)
        {
            for (unsigned int Id : PetalIds[Category])
            {
                const PetalInterfaceDatabase::petal_record* Record = Db->find_record(Category, Id);
                if (Record == nullptr)
                    continue;
                std::string Extras;
                for (const char* Extra : Record->Extras)
                {
                    if (!Extras.empty())
                        Extras += ";";
                    Extras += GameText(Extra);
                }
                FLine L;
                L.Add("category", Format("%d", Category));
                L.Add("petal", GameId(Record->NicknameId));
                L.Add("mesh", GameText(Record->Mesh.c_str()));
                L.Add("extras", Extras);
                Out.Line(L.Done());
            }
        }
    }
}

// --- Asteroid field files ------------------------------------------------------

namespace
{
    // AsteroidExclusionZoneMgr keeps its zone vector private; this view has
    // the same layout (pinned by the size) and is only read.
    struct FExclusionMgrView
    {
        void* Vtable;
        unsigned char ZonesAllocator;
        Exclusion::AsteroidExclusionZone** Begin;
        Exclusion::AsteroidExclusionZone** End;
        Exclusion::AsteroidExclusionZone** CapacityEnd;
        float MaxCliDist;
        float MaxDynDist;
    };
    static_assert(sizeof(FExclusionMgrView) == sizeof(Exclusion::AsteroidExclusionZoneMgr),
                  "AsteroidExclusionZoneMgr layout");

    // SrvAsteroid_CopyBounded: at most Size - 1 characters.
    std::string Bounded(const char* Text, size_t Size)
    {
        std::string Out = Text != nullptr ? Text : "";
        if (Out.size() > Size - 1)
            Out.resize(Size - 1);
        return Out;
    }

    void DumpField(Suite& Out, const CmnAsteroid::CAsteroidField& F, const std::string& File, bool bLoaded,
                   const std::map<const Archetype::Asteroid*, unsigned int>& ArchIds)
    {
        {
            FLine L;
            L.Add("field", GameText(File.c_str()));
            L.Add("system", GameId(F.SystemId));
            L.Add("zone", GameId(F.Zone != nullptr ? F.Zone->GetId() : 0u));
            L.Add("loaded", B(bLoaded));
            L.Add("cube_size", I(F.CubeSize));
            L.Add("arch_cube_size", I(F.ArchCubeSize));
            L.Add("fill_dist", I(F.FillDist));
            L.Add("empty_freq", GameF(F.EmptyFreq));
            Out.Line(L.Done());
        }
        {
            std::string Rotations;
            for (int Axis = 0; Axis < 3; ++Axis)
            {
                for (int Index = 0; Index < 4; ++Index)
                {
                    if (!Rotations.empty())
                        Rotations += ",";
                    Rotations += GameF(F.Rotations.Angles[Axis][Index]);
                }
            }
            FLine L;
            L.Add("rotations", Rotations);
            Out.Line(L.Done());
        }
        for (const CmnAsteroid::AsteroidCubeArchEntry& A : F.ArchList)
        {
            std::string Orient;
            for (int Row = 0; Row < 3; ++Row)
            {
                for (int Col = 0; Col < 3; ++Col)
                {
                    if (!Orient.empty())
                        Orient += ",";
                    Orient += GameF(A.Orient.E[Row][Col]);
                }
            }
            std::map<const Archetype::Asteroid*, unsigned int>::const_iterator Id = ArchIds.find(A.Arch);
            FLine L;
            L.Add("arch", GameId(Id != ArchIds.end() ? Id->second : 0u));
            L.Add("extent", V3(A.Extent));
            L.Add("orient", Orient);
            L.Add("mine", B(A.bMine));
            L.Add("instant_kill", B(A.bInstantKill));
            Out.Line(L.Done());
        }
        if (F.ExclusionMgr != nullptr)
        {
            const FExclusionMgrView* View = reinterpret_cast<const FExclusionMgrView*>(F.ExclusionMgr);
            for (Exclusion::AsteroidExclusionZone** It = View->Begin; It != View->End; ++It)
            {
                const Exclusion::AsteroidExclusionZone* Z = *It;
                FLine L;
                L.Add("exclusion", GameId(Z->ZonePtr != nullptr ? Z->ZonePtr->GetId() : 0u));
                L.Add("billboards", B(Z->bExcludeBillboards != 0));
                L.Add("dynamic", B(Z->bExcludeDynamicAsteroids != 0));
                L.Add("empty", GameF(Z->EmptyCubeFrequency));
                L.Add("count", GameF(Z->BillboardCount));
                L.Add("dist", GameF(Z->MaxDistSq));
                Out.Line(L.Done());
            }
        }
        std::string Loot;
        for (const Universe::IZone* Z : F.LootZones)
        {
            if (!Loot.empty())
                Loot += ";";
            Loot += GameId(Z->GetId());
        }
        FLine L;
        L.Add("loot_zones", Loot);
        Out.Line(L.Done());
    }
}

// Kind asteroid_fields (render_field.rs): every [Asteroids] section with a
// file and a zone of every system file, as SrvAsteroidSystem::load walks
// them, loaded through CAsteroidField::load after the [Field] pass of
// FSrvAsteroidField::load. A field whose zone is missing is not loaded (the
// server faults there). Root::load would load each rock's model through the
// engine, which the harness does not start; every asteroid archetype is
// therefore marked as loaded first (EngineIndex 0), so that Root::load only
// counts the load. The archetype dumps have already been written.
void DumpAsteroidFields(FGameKind& K, const std::string& UniversePath)
{
    std::map<const Archetype::Asteroid*, unsigned int> ArchIds;
    const unsigned int Count = Archetype::GetIdCount(Archetype::AClassAsteroid);
    for (unsigned int Index = 0; Index < Count; ++Index)
    {
        const unsigned int Id = Archetype::GetId(Archetype::AClassAsteroid, Index);
        Archetype::Asteroid* Arch = Archetype::GetAsteroid(Id);
        if (Arch == nullptr)
            continue;
        Arch->EngineIndex = 0;
        ArchIds[Arch] = Id;
    }
    std::string Dir = UniversePath;
    const size_t Slash = Dir.find_last_of("\\/");
    Dir = Slash == std::string::npos ? std::string() : Dir.substr(0, Slash + 1);

    std::vector<CmnAsteroid::CAsteroidField*> Fields;
    {
        Suite Out(K.Path("fields"));
        for (const Universe::ISystem* S : Systems())
        {
            char SystemPath[0x104];
            GetFileSysPath(SystemPath, (Dir + S->GetFile()).c_str(), nullptr);
            INI_Reader Reader;
            if (!Reader.open(SystemPath, false))
                continue;
            while (Reader.read_header())
            {
                if (!Reader.is_header("Asteroids"))
                    continue;
                std::string File;
                std::string ZoneName;
                while (Reader.read_value())
                {
                    if (Reader.is_value("file"))
                        File = Bounded(Reader.get_value_string(), 0x100);
                    else if (Reader.is_value("zone"))
                        ZoneName = Bounded(Reader.get_value_string(), 0x40);
                }
                if (File.empty() || ZoneName.empty())
                    continue;
                CmnAsteroid::CAsteroidField* Field = new CmnAsteroid::CAsteroidField();
                Fields.push_back(Field);
                Field->set_system(S->GetId());
                const Universe::IZone* Zone = Universe::get_zone(S->GetId(), ZoneName.c_str());
                Field->set_zone(Zone);
                bool bLoaded = false;
                char FieldPath[0x104];
                GetFileSysPath(FieldPath, File.c_str(), nullptr);
                INI_Reader FieldReader;
                if (Zone != nullptr && FieldReader.open(FieldPath, false))
                {
                    INI_Reader::State Start;
                    FieldReader.get_state(Start);
                    int CubeSize = Field->CubeSize;
                    int FillDist = 0;
                    int EmptyFreq = 0;
                    while (FieldReader.read_header())
                    {
                        if (!FieldReader.is_header("Field"))
                            continue;
                        while (FieldReader.read_value())
                        {
                            if (FieldReader.is_value("cube_size"))
                                CubeSize = (int)(long long)FieldReader.get_value_float(0);
                            else if (FieldReader.is_value("fill_dist"))
                                FillDist = (int)(long long)FieldReader.get_value_float(0);
                            else if (FieldReader.is_value("empty_cube_frequency"))
                                EmptyFreq = (int)(long long)FieldReader.get_value_float(0);
                        }
                    }
                    FieldReader.set_state(Start);
                    Field->load(FieldReader, CubeSize);
                    Field->FillDist = FillDist;
                    Field->EmptyFreq = (float)EmptyFreq;
                    bLoaded = true;
                }
                DumpField(Out, *Field, File, bLoaded, ArchIds);
            }
        }
    }
    {
        Suite Out(K.Path("loot"));
        for (const Universe::ISystem* S : Systems())
        {
            for (const Universe::FSystemZone& Entry : *S->GetZones())
            {
                const CmnAsteroid::LootableZoneEntry* E = Entry.Zone->GetLootableInfo();
                if (E == nullptr)
                    continue;
                FLine L;
                L.Add("loot_zone", GameId(Entry.Zone->GetId()));
                L.Add("container", GameId(E->AsteroidLootContainerId.Id) + "," + GameId(E->DynamicLootContainerId.Id));
                L.Add("commodity", GameId(E->AsteroidLootCommodityId.Id) + "," + GameId(E->DynamicLootCommodityId.Id));
                L.Add("count", I(E->AsteroidLootCountMin) + "," + I(E->AsteroidLootCountMax) + "," +
                                   I(E->DynamicLootCountMin) + "," + I(E->DynamicLootCountMax));
                L.Add("difficulty", I(E->AsteroidLootDifficulty) + "," + I(E->DynamicLootDifficulty));
                Out.Line(L.Done());
            }
        }
    }
    // The fields are not deleted: their rock entries release the asteroid
    // archetypes, and the last release would hand the made-up engine index
    // to NeuronRuntime::Engine, which the harness does not start.
    (void)Fields;
}
