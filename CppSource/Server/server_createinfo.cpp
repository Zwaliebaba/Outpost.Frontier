// Author: Giperion.

// server_createinfo.cpp -- the six CREATE* record builders that Observer
// ::update needs before its five create branches can actually send anything.

//   (the create-info constructors, destructor and clear() are inline in
//    src/include/server_createinfo.h -- see the note below the helper)
// module storage  CShipCreateInfo fill                 (218 insns)
// module storage  LootDesc fill                        ( 75 insns)
// module storage  CREATECOUNTER / CREATEMINE fill      ( 62 insns)
// module storage  CREATEGUIDED fill                    ( 71 insns)

// Ground truth is Server. The inspected .c agrees on shape
// here but loses every register-allocated value in the routine (its
// `unaff_EBX` / `unaff_ESI` for the quaternion's first two dwords are the
// I3DMathEngine call's out-parameter, read back off the stack at module storage
// and module storage).

// See src/include/server_createinfo.h for how RTTI names these records and for
// the field-by-field agreement with rpclocal's independent reading of the same
// bytes from the receiving side.
// ---------------------------------------------------------------------------

#include "../include/server_createinfo.h"

#include "../include/common_archetype.h"
#include "../include/common_cequip.h"
#include "../include/common_csys.h"       // Matrix
#include "../include/neuroncomponents.h"             // NEURONCOMPONENTS_Acquire, ICOManager, AGGDESC (the routine)
#include "../include/common_iface.h"      // IObjRW
#include "../include/common_misc.h"       // DamageList, SUBOBJ_ID_NONE
#include "../include/common_reputation.h" // Reputation::Vibe::GetRank
#include "../include/x86math.h"           // I3DMathEngine::MatrixToQuaternion

// ===========================================================================
// see the declaration in server_createinfo.h.
// SUB  ESP,0x10
// MOV  [ESP + 0x8],"3DMathEngine"     ; desc.name
// MOV  [ESP + 0xc],0x0                ; desc third dword
// MOV  [ESP + 0x4],0xc                ; desc.size
// CALL [->NEURONCOMPONENTS.DLL::NEURONCOMPONENTS_Acquire]
// CALL [ECX + 0xc]                    ; CreateInstance(&desc, &[ESP])
// MOV  EAX,[ESP]                      ; return the out slot
// The out slot is never initialised before the call, and neither the manager
// nor the result is tested. The port seeds the out slot with null so that a
// CreateInstance that does not write it hands back null rather than stack
// residue; every baseline caller dereferences the result either way.
// ===========================================================================
I3DMathEngine* Server_Acquire3DMathEngine()
{
    struct FAGGDESC12
    {
        unsigned long Size;       // +0x00  0xc
        const char* Name;         // +0x04  "3DMathEngine"
        void* Field_0x08;         // +0x08  0
    };
    FAGGDESC12 Desc;
    Desc.Size = 0xc;
    Desc.Name = "3DMathEngine";
    Desc.Field_0x08 = nullptr;

    void* Engine = nullptr;
    NEURONCOMPONENTS_Acquire()->CreateInstance(reinterpret_cast<AGGDESC*>(&Desc), &Engine);
    return static_cast<I3DMathEngine*>(Engine);
}

// .data:the field -- StarSys.cpp's file-static engine, shared by the four
// record builders below (module storage, module storage, module storage,
// , module storage) and by the routine / the routine /
// 's own lazy fetches of the same slot.
static I3DMathEngine* GStarSysMathEngine = nullptr;

// ===========================================================================
// The shared quaternion helper.

// MOV  EAX,[.data:the field]
// JNZ  the branch
// CALL the routine / MOV [.data:the field],EAX
// PUSH EDX        ; &CObject::Orientation  (object + 0x08)
// PUSH EDX        ; the 16-byte out slot
// PUSH EAX        ; the engine
// ff5150  CALL dword ptr [ECX + 0x50]
// I3DMathEngine's slot +0x50 is MatrixToQuaternion; it is the same slot the
// observer update calls at module storage. No null test on the engine and no
// default for the output: baseline's out slot is uninitialised stack that the
// call always fills.
// ===========================================================================
void Server_MatrixToQuat(Quaternion& Out, Matrix const& In)
{
    if (GStarSysMathEngine == nullptr)
        GStarSysMathEngine = Server_Acquire3DMathEngine();
    GStarSysMathEngine->MatrixToQuaternion(reinterpret_cast<FQuaternion&>(Out),
                                           reinterpret_cast<FMatrix const&>(In));
}

