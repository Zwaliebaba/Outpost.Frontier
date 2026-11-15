// Author: Giperion.

#include "cli_asteroid_dynamic.h"

#include "NeuronRuntime.h"
#include "cli_asteroid_med.h"       // CliAsteroidSlot1bcBody (the branch)
#include "cli_projectile_fx.h"      // CliEffectListRemove (the routine)
#include "client_cliobj.h"          // CliDynamicAsteroidHasParticleEffect / ...ParticleEffect
#include "client_explosion.h"       // Action_Explode (the routine)
#include "client_objdb.h"           // ClientObjDb_RegisterGameobj (the routine)
#include "common_archetype.h"
#include "common_misc.h"            // random_on_unit_sphere, ICamera, SUBOBJ_ID_NONE
#include "common_physys.h"
#include "common_server.h"          // IServer::MineAsteroid (slot 62)
#include "common_universe.h"        // Archetype::GetDynamicAsteroidID
#include "neuroncomponents.h"                  // FDUMP
#include "neuroncomponents_iids.h"
#include "engine/sceneruntime.h"         // IEngine::QueryEngineComponent
#include "engine_light.h"           // FGameLight, GameLightRelease (the routine)
#include "game_options.h"           // GetGameOption (the routine)
#include "particle_effect.h"        // FParticleEffect, CliCreateEffectOneShot (the routine)
#include "player_behavior.h"        // GetPlayerObjRW (the routine)
#include "player_spawn.h"           // GetPlayerCharacterId / GetPlayerConnectionId
#include "render/IMaterial.h"
#include "render/IRenderPipeline.h"
#include "render/mesh_iface.h"      // IMesh
#include "render/rendcomp.h"        // IRenderableMesh, IRenderer2
#include "server_connect.h"         // GetLocalServerInterface (the field)
#include "soundmanager.h"           // ISoundSource
#include "space_camera.h"           // AsCamera
#include "space_scene_obj_draw.h"   // GameobjSweepEffectListbaseline (the routine)
#include "star_sys.h"               // StarSysGetActiveSystemId (the routine)
#include "vis_effect_arch.h"        // FindEffectArch (the routine)
#include "x86math.h"                // I3DMathEngine::MulTransform, FTransform

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <float.h>

namespace
{
    // CliAsteroid.cpp's FDUMP file argument.
    char const kCliAsteroidSourceFile[] = "Client\\Space\\CliAsteroid.cpp";

    // 1/32768, the rand() scale.
    constexpr float kRandScale = 3.0517578125e-05f;
    // 1/16384 (twice the rand() scale).
    constexpr float kRandScale2 = 6.103515625e-05f;
    // the alpha a dynamic asteroid must exceed to count as visible / be drawn.
    constexpr float kVisibleAlpha = 0.005f;
    // the zone immersion the spawn point must exceed.
    constexpr float kSpawnImmersion = 0.001f;
    // the recycle radius factor (1.05) on the squared placement radius.
    constexpr float kFarFactor = 1.05f;
    // (.data, 0.35, read only at 0x00525e4a): the fade band on the squared
    // placement radius.
    constexpr float kFadeFactor = 0.35f;

    // LIGHTING (0x89) on, SPECULARENABLE (0x1d) off, ALPHABLENDENABLE (0x1b) off
    // (the opaque draw state).
    unsigned long const kRsDynOpaque[] = {0x89, 1, 0x1d, 0, 0x1b, 0, 0};
    // the same with ALPHABLENDENABLE on, SRCBLEND (0x13) SRCALPHA (5), DESTBLEND
    // (0x14) INVSRCALPHA (6).
    unsigned long const kRsDynBlend[] = {0x89, 1, 0x1d, 0, 0x1b, 1, 0x13, 5, 0x14, 6, 0};

    // The render-state table loop the routine inlines three times (0x00526290, 0x00526500,
    // , 0x00526624): the first state is the table's first dword, then value / next
    // state pairs until a zero state.
    void ApplyRsTable(IRenderPipeline* Pipe, unsigned long const* Table)
    {
        unsigned long State = Table[0];
        do
        {
            Pipe->SetRenderState((D3DRENDERSTATETYPE)State, static_cast<int>(Table[1]));
            State = Table[2];
            Table += 2;
        } while (State != 0);
    }

    IServer* LocalServer()
    {
        return static_cast<IServer*>(GetLocalServerInterface());
    }

    // The particle's vt+0xc (SetIntensity), __thiscall with one float.
    void ParticleSetIntensity(FParticleEffect* Particle, float Intensity)
    {
        typedef void(__thiscall * FSetIntensity)(FParticleEffect*, float);
        Particle->SetIntensity(Intensity);
    }
}

// ===========================================================================
// DynamicAsteroid (the gameobj)
// ===========================================================================

FCliDynamicAsteroidGameobj::FCliDynamicAsteroidGameobj(FCliDynamicAsteroid* Owner)
    : FClientGameobj(EClientGameobjKind::DynamicAsteroid)
{
    // after the routine(9). Alpha (+0xb0) is not written.
    SetOwner(Owner);
    Companion[0] = nullptr;
    Companion[1] = nullptr;
    Companion[2] = nullptr;
    Companion[3] = nullptr;
    bActive = 0;
    bPlaced = 0;
    Object = nullptr;
    Nickname.Id = 0;
}