// ===========================================================================
// QuaternionToMatrix through the same engine -- the step both object builders
// take from their request's +0x58 quaternion back to CreateParms' 3x3:
// module storage MOV EAX,[.data:the field] / JNZ / CALL the routine
// LEA EDX,[EBP + 0x58] / PUSH EDX ; the quaternion
// LEA EDX,[ESP + 0x11c] / PUSH EDX ; 36-byte out slot
// CALL dword ptr [ECX + 0x54]
// module storage .. module storage, the same shape.
// Both then copy the nine dwords with REP MOVSD into the parms. No null test on
// the engine.
// ===========================================================================
void Server_QuatToMatrix(Matrix& Out, Quaternion const& In)
{
    if (GStarSysMathEngine == nullptr)
        GStarSysMathEngine = Server_Acquire3DMathEngine();
    GStarSysMathEngine->QuaternionToMatrix(reinterpret_cast<FMatrix&>(Out),
                                           reinterpret_cast<FQuaternion const&>(In));
}

// ===========================================================================
// CEqObjCreateInfo / CShipCreateInfo / CSolarCreateInfo members --
// (ctor), module storage (dtor), module storage and
// (clear) -- are defined INLINE in
// src/include/server_createinfo.h, because baseline compiled them from a shared
// header into both Server.dll and rpclocal.dll (rpclocal's copies are
// / module storage / module storage / module storage). The
// store-by-store derivation is quoted there, next to each member.
// ===========================================================================

// ===========================================================================
// CShipCreateInfo fill. __cdecl(Info, Ship).