FCliDynamicAsteroidGameobj::~FCliDynamicAsteroidGameobj()
{
    // (this = the CBase sub-object, so +0xb4 / +0xb0 / +0xb8 / +0xac there are
    // the gameobj's +0xc0 / +0xbc / +0xc4 / +0xb8).
    if (Companion[2] != nullptr)
    {
        ParticleEffectRelease(static_cast<FParticleEffect*>(Companion[2]));
        Companion[2] = nullptr;
    }
    if (Companion[1] != nullptr)
    {
        static_cast<ISoundSource*>(Companion[1])->Release();                  // vt+8
        Companion[1] = nullptr;
    }
    if (Companion[3] != nullptr)
    {
        GameLightRelease(static_cast<FGameLight*>(Companion[3]));
        Companion[3] = nullptr;
    }
    Companion[0] = nullptr;
    // GameObject's the routine (the base destructor).
}

int FCliDynamicAsteroidGameobj::get_type(uint& OutType) const
{
    // .
    OutType = 0x20000000u;
    return 0;
}

void FCliDynamicAsteroidGameobj::create(Archetype::Root* Arch)
{
    // (RET 4).
    GameobjCreateOfClass(this, CObject::ClassDynamicAsteroid, Arch);
    Object->open(Arch);                                                       // vt+0x4c
    Nickname = Arch->Nickname;                                                // Arch +0x08
}

bool FCliDynamicAsteroidGameobj::find(unsigned const& Id)
{
    // XOR AL,AL / RET 4.
    (void)Id;
    return false;
}

void FCliDynamicAsteroidGameobj::Slot194(void const* A, void const* B, FClientGameobj* Other,
                                         void const* D)
{
    // (RET 0x10). B is not read.
    (void)B;
    // Other's slot 104 (+0x1a0) with (A, D, this, SUBOBJ_ID_NONE's
    // word zero-extended). Unlike GameObject's slot 101 there is no own slot 107 afterwards.
    Other->Slot1a0(A, D, this,
                   reinterpret_cast<void const*>(static_cast<uintptr_t>(SUBOBJ_ID_NONE)));

    // an active asteroid, Other's object narrowed by class & 7.
    if (bActive == 0)
        return;
    CObject* const OtherObj = Other->GetObject();
    if (OtherObj == nullptr || (static_cast<unsigned>(OtherObj->ObjectClass) & 7u) != 7u)
        return;

    // own slot 108 at A.
    Slot1b0(static_cast<Vector const*>(A));

    // the shot's owner (+0xe4) must be the player's character
    // (the field), and there must be an owner manager (+0xa8).
    if (static_cast<CProjectile const*>(OtherObj)->OwnerId != GetPlayerCharacterId())
        return;
    FCliDynamicAsteroid* const Owner = GetOwner();
    if (Owner == nullptr)
        return;

    // the manager's common field (+4, no null test) at the camera
    // position the draw last stored (+0x28): its lootable zone and that zone's loot record
    // (vt+0x7c).
    Universe::IZone const* const Zone =
        Owner->CmnField->get_lootable_zone(*reinterpret_cast<Vector const*>(Owner->CameraPos));
    if (Zone == nullptr)
        return;
    CmnAsteroid::LootableZoneEntry const* const Loot =
        static_cast<CmnAsteroid::LootableZoneEntry const*>(Zone->GetLootableInfo());
    if (Loot == nullptr)
        return;

    // rand() / 32768 against 1.0 / dynamic_loot_difficulty (+0x24):
    // FCOMPP of the quotient with the roll, TEST AH,0x41 / JNZ -- on only while the quotient is
    // strictly greater.
    double const Roll = static_cast<double>(rand()) * kRandScale;
    double const Chance = 1.0 / static_cast<double>(Loot->DynamicLootDifficulty);
    if (!(Chance > Roll))
        return;

    // count = ftol(rand() * InvDetailScale * (max - min) / 32768 +
    // min + 0.5); min and max - min are stored as floats first.
    float const Min = static_cast<float>(Loot->DynamicLootCountMin);
    float const InvDetail = Owner->InvDetailScale;
    float const Range = static_cast<float>(static_cast<double>(Loot->DynamicLootCountMax) - Min);
    double const Scaled =
        static_cast<double>(rand()) * InvDetail * Range * kRandScale + Min + 0.5f;
    unsigned const Quantity = static_cast<unsigned>(static_cast<int>(Scaled));

    // the field slot 62 (+0xf8) MineAsteroid(the active system,
    // get_position(), dynamic_loot_container (+4), dynamic_loot_commodity (+0xc), count,
    // ). get_position runs before the routine.
    Vector const& Pos = get_position();
    unsigned const SystemId = StarSysGetActiveSystemId();
    LocalServer()->MineAsteroid(SystemId, Pos, Loot->DynamicLootContainerId.Id,
                                Loot->DynamicLootCommodityId.Id, Quantity,
                                GetPlayerConnectionId());
}

void FCliDynamicAsteroidGameobj::Slot1b0(Vector const* Pos)
{
    // (RET 4).
    bActive = 0;
    if (Object != nullptr)
        PhySys::SetCollisionState(Object, false);
    CliEffectListRemove(&EffectList, Companion);
    // Pos, or vt+0 get_position() when null, copied to the stack.
    Vector const* const Where = (Pos != nullptr) ? Pos : &get_position();
    Vector const Copy = *Where;

    if (ExplosionArch == nullptr)
        return;
    if (ExplosionArch->Process != 2)
    {
        // .
        FDUMP(0x100002, "%s(%d) : *** WARNING: %s", kCliAsteroidSourceFile, 0x84c,
              "Explosion process for dynamic asteroid should be set to NONE");
        return;
    }
    // the routine(ExplosionArch, this, &Copy) into the action list.
    Actions.add(Action_Explode::Create(ExplosionArch, this, &Copy));
}

unsigned short* FCliDynamicAsteroidGameobj::Slot1bc(unsigned short* Out, unsigned A, unsigned B,
                                                   unsigned C, unsigned D)
{
    // (shared with MedAsteroid).
    (void)A;
    (void)B;
    (void)C;
    (void)D;
    return CliAsteroidSlot1bcBody(Out);
}

void FCliDynamicAsteroidGameobj::Place(Vector const* Pos, float MaxVelocity,
                                       float MaxAngularVelocity)
{
    // (RET 0xc). 0x00525206..0x00525254: an identity orientation on the stack and
    // _control87(0, 0) (reads the control word, changes nothing).
    Matrix const Identity(1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f);
    _control87(0, 0);
    // PhySys::BeamR(Object, *Pos, identity, true), no null test.
    PhySys::BeamR(Object, *Pos, Identity, true);

    // three rand() * (1/16384) - 1.0, each stored as a float.
    float const X = static_cast<float>(rand()) * kRandScale2 - 1.0f;
    float const Y = static_cast<float>(rand()) * kRandScale2 - 1.0f;
    float const Z = static_cast<float>(rand()) * kRandScale2 - 1.0f;
    // each squared, the square negated unless the value is above 0
    // (FCOMP 0.0, TEST AH,0x41 / JZ skips the FCHS).
    float SignedX = X * X;
    if (!(X > 0.0f))
        SignedX = -SignedX;
    float SignedY = Y * Y;
    if (!(Y > 0.0f))
        SignedY = -SignedY;
    float SignedZ = Z * Z;
    if (!(Z > 0.0f))
        SignedZ = -SignedZ;

    // the target velocity (* MaxVelocity) minus the current one
    // (PhySys::GetVelocity) through PhySys::AddToVelocity.
    Vector Target;
    Target.x = SignedX * MaxVelocity;
    Target.y = SignedY * MaxVelocity;
    Target.z = SignedZ * MaxVelocity;
    Vector const Current = PhySys::GetVelocity(Object);
    Vector Delta;
    Delta.x = Target.x - Current.x;
    Delta.y = Target.y - Current.y;
    Delta.z = Target.z - Current.z;
    PhySys::AddToVelocity(Object, Delta);

    // the current angular velocity, then the target: x = 2 * (r2 /
    // 32768 * M) - M, y = 2 * (r1 / 32768 * M) - M, z = r3 * M / 16384 - M (r1 drawn first and
    // kept as a float), through PhySys::AddToAngularVelocityOS as the difference.
    Vector const AngCurrent = PhySys::GetAngularVelocityOS(Object);
    float const R1 = static_cast<float>(rand()) * kRandScale;
    float const R2Scaled = static_cast<float>(rand()) * kRandScale * MaxAngularVelocity;
    float const AngX = (R2Scaled + R2Scaled) - MaxAngularVelocity;
    float const R1Scaled = R1 * MaxAngularVelocity;
    float const AngY = (R1Scaled + R1Scaled) - MaxAngularVelocity;
    float const AngZ =
        static_cast<float>(rand()) * MaxAngularVelocity * kRandScale2 - MaxAngularVelocity;
    Vector AngDelta;
    AngDelta.x = AngX - AngCurrent.x;
    AngDelta.y = AngY - AngCurrent.y;
    AngDelta.z = AngZ - AngCurrent.z;
    PhySys::AddToAngularVelocityOS(Object, AngDelta);
    // GetAngularVelocityOS once more; the result is dropped.
    (void)PhySys::GetAngularVelocityOS(Object);

    // the particle (+0xc0) starts at intensity 0 (vt+0xc).
    if (Companion[2] != nullptr)
        ParticleSetIntensity(static_cast<FParticleEffect*>(Companion[2]), 0.0f);
}

FCliDynamicAsteroid* FCliDynamicAsteroidGameobj::GetOwner() const
{
    FCliDynamicAsteroid* Owner = nullptr;
    std::memcpy(&Owner, &bWatchListed, sizeof(Owner));
    return Owner;
}

void FCliDynamicAsteroidGameobj::SetOwner(FCliDynamicAsteroid* Owner)
{
    static_assert(sizeof(FCliDynamicAsteroid*) == 4, "the +0xa8 owner is one dword");
    std::memcpy(&bWatchListed, &Owner, sizeof(Owner));
}

// ===========================================================================
// The [DynamicAsteroids] manager
// ===========================================================================

FCliDynamicAsteroid* CliDynamicAsteroidConstruct(FCliDynamicAsteroid* This,
                                                 CmnAsteroid::CAsteroidField* CmnField)
{
    // (RET 4), in store order. SlotCount (+0x14), CameraPos (+0x28), MeshDesc
    // (+0x38), PrimitiveType (+0xac), the two totals (+0xb4 / +0xb8) and InvDetailScale (+0xe4)
    // are not written; the routine sets InvDetailScale right after.
    This->CmnField = CmnField;
    This->TintedDiffuse0 = 1.0f;
    This->TintedDiffuse1 = 1.0f;
    This->TintedDiffuse2 = 1.0f;
    This->TintedAmbient0 = 1.0f;
    This->TintedAmbient1 = 1.0f;
    This->TintedAmbient2 = 1.0f;
    This->ColorShift0 = 1.0f;
    This->ColorShift1 = 1.0f;
    This->ColorShift2 = 1.0f;
    This->DetailScale = 1.0f;
    This->MaxVelocity = 50.0f;
    This->MaxAngularVelocity = 5.0f;
    This->CountIni = 10;
    This->PlacementRadius = 150.0f;
    This->PlacementRadiusInner = 75.0f;
    This->Arch = nullptr;
    This->SlotPtrs = nullptr;
    This->bSpawnAllowed = 0;
    This->bAnyVisible = 0;
    This->Zone = nullptr;
    This->Material = nullptr;
    This->Mesh = nullptr;
    if (This->CmnField == nullptr)
    {
        // .
        FDUMP(0x100001, "%s(%d) : *** ERROR: %s", kCliAsteroidSourceFile, 0x8ab,
              "NULL common field passed to dynamic asteroid manager");
    }
    return This;
}