// EDI is Info (read from the param_1 slot at module storage, i.e. AFTER the
// PUSH EDI at module storage -- `MOV EDI,[ESP+0x5c]` with ESP at entry-0x58)
// and ESI is Ship.
// ===========================================================================
void __cdecl CShipCreateInfo_Fill(CShipCreateInfo* Info, CShip* Ship)
{
    // (w4-server-guards) No null test on either argument: module storage
    // `MOV ECX,ESI` / 06d076b1 calls through Ship and 06d076bd stores through
    // Info with nothing in between.

    // CALL [->COMMON.DLL::CShip::is_using_tradelane]
    // MOV byte ptr [EDI + 0xdc],AL
    Info->bUsingTradelane =
        static_cast<unsigned char>(Ship->is_using_tradelane() ? 1 : 0);

    // CALL [->COMMON.DLL::CObject::get_archetype]
    // MOV EAX,[EAX + 0x8]   -- Archetype::Root+0x08 is Nickname.
    // (w4-server-guards) The archetype is read with no null test.
    Archetype::Root const* const Arch = Ship->get_archetype();
    Info->ArchetypeNickname = Arch->get_id();

    // LEA EAX,[ESI + 0x2c] -- the position is read as a FIELD,
    // not through get_position(); +0x2c is EngineObject::WorldPosition.
    Info->Position = Ship->WorldPosition;

    // LEA EDX,[ESI + 0x8] -- and the orientation likewise, as
    // the raw Matrix at EngineObject+0x08.
    Server_MatrixToQuat(Info->Orientation, Ship->Orientation);

    // ff5054  CALL dword ptr [EAX + 0x54]
    // CObject's vtable +0x54 is get_velocity: in Common.dll's own named tables
    // ??_7CEqObj@@6B@ (module storage) has module storage == base + 0x54
    // pointing at .text:CObject::get_velocity, and ??_7CLoot@@6B@ agrees at its
    // own module storage. It returns a Vector by value, which is why baseline
    // pushes a 12-byte out slot and copies three dwords back.
    Info->Velocity = Ship->get_velocity();

    // ff5258  CALL dword ptr [EDX + 0x58] -- the very next slot,
    // == CObject::get_angular_velocity.
    Info->AngularVelocity = Ship->get_angular_velocity();

    // MOV ECX,[ESI + 0xb0] -- CObject::Id, read as a field.
    Info->ObjId = Ship->Id;
    Info->Field_0x6c = 0u;

    // ff92ac000000  CALL dword ptr [EDX + 0xac]
    // == CEqObj vtable base + 0xac == CEqObj::get_equip_desc_list.
    // The argument is `Info + 0x70`, the EquipDescVector itself.
    Ship->get_equip_desc_list(Info->Equipment);

    // CALL [->COMMON.DLL::CEqObj::get_collision_group_des...]
    // The decorated import spells the element type out:
    // ?get_collision_group_description@CEqObj@@QBEXAAV?$list@UCollisionGroupDesc@@...
    Ship->get_collision_group_description(Info->CollisionGroups);

    // MOV EDX,[ESI + 0x29c] -- moved as a dword, but +0x29c is
    // `float Throttle` (common_cobject.h pins it), and rpclocal's encoder proves
    // the type from the other end: it FLDs the field this one feeds and
    // quantizes it with the 127.0 [-1,+1] scale.
    Info->Throttle = Ship->Throttle;

    // .. module storage -- CEqObj+0x104 then the Costume block at
    // CEqObj+0x108..+0x138. Four head dwords, the count, and then exactly
    // `count` accessory dwords -- module storage `MOV ECX,[ECX + EDX*0x1]` walks
    // source+0x118 upward while EBP counts to [Info + 0x44].
    Info->VibeGroupId = static_cast<int>(Ship->VibeId);
    Info->CostumeHead = Ship->SpaceCostume.PartIds[0];
    Info->CostumeBody = Ship->SpaceCostume.PartIds[1];
    Info->CostumeLeftHand = Ship->SpaceCostume.PartIds[2];
    Info->CostumeRightHand = Ship->SpaceCostume.PartIds[3];
    Info->CostumeAccessoryCount = Ship->SpaceCostume.AccessoryCount;
    // Unbounded, as baseline: the signed JLE at module storage skips a count <= 0,
    // and the JL at module storage re-reads the bound from Info+0x44 each pass.
    for (int Index = 0; Index < Info->CostumeAccessoryCount; ++Index)
        Info->CostumeAccessory[Index] = Ship->SpaceCostume.AccessoryIds[Index];

    Info->VoiceId = Ship->VoiceId;               // ,  CEqObj+0x13c
    Info->Field_0xb0 = Ship->GroupNameId;        // ,  CShip+0x21c
    Info->OwnerPlayer = Ship->OwnerPlayer;       // ,  CSimple+0xb4
    Info->DockingBaseId = Ship->DockingBaseId;   // ,  CEqObj+0x160
    // MOV ECX,dword ptr [ESI + 0x1b4]
    // MOV dword ptr [EDI + 0xd8],ECX
    // CShip+0x1b4 is the multiplayer group id (common_cobject.h GroupId).
    Info->Field_0xd8 = Ship->GroupId;

    // CALL [->COMMON.DLL::CSimple::get_hit_pts]
    // CALL MSVCRT.DLL::ftol            ; truncate toward zero
    // MOV [EDI + 0x68],EAX
    Info->HitPts = static_cast<int>(Ship->get_hit_pts());

    // CALL [->COMMON.DLL::Reputation::Vibe::GetRank]
    // __cdecl with two pushed arguments -- `LEA EAX,[ESI + 0x104]` (the group id
    // BY REFERENCE, not a Vibe object) and the address of a stack unsigned. The
    // `ADD ESP,0xc` at module storage balances those two plus the operator_new
    // argument pushed in between.
    {
        unsigned int Rank = 0u;
        int const GroupId = static_cast<int>(Ship->VibeId);
        Reputation::Vibe::GetRank(GroupId, Rank);
        Info->ReputationRank = Rank;
    }

    // -----------------------------------------------------------------------
    // .. module storage -- and now the part that looks like a
    // decode error and is not.

    // baseline builds a COMPLETE DamageList on the stack (operator_new(0x14) for
    // the list sentinel at module storage, then the four POD fields zeroed at
    // /.../module storage), walks the ship's equipment with a
    // CEquipTraverser(0x100) and adds one entry per item --
    // ff523c  CALL dword ptr [EDX + 0x3c]   ; CEquip::GetHitPoints
    // CALL [->COMMON.DLL::DamageList::add_damage_entry]
    // with the arguments pushed 0 / hit-points / id -- and then DESTROYS it at
    // .. module storage without ever storing it anywhere. The
    // record's own DamageList at +0x8c is left exactly as the constructor
    // made it: empty.

    // That is not a misread. The list's base is the stack slot at entry-0x28
    // (`LEA ECX,[ESP+0x40]` at module storage with three arguments already
    // pushed, ESP at entry-0x68), no register ever holds `Info + 0x8c`, and the
    // teardown loop at module storage reads back the same entry-0x24 head it
    // allocated. analysis's own output agrees: `aDStack_40` is a local and
    // `param_1 + 0x8c` never appears.

    // It is reproduced rather than dropped because it is what baseline does, and
    // because CEquip::GetHitPoints is virtual -- some override could in
    // principle have a side effect this port has not seen yet.
    // -----------------------------------------------------------------------
    {
        DamageList Discarded;
        // PUSH 0x100 -- the ONE-argument CEquipTraverser ctor
        // (module storage "??0CEquipTraverser@@QAE@H@Z"), mask 0x100. Note this
        // is a different mask and a different overload from the (-1, true) the
        // damage-list slot uses.
        CEquipTraverser Walk(0x100);
        for (;;) {
            // CALL EBP=>COMMON.DLL::CEquipManager::Traverse
            CEquip* const Equip = Ship->EquipManager.Traverse(Walk);
            if (Equip == nullptr)
                break;
            // MOV DI,word ptr [EAX + 0x8] -- the id is taken as
            // a WORD out of CEquip+0x08 while EDI still holds the high half of
            // the Info pointer; the callee only reads the low word.
            Discarded.add_damage_entry(Equip->Id, Equip->GetHitPoints(),
                                       DamageEntry::SubObjFate::None);
        }
    }
}