void CliDynamicAsteroidReleaseSlots(FCliDynamicAsteroid* This)
{
    // .
    if (This->SlotPtrs == nullptr)
        return;
    for (int Index = 0; Index < This->SlotCount; ++Index)
    {
        if (This->SlotPtrs[Index] == nullptr)
            continue;
        This->SlotPtrs[Index]->Slot15c(true);
        This->SlotPtrs[Index] = nullptr;
    }
}

void CliDynamicAsteroidDestroy(FCliDynamicAsteroid* This)
{
    // . 0x00525764..0x0052576d: the archetype released (+8 is left as it is).
    if (This->Arch != nullptr)
        This->Arch->Release();
    // the slot loop (the routine's body, inlined), then the array.
    CliDynamicAsteroidReleaseSlots(This);
    ::operator delete(This->SlotPtrs);
    This->SlotPtrs = nullptr;
    // the material (vt+8).
    if (This->Material != nullptr)
    {
        This->Material->Release();
        This->Material = nullptr;
    }
    // the mesh copy -- vertices, indices, the block (+0xb0 is not
    // cleared).
    FCliDynamicAsteroidMesh* const Mesh = This->Mesh;
    if (Mesh != nullptr)
    {
        ::operator delete(Mesh->Verts);
        Mesh->Verts = nullptr;
        Mesh->VertCount = 0;
        ::operator delete(Mesh->Indices);
        Mesh->Indices = nullptr;
        Mesh->IndexCount = 0;
        ::operator delete(Mesh);
    }
}

void CliDynamicAsteroidParseIni(FCliDynamicAsteroid* This, INI_Reader& Reader)
{
    // (RET 4). 0x00525824..0x00525998: the section's values.
    if (Reader.read_value())
    {
        do
        {
            if (Reader.is_value("asteroid"))
            {
                // Archetype::GetDynamicAsteroid(GetDynamicAsteroidID(
                // CreateID(name))) into +8; a miss warns; a hit is loaded (vt+0xc) and its
                // companion Load(0) (vt+0x20).
                char const* const Name = Reader.get_value_string(0);
                ID_String Id;
                Id.Id = CreateID(Name);
                This->Arch = Archetype::GetDynamicAsteroid(Archetype::GetDynamicAsteroidID(Id));
                if (This->Arch == nullptr)
                {
                    FDUMP(0x100002,
                          "%s(%d) : *** WARNING: dynamic asteroid archetype (%s) not found",
                          kCliAsteroidSourceFile, 0x8cb, Name);
                }
                if (This->Arch != nullptr)
                {
                    This->Arch->load();
                    if (This->Arch->Companion != nullptr)
                        This->Arch->Companion->Load(0);
                }
            }
            else if (Reader.is_value("count"))
            {
                // ftol.
                This->CountIni = static_cast<int>(Reader.get_value_float(0));
            }
            else if (Reader.is_value("placement_radius"))
            {
                // the radius, and radius * 0.7 (double the field).
                float const Radius = Reader.get_value_float(0);
                This->PlacementRadius = Radius;
                This->PlacementRadiusInner = static_cast<float>(Radius * 0.7);
            }
            else if (Reader.is_value("max_velocity"))
            {
                This->MaxVelocity = Reader.get_value_float(0);
            }
            else if (Reader.is_value("max_angular_velocity"))
            {
                This->MaxAngularVelocity = Reader.get_value_float(0);
            }
            else if (Reader.is_value("color_shift"))
            {
                // all three read, then stored.
                float const R = Reader.get_value_float(0);
                float const G = Reader.get_value_float(1);
                float const B = Reader.get_value_float(2);
                This->ColorShift0 = R;
                This->ColorShift1 = G;
                This->ColorShift2 = B;
            }
        } while (Reader.read_value());
    }

    // the routine, the array freed, SlotCount = ftol(CountIni *
    // DetailScale + 0.5), a new array zeroed.
    CliDynamicAsteroidReleaseSlots(This);
    ::operator delete(This->SlotPtrs);
    This->SlotPtrs = nullptr;
    This->SlotCount = static_cast<int>(
        static_cast<double>(This->CountIni) * This->DetailScale + 0.5f);
    This->SlotPtrs = static_cast<FCliDynamicAsteroidGameobj**>(::operator new(
        static_cast<unsigned>(This->SlotCount) * sizeof(FCliDynamicAsteroidGameobj*)));
    for (int Index = 0; Index < This->SlotCount; ++Index)
        This->SlotPtrs[Index] = nullptr;

    // the material released, both totals zeroed.
    if (This->Material != nullptr)
    {
        This->Material->Release();
        This->Material = nullptr;
    }
    This->MeshVertTotal = 0;
    This->MeshIndexTotal = 0;

    if (This->Arch != nullptr)
    {
        // IMesh for the archetype's engine index (+0x40).
        IMesh* Mesh = nullptr;
        IRenderableMesh* RMesh = nullptr;
        int const EngineIndex = static_cast<int>(This->Arch->EngineIndex);
        if (NeuronRuntime::Engine->QueryEngineComponent(EngineIndex, const_cast<char*>(Iids::Mesh),
                                                reinterpret_cast<void**>(&Mesh)) < 0)
        {
            // no mesh block, whatever was handed out released, and
            // no vertex source descriptor.
            This->Mesh = nullptr;
            if (RMesh != nullptr)
                RMesh->Release();
            if (Mesh != nullptr)
                Mesh->Release();
            return;
        }
        // IMesh slot 9 (0, 1), slot 14 into the material while there is
        // none, slot 13 into the primitive type, slot 10.
        Mesh->OpenFaceGroup(0, 1);
        if (This->Material == nullptr)
            Mesh->GetFaceGroupMaterial(&This->Material);
        Mesh->GetFaceGroupPrimitiveType(&This->PrimitiveType);
        Mesh->CloseFaceGroup();
        // NeuronRuntime::Renderer slot 13 (engine index, LOD 0).
        NeuronRuntime::Renderer->SetArchetypeLod(reinterpret_cast<void*>(static_cast<intptr_t>(EngineIndex)), 0);
        // IRenderableMesh; the result is not tested.
        NeuronRuntime::Engine->QueryEngineComponent(EngineIndex,
                                            const_cast<char*>(Iids::RenderableMesh),
                                            reinterpret_cast<void**>(&RMesh));
        // the block, operator new(0x10) zeroed.
        FCliDynamicAsteroidMesh* const Block =
            static_cast<FCliDynamicAsteroidMesh*>(::operator new(sizeof(FCliDynamicAsteroidMesh)));
        Block->Verts = nullptr;
        Block->Indices = nullptr;
        Block->IndexCount = 0;
        Block->VertCount = 0;
        This->Mesh = Block;
        // vertex count (slot 3), operator new(count * 0x2c) (the
        // per-vertex texcoord constructors the routine write nothing), the total, then the
        // vertices in FVF 0x252 (slot 4).
        RMesh->GetVertexCount(&Block->VertCount);
        Block->Verts = static_cast<unsigned char*>(
            ::operator new(static_cast<unsigned>(Block->VertCount) * 0x2c));
        This->MeshVertTotal += static_cast<unsigned>(Block->VertCount);
        RMesh->CopyVertexData(Block->Verts, 0x252u,
                     0, Block->VertCount);
        // index count (slot 5), operator new(count * 2), the indices
        // (slot 6), the total.
        RMesh->GetIndexCount(&Block->IndexCount);
        Block->Indices = static_cast<unsigned short*>(
            ::operator new(static_cast<unsigned>(Block->IndexCount) * 2));
        RMesh->CopyIndices(&Block->Indices, Block->IndexCount);
        This->MeshIndexTotal += static_cast<unsigned>(Block->IndexCount);
        // both interfaces released.
        if (RMesh != nullptr)
            RMesh->Release();
        if (Mesh != nullptr)
            Mesh->Release();
    }

    // the routine(&MeshDesc, the block's vertices, the total's low
    // word) -- with no archetype too, through whatever +0xb0 holds (no null test).
    CliAsteroidFillVertexSource(&This->MeshDesc, This->Mesh->Verts,
                                static_cast<unsigned short>(This->MeshVertTotal));
}

FCliDynamicAsteroidGameobj* CliDynamicAsteroidSpawn(FCliDynamicAsteroid* This)
{
    // . 0x0052666b..0x005266da: operator new(0xc8) and the inlined ctor.
    FCliDynamicAsteroidGameobj* const Go = new FCliDynamicAsteroidGameobj(This);
    // vt+0x168 create(the archetype).
    Go->create(This->Arch);
    // the object narrowed by class & 0x4001 (null otherwise, not
    // tested) gets CDynamicAsteroid::init({the active system, zero velocity, zero spin}).
    CObject* const Obj = Go->GetObject();
    CDynamicAsteroid* const Dynamic =
        (Obj != nullptr &&
         (static_cast<unsigned>(Obj->ObjectClass) & 0x4001u) == 0x4001u)
            ? static_cast<CDynamicAsteroid*>(Obj)
            : nullptr;
    CDynamicAsteroid::CreateParms Parms;
    Parms.CollisionGroupId = 0;
    Parms.Velocity.x = 0.0f;
    Parms.Velocity.y = 0.0f;
    Parms.Velocity.z = 0.0f;
    Parms.AngularVelocity.x = 0.0f;
    Parms.AngularVelocity.y = 0.0f;
    Parms.AngularVelocity.z = 0.0f;
    Parms.CollisionGroupId = StarSysGetActiveSystemId();
    Dynamic->init(Parms);
    // collisions off while it waits to be placed.
    if (Go->GetObject() != nullptr)
        PhySys::SetCollisionState(Go->GetObject(), false);
    // slot 92 (the routine), then the routine(Go, 0).
    Go->Slot170();
    ClientObjDb_RegisterGameobj(Go, false);
    Go->bActive = 0;
    Go->bPlaced = 0;
    return Go;
}