// ===========================================================================
// CSolarCreateInfo fill. __cdecl(Info, Solar); its only caller,
// StarSystem::add_player's multiplayer solar walk, balances with `ADD ESP,0x8`
// at module storage. Ported 2026-09-24 from the raw listing
// (module storage .. module storage).

// ESI is Solar (read at module storage, `[ESP+0x58]` two pushes into the 0x48
// frame == entry+8) and EDI is Info (module storage, entry+4). It is the ship
// fill's shape minus everything ship-specific: no tradelane byte, no
// velocities, no throttle, no rank, no owner, no +0x1b4 -- and two solar bytes
// of its own at the end. Nothing is null-tested; the caller only passes an
// object whose (ObjectClass & 0x303) == 0x303.
// ===========================================================================
void __cdecl CSolarCreateInfo_Fill(CSolarCreateInfo* Info, CSolar* Solar)
{
    // CALL [->COMMON.DLL::CObject::get_archetype]
    // MOV EAX,[EAX + 0x8] / module storage MOV [EDI + 0x8],EAX
    Info->ArchetypeNickname = Solar->get_archetype()->get_id();

    // .. module storage -- the three dwords at CObject+0x2c, as
    // a field, into Info+0x4c.
    Info->Position = Solar->WorldPosition;

    // .. module storage -- the field's MatrixToQuaternion on
    // CObject+0x08 (`LEA EDX,[ESI + 0x8]`), the four result dwords into +0x58.
    Server_MatrixToQuat(Info->Orientation, Solar->Orientation);

    Info->ObjId = Solar->Id;                     // / module storage
    Info->Field_0x6c = 0u;                       // MOV [EDI + 0x6c],EBX

    // ff92ac000000  CALL dword ptr [EDX + 0xac] on Info+0x70 --
    // CEqObj::get_equip_desc_list, the same slot the ship fill uses.
    Solar->get_equip_desc_list(Info->Equipment);

    // CALL [->COMMON.DLL::CEqObj::get_collision_group_description]
    // on Info+0x80.
    Solar->get_collision_group_description(Info->CollisionGroups);

    // .. module storage -- CEqObj+0x104 into +0x10, the four
    // Costume head dwords at CEqObj+0x108.. into +0x14.., the count at
    // CEqObj+0x138 into +0x44 (module storage), and then exactly `count`
    // accessory dwords, source CEqObj+0x118.. walked by the distance
    // `ECX - EAX` against the destination pointer (module storage / 06d07a50).
    // The loop re-reads the bound from Info+0x44 each pass (module storage) and
    // is not clamped.
    Info->VibeGroupId = static_cast<int>(Solar->VibeId);
    Info->CostumeHead = Solar->SpaceCostume.PartIds[0];
    Info->CostumeBody = Solar->SpaceCostume.PartIds[1];
    Info->CostumeLeftHand = Solar->SpaceCostume.PartIds[2];
    Info->CostumeRightHand = Solar->SpaceCostume.PartIds[3];
    Info->CostumeAccessoryCount = Solar->SpaceCostume.AccessoryCount;
    for (int Index = 0; Index < Info->CostumeAccessoryCount; ++Index)
        Info->CostumeAccessory[Index] = Solar->SpaceCostume.AccessoryIds[Index];

    Info->VoiceId = Solar->VoiceId;               // / module storage
    Info->DockingBaseId = Solar->DockingBaseId;   // / module storage

    // / module storage -- CSolar+0x1a8 into +0xad, and
    // / module storage -- CSolar+0x1a9 into +0xac.
    Info->Field_0xad = static_cast<unsigned char>(Solar->bDestructible ? 1 : 0);
    Info->Field_0xac = static_cast<unsigned char>(Solar->bDynamic ? 1 : 0);

    // CALL [->COMMON.DLL::CSimple::get_hit_pts]
    // CALL MSVCRT.DLL::ftol / module storage MOV [EDI + 0x68],EAX
    Info->HitPts = static_cast<int>(Solar->get_hit_pts());

    // .. module storage -- the same dead DamageList the ship fill
    // builds: operator_new(0x14) sentinel (module storage), the four POD fields
    // zeroed (module storage .. 06d07ac5), CEquipTraverser(0x100)
    // (module storage) over CEqObj+0xe4 (module storage), one
    // DamageList::add_damage_entry(equip->Id as a word, GetHitPoints (vtable
    // +0x3c), 0) per item (module storage .. module storage), then every node
    // erased and freed (module storage .. module storage) and the sentinel
    // freed (module storage). It is a stack local; nothing reads it.
    {
        DamageList Discarded;
        CEquipTraverser Walk(0x100);
        for (;;) {
            CEquip* const Equip = Solar->EquipManager.Traverse(Walk);
            if (Equip == nullptr)
                break;
            Discarded.add_damage_entry(Equip->Id, Equip->GetHitPoints(),
                                       DamageEntry::SubObjFate::None);
        }
    }
}

// ===========================================================================
// the 0x48-byte LootDesc. __cdecl(Desc, Loot).

// ESI is Loot (param_2, read at module storage after one PUSH) and EDI is Desc
// (param_1, read at module storage after two).
// ===========================================================================
void __cdecl ServerLootDesc_Fill(FServerLootDesc* Desc, CLoot* Loot)
{
    // (w4-server-guards) No null test on Desc, Loot, ContentsArch or the
    // archetype: module storage reads Loot, 06d07ba3 stores through Desc,
    // `MOV EDX,[ECX + 0x8]` reads ContentsArch, 06d07bcb
    // `MOV ECX,[EAX + 0x8]` reads get_archetype()'s result, and 06d07bec
    // `CMP EAX,[ECX + 0x14]` reads ContentsArch again -- all unconditionally.

    Desc->LootId = Loot->Id;                     // / module storage

    // MOV ECX,[ESI + 0xec]      ; CLoot::ContentsArch
    // MOV EDX,[ECX + 0x8]       ; Archetype::Root+0x08 Nickname
    Archetype::Equipment const* const Contents = Loot->ContentsArch;
    Desc->ContentsNickname = Contents->get_id();

    Desc->ContentsHitPts = Loot->ContentsHitPts; // FLD [ESI+0xf4]
    Desc->Units = Loot->Units;                   // / module storage

    // CALL [->COMMON.DLL::CObject::get_archetype]
    Archetype::Root const* const Container = Loot->get_archetype();
    Desc->ContainerNickname = Container->get_id();

    // CALL [->COMMON.DLL::CSimple::get_hit_pts]
    // d95f14  FSTP float ptr [EDI + 0x14] -- kept as a FLOAT
    // here, unlike CShipCreateInfo+0x68 which ftol's the same accessor.
    Desc->HitPts = Loot->get_hit_pts();

    // ff9288000000  CALL dword ptr [EDX + 0x88]
    // CObject-family vtable +0x88 is get_name: ??_7CLoot@@6B@'s own slot at
    // (base module storage) is .text:CLoot::get_name, and the
    // matching slot of ??_7CEqObj@@6B@ at module storage is CEqObj::get_name.

    // CMP EAX,dword ptr [ECX + 0x14] -- ECX is still
    // CLoot::ContentsArch, so +0x14 is Archetype::Root::IdsName. The name only
    // travels when it is an OVERRIDE; when get_name() answers the contents
    // archetype's own ids_name the field keeps whatever the caller left there
    // (the caller zeroed +0x18 at module storage).
    unsigned int const Name = Loot->get_name();
    if (Name != static_cast<unsigned int>(Contents->IdsName))
        Desc->NameOverride = Name;

    Desc->Position = Loot->WorldPosition;        // .. module storage
    Server_MatrixToQuat(Desc->Orientation, Loot->Orientation); // /module storage
    Desc->Velocity = Loot->get_velocity();       // CALL [EAX + 0x54]
    // / module storage
    Desc->bTemporary = static_cast<unsigned char>(Loot->bTemporary ? 1 : 0);
}