void CliDynamicAsteroidUpdate(FCliDynamicAsteroid* This, FSpaceCamera* Cam, float Dt,
                              Exclusion::AsteroidExclusionZoneMgr* ExclusionMgr)
{
    // (RET 0xc). Dt is not read.
    (void)Dt;

    if (GetGameOption(kGameOptDynAsteroids) == 0)
    {
        // DYN_ASTEROIDS off -- every slot released, the array freed.
        if (This->SlotPtrs == nullptr)
            return;
        for (int Index = 0; Index < This->SlotCount; ++Index)
        {
            if (This->SlotPtrs[Index] == nullptr)
                continue;
            This->SlotPtrs[Index]->Slot15c(true);
            This->SlotPtrs[Index] = nullptr;
        }
        ::operator delete(This->SlotPtrs);
        This->SlotPtrs = nullptr;
        return;
    }

    if (This->SlotPtrs == nullptr)
    {
        // the array rebuilt at ftol(CountIni * DetailScale + 0.5).
        This->SlotCount = static_cast<int>(
            static_cast<double>(This->CountIni) * This->DetailScale + 0.5f);
        This->SlotPtrs = static_cast<FCliDynamicAsteroidGameobj**>(::operator new(
            static_cast<unsigned>(This->SlotCount) * sizeof(FCliDynamicAsteroidGameobj*)));
        for (int Index = 0; Index < This->SlotCount; ++Index)
            This->SlotPtrs[Index] = nullptr;
    }

    // the spawn centre -- the camera position plus the view axis (the
    // orientation's third column, +0x8 / +0x14 / +0x20) times -PlacementRadiusInner.
    ICamera* const Camera = AsCamera(Cam);
    float const* const Orient = Camera->GetOrientation();
    float const NegInner = -This->PlacementRadiusInner;
    float const OffsetX = Orient[2] * NegInner;
    float const OffsetY = Orient[5] * NegInner;
    float const OffsetZ = Orient[8] * NegInner;
    float const* const CamPos = Camera->GetPosition();
    Vector Centre;
    Centre.x = OffsetX + CamPos[0];
    Centre.y = OffsetY + CamPos[1];
    Centre.z = OffsetZ + CamPos[2];

    // no spawning while the player's ship (class & 0x503) is in a
    // tradelane; otherwise only when there is no zone or its immersion at the centre is above
    // 0.001 (FCOMP, TEST AH,0x41 / JZ -> allowed).
    FClientGameobj* const PlayerGo =
        static_cast<FClientGameobj*>(static_cast<IObjRW*>(GetPlayerObjRW()));
    CObject* const PlayerObj = (PlayerGo != nullptr) ? PlayerGo->GetObject() : nullptr;
    if (PlayerObj != nullptr &&
        (static_cast<unsigned>(PlayerObj->ObjectClass) & 0x503u) == 0x503u &&
        static_cast<CShip*>(PlayerObj)->is_using_tradelane())
    {
        This->bSpawnAllowed = 0;
    }
    else if (This->Zone != nullptr && !(This->Zone->GetImmersion(Centre) > kSpawnImmersion))
    {
        This->bSpawnAllowed = 0;
    }
    else
    {
        This->bSpawnAllowed = 1;
    }
    // an excluded centre (vt+4 is_excluded, type 3) blocks it too.
    if (This->bSpawnAllowed != 0 && ExclusionMgr != nullptr)
    {
        This->bSpawnAllowed =
            ExclusionMgr->is_excluded(Centre, Exclusion::EOT_DynamicAsteroid, nullptr) ? 0 : 1;
    }

    // r^2, the recycle distance r^2 * 1.05 and the fade band r^2 * 0.35
    // (both kept as doubles), and the start of the band (recycle - fade).
    float const Radius = This->PlacementRadius;
    double const RadiusSq = static_cast<double>(Radius) * Radius;
    double const FarSq = kFarFactor * RadiusSq;
    double const FadeSq = RadiusSq * kFadeFactor;
    double const BandStart = FarSq - FadeSq;
    This->bAnyVisible = 0;

    for (int Index = 0; Index < This->SlotCount; ++Index)
    {
        FCliDynamicAsteroidGameobj* Go = This->SlotPtrs[Index];
        float DistSq = 0.0f;
        bool bFade = false;
        if (Go != nullptr && Go->bPlaced != 0)
        {
            // the squared distance to the centre (z, y, then x),
            // stored as a float; inside the recycle distance (TEST AH,0x41 / JNZ) -> the fade.
            Vector const& Pos = Go->get_position();
            float const Dx = Pos.x - Centre.x;
            float const Dy = Pos.y - Centre.y;
            float const Dz = Pos.z - Centre.z;
            DistSq = (Dz * Dz + Dy * Dy) + Dx * Dx;
            if (!(DistSq > FarSq))
                bFade = true;
            else
                Go->bPlaced = 0;
        }

        if (bFade)
        {
            // alpha 1.0 inside the band start, else (recycle - d^2) /
            // fade; it counts as visible above 0.005.
            if (This->SlotPtrs[Index]->bActive == 0)
                continue;
            double Alpha = 1.0;
            if (DistSq > BandStart)
                Alpha = (FarSq - DistSq) / FadeSq;
            This->SlotPtrs[Index]->Alpha = static_cast<float>(Alpha);
            if (Alpha > kVisibleAlpha)
                This->bAnyVisible = 1;
            continue;
        }

        if (This->bSpawnAllowed == 0)
        {
            // out of range with spawning blocked -- released.
            if (This->SlotPtrs[Index] != nullptr)
            {
                This->SlotPtrs[Index]->Slot15c(true);
                This->SlotPtrs[Index] = nullptr;
            }
            continue;
        }

        // an empty slot gets a new gameobj (the routine); both
        // conditions below only warn.
        if (This->SlotPtrs[Index] == nullptr)
            This->SlotPtrs[Index] = CliDynamicAsteroidSpawn(This);
        if (This->SlotPtrs[Index] == nullptr)
        {
            FDUMP(0x100002, "%s(%d) : *** WARNING: %s", kCliAsteroidSourceFile, 0x986,
                  "dynamic asteroid not created");
        }
        if (This->SlotPtrs[Index]->bPlaced != 0)
        {
            FDUMP(0x100002, "%s(%d) : *** WARNING: %s", kCliAsteroidSourceFile, 0x988,
                  "dynamic asteroid should be flagged as !valid_pos");
        }

        // a random point on the placement sphere about the centre.
        float const PlaceRadius = This->PlacementRadius;
        Vector const Unit = random_on_unit_sphere();
        Vector Place;
        Place.x = PlaceRadius * Unit.x + Centre.x;
        Place.y = PlaceRadius * Unit.y + Centre.y;
        Place.z = PlaceRadius * Unit.z + Centre.z;

        // the old effect dropped (the routine), collisions off, the
        // placement (the routine), collisions on.
        Go = This->SlotPtrs[Index];
        CliEffectListRemove(&Go->EffectList, Go->Companion);
        if (This->SlotPtrs[Index]->GetObject() != nullptr)
            PhySys::SetCollisionState(This->SlotPtrs[Index]->GetObject(), false);
        This->SlotPtrs[Index]->Place(&Place, This->MaxVelocity, This->MaxAngularVelocity);
        if (This->SlotPtrs[Index]->GetObject() != nullptr)
            PhySys::SetCollisionState(This->SlotPtrs[Index]->GetObject(), true);

        // an effect that is still alive (a particle without the
        // deactivated bit, an active sound, a light without anim keys -- the keep tests of
        // ) warns and is dropped.
        Go = This->SlotPtrs[Index];
        Archetype::DynamicAsteroid* const Arch = This->Arch;
        FParticleEffect* const Particle = static_cast<FParticleEffect*>(Go->Companion[2]);
        ISoundSource* const Sound = static_cast<ISoundSource*>(Go->Companion[1]);
        FGameLight* const Light = static_cast<FGameLight*>(Go->Companion[3]);
        bool bLive = false;
        if (Particle != nullptr && (Particle->Flags08 & 2u) == 0u)
            bLive = true;
        else if (Sound != nullptr && Sound->IsActive() != 0)
            bLive = true;
        else if (Light != nullptr && Light->Anim.Keys == nullptr)
            bLive = true;
        if (bLive)
        {
            FDUMP(0x100002, "%s(%d) : *** WARNING: %s", kCliAsteroidSourceFile, 0x87c,
                  "dynamic asteroid effect already created");
            CliEffectListRemove(&Go->EffectList, Go->Companion);
        }

        // the archetype companion's particle_effect (+0x58 set, +0x54
        // not empty) as a one-shot on the gameobj -- the routine on the field with an
        // identity frame (the routine, zero position), the engine index (vt+0x1c) and the
        // companion slot (+0xb8).
        Archetype::ICliObj* const CliComp = Arch->Companion;
        if (CliComp != nullptr && Archetype::CliDynamicAsteroidHasParticleEffect(CliComp))
        {
            FTransform Frame;
            Frame.Rotation.D[0][0] = 1.0f;
            Frame.Rotation.D[0][1] = 0.0f;
            Frame.Rotation.D[0][2] = 0.0f;
            Frame.Rotation.D[1][0] = 0.0f;
            Frame.Rotation.D[1][1] = 1.0f;
            Frame.Rotation.D[1][2] = 0.0f;
            Frame.Rotation.D[2][0] = 0.0f;
            Frame.Rotation.D[2][1] = 0.0f;
            Frame.Rotation.D[2][2] = 1.0f;
            Frame.Translation.X = 0.0f;
            Frame.Translation.Y = 0.0f;
            Frame.Translation.Z = 0.0f;
            int const EngineIndex = static_cast<int>(Go->get_index());
            ID_String const* const EffectId = Archetype::CliDynamicAsteroidParticleEffect(CliComp);
            if (!EffectId->IsEmpty())
            {
                CliCreateEffectOneShot(FindEffectArch(&EffectId->Id), Go, EngineIndex, &Frame,
                                       Go->Companion);
            }
        }

        // .
        This->SlotPtrs[Index]->bPlaced = 1;
        This->SlotPtrs[Index]->bActive = 1;
        This->bAnyVisible = 1;
    }
}