// ===========================================================================
// CREATECOUNTER (IClient::SendServerCreateCounter) and CREATEMINE
// (IClient::SendServerCreateMine) share one
// builder. __cdecl(Info, Projectile).

// The parameters are in the OPPOSITE registers from the loot builder: ESI is
// param_1 (module storage `MOV ESI,[ESP+0x18]`, one PUSH deep) and EDI is
// param_2 (module storage, two deep). analysis's own stack-variable annotation
// says so on both lines, and every subsequent store goes through ESI.
// ===========================================================================
void __cdecl ServerCreateProjectileInfo_Fill(FServerCreateProjectileInfo* Info,
                                             CProjectile* Projectile)
{
    // (w4-server-guards) No null test on Info, Projectile or the archetype:
    // reads Projectile, 06d07c95 stores through Info and
    // `MOV ECX,[EAX + 0x8]` reads get_archetype()'s result.
    Info->ObjId = Projectile->Id;                // / module storage

    Archetype::Root const* const Arch = Projectile->get_archetype();
    Info->ArchetypeNickname = Arch->get_id();

    // MOV AX,word ptr [EDX] / module storage MOV [ESI + 0xc],AX
    Info->SubObjId = SUBOBJ_ID_NONE;

    Server_MatrixToQuat(Info->Orientation, Projectile->Orientation); // ..cef
    Info->OwnerId = Projectile->OwnerId;         // , CProjectile+0xe4
    Info->Position = Projectile->WorldPosition;  // .. module storage
    Info->Field_0x44 = 0u;
    Info->Velocity = Projectile->get_velocity(); // CALL [EDX + 0x54]
}

// ===========================================================================
// CREATEGUIDED (IClient::SendServerCreateGuided). __cdecl(Info, Guided).
// ===========================================================================
void __cdecl ServerCreateGuidedInfo_Fill(FServerCreateGuidedInfo* Info, CGuided* Guided)
{
    // (w4-server-guards) No null test on Info, Guided or the archetype:
    // calls through Guided, 06d07d51 `MOV EAX,[EAX + 0x8]`
    // reads the archetype and 06d07d58 stores through Info.
    Archetype::Root const* const Arch = Guided->get_archetype();
    Info->ArchetypeNickname = Arch->get_id();
    Info->Field_0x2c = 0u;
    Info->ObjId = Guided->Id;                    // / module storage
    Info->Position = Guided->WorldPosition;      // .. module storage
    Server_MatrixToQuat(Info->Orientation, Guided->Orientation); // ..dbd
    Info->OwnerId = Guided->OwnerId;             // , CProjectile+0xe4

    // .. module storage -- CGuided::get_target() inlined, then
    // IObjRW's vtable slot +0x20 (get_id) on the result:
    // MOV EAX,[EDI + 0xf0]     ; BaseWatcher::Watched
    // ADD EAX,-0x8             ; Watchable -> IObjRW
    // JZ  the branch         ; the adjusted pointer null-checked
    // MOV EDX,[EAX + -0x8]
    // ff5220  CALL dword ptr [EDX + 0x20]
    // baseline re-loads +0xf0 for the call and has a the branch arm that does
    // `XOR ECX,ECX / MOV EDX,[ECX]` -- a guaranteed null dereference, reachable
    // only if the field changed between the two loads, which it cannot. The port
    // evaluates get_target() once.
    IObjRW* const Target = static_cast<IObjRW*>(Guided->get_target());
    Info->TargetId = (Target != nullptr) ? Target->get_id() : 0u;

    Info->TargetSubObjId = Guided->SubTarget;    // / module storage
    Info->SubObjId = SUBOBJ_ID_NONE;             // / module storage
}