void CliDynamicAsteroidDraw(FCliDynamicAsteroid* This, FSpaceCamera* Cam,
                            unsigned char* bAmbientDone, FCliAsteroid* Field)
{
    // (RET 0xc). 0x005261b9..0x005261e1: DYN_ASTEROIDS, a slot array, a camera.
    if (GetGameOption(kGameOptDynAsteroids) == 0)
        return;
    if (This->SlotPtrs == nullptr)
        return;
    if (Cam == nullptr)
        return;

    // the camera position into +0x28 (read by the loot roll).
    ICamera* const Camera = AsCamera(Cam);
    float const* const CamPos = Camera->GetPosition();
    This->CameraPos[0] = CamPos[0];
    This->CameraPos[1] = CamPos[1];
    This->CameraPos[2] = CamPos[2];
    if (This->bAnyVisible == 0)
        return;

    // the mesh copy into vertex memory -- lock (FVF +0xa0, count
    // +0xa4), copy (to lock[3], lock[1], the descriptor, 0, count), unlock.
    IVertexBufferManager* const VbMgr = NeuronRuntime::VertexBuffer;
    FVertexMemoryLock Lock;
    if (VbMgr->LockVertexMemory(static_cast<unsigned int>(This->MeshDesc.SrcFvf),
                                static_cast<unsigned int>(This->MeshDesc.SrcCount), &Lock) < 0)
    {
        // .
        FDUMP(0x100002, "%s(%d) : *** WARNING: %s", kCliAsteroidSourceFile, 0x9bd,
              "could not lock the vertex buffer for dynamic asteroids");
        return;
    }
    VbMgr->CopyVertexData(Lock.Vertices, Lock.Listing.Fvf, &This->MeshDesc, 0, This->MeshDesc.SrcCount);
    VbMgr->UnlockVertexMemory(&Lock);

    // the material's slot 8 with 0.
    if (This->Material != nullptr)
        This->Material->Bind(nullptr);

    // the opaque table, D3DRS_ALPHAREF (0x18) saved, then 5.
    IRenderPipeline* const Pipe = NeuronRuntime::Pipeline;
    ApplyRsTable(Pipe, kRsDynOpaque);
    int SavedAlphaBlend = 0;
    Pipe->GetRenderState(D3DRS_ALPHAREF, &SavedAlphaBlend);
    Pipe->SetRenderState(D3DRS_ALPHAREF, 5);

    // the 0x44-byte material block zeroed; diffuse = +0xbc..+0xc4;
    // ambient = +0xc8..+0xd0 with LIGHTING (0x18) on, else the diffuse (and its alpha).
    unsigned char bBlending = 0;
    float MaterialBlock[0x11] = {};
    int const bLighting = GetGameOption(kGameOptLighting);
    MaterialBlock[0] = This->TintedDiffuse0;
    MaterialBlock[1] = This->TintedDiffuse1;
    MaterialBlock[2] = This->TintedDiffuse2;
    if (bLighting != 0)
    {
        MaterialBlock[4] = This->TintedAmbient0;
        MaterialBlock[5] = This->TintedAmbient1;
        MaterialBlock[6] = This->TintedAmbient2;
    }
    else
    {
        MaterialBlock[4] = MaterialBlock[0];
        MaterialBlock[7] = MaterialBlock[3];
        MaterialBlock[5] = MaterialBlock[1];
        MaterialBlock[6] = MaterialBlock[2];
    }

    // the camera's view transform (vt+0x40), 12 floats.
    FTransform View;
    std::memcpy(&View, Camera->GetViewTransform(), sizeof(View));

    for (int Index = 0; Index < This->SlotCount; ++Index)
    {
        FCliDynamicAsteroidGameobj* const Go = This->SlotPtrs[Index];
        if (Go == nullptr || Go->bActive == 0)
            continue;
        // FCOMP 0.005, TEST AH,0x41 / JNP -- below or equal skips.
        if (Go->Alpha <= kVisibleAlpha)
            continue;

        // the field's ambient pass once per frame (the routine with
        // 100.0f).
        if (*bAmbientDone == 0)
        {
            CliAsteroidLightAround(Field, Cam, 100.0f);
            *bAmbientDone = 1;
        }

        // the slot's frame (vt+0x10, 12 floats); pipeline state 7 set
        // -> MulTransform(view, world) into the modelview; otherwise world and view apart.
        FTransform World;
        std::memcpy(&World, &This->SlotPtrs[Index]->get_transform(), sizeof(World));
        int bModelview = 1;
        Pipe->GetPipelineState(7, &bModelview);
        if (bModelview != 0)
        {
            FTransform Combined;
            CliAsteroidMathEngine()->MulTransform(Combined, View, World);
            FTransform Modelview = Combined;
            Pipe->SetModelview(reinterpret_cast<float const*>(&Modelview));
        }
        else
        {
            Pipe->SetWorld(reinterpret_cast<float const*>(&World));
            Pipe->SetView(reinterpret_cast<float const*>(&View));
        }

        // the alpha into the material's diffuse alpha; not below 1.0
        // (FCOMP 1.0, TEST AH,5 / JP) -> the opaque table and ALPHAREF 0x7f every time;
        // below 1.0 -> the blend table and 5, once until an opaque slot resets the flag.
        MaterialBlock[3] = This->SlotPtrs[Index]->Alpha;
        if (!(MaterialBlock[3] < 1.0f))
        {
            ApplyRsTable(Pipe, kRsDynOpaque);
            Pipe->SetRenderState(D3DRS_ALPHAREF, 0x7f);
            bBlending = 0;
        }
        else if (bBlending == 0)
        {
            ApplyRsTable(Pipe, kRsDynBlend);
            Pipe->SetRenderState(D3DRS_ALPHAREF, 5);
            bBlending = 1;
        }

        // the material, then DrawIndexedPrimitiveVb(prim +0xac,
        // lock[0], lock[2], count +0xa4, the indices, their count, 0).
        Pipe->SetMaterial((const D3DMATERIAL8*)MaterialBlock);
        Pipe->DrawIndexedPrimitiveVb(This->PrimitiveType, Lock.Listing.VertexBuffer,
                                     static_cast<unsigned int>(Lock.Listing.BaseVertex),
                                     static_cast<unsigned int>(This->MeshDesc.SrcCount),
                                     This->Mesh->Indices,
                                     static_cast<unsigned int>(This->Mesh->IndexCount), 0);

        // the particle's intensity = the alpha (vt+0xc), then the
        // effect list swept (the routine).
        if (This->SlotPtrs[Index]->Companion[2] != nullptr)
        {
            ParticleSetIntensity(static_cast<FParticleEffect*>(This->SlotPtrs[Index]->Companion[2]),
                                 MaterialBlock[3]);
        }
        GameobjSweepEffectListbaseline(&This->SlotPtrs[Index]->EffectList);
    }

    // ALPHAREF restored, the opaque table again.
    Pipe->SetRenderState(D3DRS_ALPHAREF, SavedAlphaBlend);
    ApplyRsTable(Pipe, kRsDynOpaque);
}
