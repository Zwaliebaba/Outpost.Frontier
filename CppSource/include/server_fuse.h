// Author: Giperion.

#pragma once

// Server.dll fuse database and fuse-action classes -- baseline module storage-06cffc97
// (RTTI: ServerFuse, ServerFuseAction, IgniteFuseAction, DestroyRootAction,
// DamageRootAction, DestroyGroupAction, DamageGroupAction,
// DestroyHardpointAttachmentAction, DamageHardpointAttachmentAction, ImpulseAction,
// TumbleAction, MakeInvincibleAction, DumpCargoAction -- module storage .. 06d88a68).

// Common.dll owns only the Fuse / FuseAction base classes and the list of fuse INI paths
// (FuseDB::m_FuseINIFiles). Server.dll reads those files itself (the routine) into its own
// map of ServerFuse prototypes (the field), creating one Server FuseAction subclass per
// action section. An EqObj ignites a fuse by copy-constructing the prototype into its own burn
// list (EqObj light_fuse, module storage, vtable +0x1e4) and binding itself as the owner
// (the routine). The OutpostFrontier.exe counterpart of this file is
// src/OutpostFrontier/Client/client_fuse.h.

// Port status: the database (load / teardown / accessor), the INI reader, the owner binding,
// every constructor / clone / Read / slot-12 copy and every Trigger / UnTrigger override follow
// the listing.

// Every damage Trigger builds its DamageList the same way (baseline inlines the DamageList
// constructor): cause 0x15, inflictor = the source id, inflictor owner = the source object's
// CSimple+0xb4 when the owner sits in a StarSystem that knows the source (the routine, mask
// 0x7f0000). The EqObj / GameObject slots they call (+0x188, +0x194, +0x1c0 .. +0x1e8) are
// declared in server_gameobject.h.

#include "common_archetype.h" // CacheString
#include "common_fuses.h"
#include "common_ini.h"       // INI_Reader, Vector

#include <map>

class EqObj;

// ---------------------------------------------------------------------------
// ServerFuse -- vftable module storage: [module storage scalar deleting dtor (Fuse::~Fuse +
// delete), Fuse::FreeActions]. sizeof 0x3c = Fuse (0x34) + Owner +0x34 + bHasDestroyRoot +0x38.
// ---------------------------------------------------------------------------
class ServerFuse : public Fuse
{
public:
    // module storage-06cfd5aa: Fuse(ID_String{CreateID("")}, -1.0f), +0x34 = 0,
    // +0x38 = 0.
    ServerFuse(ID_String const& Nickname = ID_String{CreateID("")}, float Lifetime = -1.0f);
    // EqObj light_fuse module storage-06cec5b9 (inlined): the Common copy constructor (which
    // clones every action through FuseAction slot 1), then +0x34 and +0x38 copied.
    ServerFuse(ServerFuse const& Other);
    // Fuse::~Fuse only.
    ~ServerFuse() override;

    // reads one [fuse] section and the action sections that follow it.
    bool Read(INI_Reader& Reader);
    // stores the owning object in the fuse and in every action.
    void SetOwner(EqObj* NewOwner);

    EqObj* Owner;                   // +0x34
    // +0x38. Set by Read when a [destroy_root] section is met (module storage); read by EqObj
    // light_fuse (module storage) to decide whether igniting this fuse kills the object.
    bool bHasDestroyRoot;
};

// ---------------------------------------------------------------------------
// ServerFuseAction -- vftable module storage, 12 slots: the routine scalar deleting dtor,
// _purecall x2, every other slot the Common.dll FuseAction body. Only adds the owner pointer at
// +0x24. Abstract, like FuseAction.
// ---------------------------------------------------------------------------
class ServerFuseAction : public FuseAction
{
public:
    // Inlined at every construction site (e.g. module storage-06cfc6d8): FuseAction(),
    // +0x24 = 0.
    ServerFuseAction();
    // the inlined ~FuseAction (restore the Common vftable, free the sub-object
    // vector, zero its three pointers). the routine is its scalar deleting wrapper, shared by
    // every subclass vftable.
    ~ServerFuseAction() override;

    EqObj* Owner; // +0x24
};

// Slot 12 (+0x30) of every action subclass below except DestroyRootAction is a new virtual
// that copies the archetype part of another instance of the same class
// (FuseAction::CopyArchProperties + the subclass fields); the slot-1 clones call it on the fresh
// object. Its baseline name is unknown, hence Slot30 (the same convention as client_fuse.h).

// ---------------------------------------------------------------------------
// IgniteFuseAction -- vftable module storage, sizeof 0x30 ([ignite_fuse]).
// ---------------------------------------------------------------------------
class IgniteFuseAction : public ServerFuseAction
{
public:
    IgniteFuseAction();

    FuseAction* Slot04() const override;
    bool Slot08(INI_Reader& Reader) override;
    void Trigger(unsigned SourceId, unsigned short SubObjId) override;
    // the only UnTrigger override in the family (slot 8).
    void UnTrigger(unsigned short SubObjId) override;

protected:
    virtual void Slot30(IgniteFuseAction const& Other);

public:
    ID_String FuseId; // +0x28 "fuse"
    float FuseT;      // +0x2c "fuse_t"
};

// ---------------------------------------------------------------------------
// DestroyRootAction -- vftable module storage, sizeof 0x28 ([destroy_root]). No slot 12: the
// clone copies through CopyArchProperties (slot 11) directly.
// ---------------------------------------------------------------------------
class DestroyRootAction : public ServerFuseAction
{
public:
    DestroyRootAction();

    FuseAction* Slot04() const override;
    bool Slot08(INI_Reader& Reader) override;
    void Trigger(unsigned SourceId, unsigned short SubObjId) override;
};

// ---------------------------------------------------------------------------
// DamageRootAction -- vftable module storage, sizeof 0x30 ([damage_root]).
// ---------------------------------------------------------------------------
class DamageRootAction : public ServerFuseAction
{
public:
    DamageRootAction();

    FuseAction* Slot04() const override;
    bool Slot08(INI_Reader& Reader) override;
    void Trigger(unsigned SourceId, unsigned short SubObjId) override;

protected:
    virtual void Slot30(DamageRootAction const& Other);

public:
    int Hitpoints;    // +0x28 "hitpoints"
    bool bCanKill;   // +0x2c "damage_type": stricmp "absolute" -> 0, anything else -> 1; the bCanKill of EqObj +0x1c0/+0x1c4/+0x1cc
};

// ---------------------------------------------------------------------------
// DestroyGroupAction -- vftable module storage, sizeof 0x30 ([destroy_group]).
// ---------------------------------------------------------------------------
class DestroyGroupAction : public ServerFuseAction
{
public:
    DestroyGroupAction();

    FuseAction* Slot04() const override;
    bool Slot08(INI_Reader& Reader) override;
    void Trigger(unsigned SourceId, unsigned short SubObjId) override;

protected:
    virtual void Slot30(DestroyGroupAction const& Other); // (ICF)

public:
    CacheString GroupName; // +0x28 "group_name"
    int Fate;              // +0x2c "fate": "debris" -> 2, "disappear" -> 3
};

// ---------------------------------------------------------------------------
// DamageGroupAction -- vftable module storage, sizeof 0x34 ([damage_group]).
// ---------------------------------------------------------------------------
class DamageGroupAction : public ServerFuseAction
{
public:
    DamageGroupAction();

    FuseAction* Slot04() const override;
    bool Slot08(INI_Reader& Reader) override;
    void Trigger(unsigned SourceId, unsigned short SubObjId) override;

protected:
    virtual void Slot30(DamageGroupAction const& Other);

public:
    CacheString GroupName; // +0x28 "group_name"
    int Hitpoints;         // +0x2c "hitpoints"
    bool bCanKill;        // +0x30 "damage_type": exact "absolute" -> 0, anything else -> 1; the bCanKill of EqObj +0x1c0/+0x1c4/+0x1cc
};

// ---------------------------------------------------------------------------
// DestroyHardpointAttachmentAction -- vftable module storage, sizeof 0x30
// ([destroy_hp_attachment]).
// ---------------------------------------------------------------------------
class DestroyHardpointAttachmentAction : public ServerFuseAction
{
public:
    DestroyHardpointAttachmentAction();

    FuseAction* Slot04() const override;
    bool Slot08(INI_Reader& Reader) override;
    void Trigger(unsigned SourceId, unsigned short SubObjId) override;

protected:
    virtual void Slot30(DestroyHardpointAttachmentAction const& Other); // (ICF)

public:
    CacheString Hardpoint; // +0x28 "hardpoint"
    int Fate;              // +0x2c "fate": "debris" -> 2, "disappear" -> 3, "loot" -> 5
};

// ---------------------------------------------------------------------------
// DamageHardpointAttachmentAction -- vftable module storage, sizeof 0x34
// ([damage_hp_attachment]).
// ---------------------------------------------------------------------------
class DamageHardpointAttachmentAction : public ServerFuseAction
{
public:
    DamageHardpointAttachmentAction();

    FuseAction* Slot04() const override;
    bool Slot08(INI_Reader& Reader) override;
    void Trigger(unsigned SourceId, unsigned short SubObjId) override;

protected:
    virtual void Slot30(DamageHardpointAttachmentAction const& Other); // (ICF)

public:
    CacheString Hardpoint; // +0x28 "hardpoint"
    int Hitpoints;         // +0x2c "hitpoints"
    bool bCanKill;        // +0x30 "damage_type": exact "absolute" -> 0, anything else -> 1; the bCanKill of EqObj +0x1c0/+0x1c4/+0x1cc
};

// ---------------------------------------------------------------------------
// ImpulseAction -- vftable module storage, sizeof 0x44 ([impulse]).
// ---------------------------------------------------------------------------
class ImpulseAction : public ServerFuseAction
{
public:
    ImpulseAction();

    FuseAction* Slot04() const override;
    bool Slot08(INI_Reader& Reader) override;
    void Trigger(unsigned SourceId, unsigned short SubObjId) override;

protected:
    virtual void Slot30(ImpulseAction const& Other);

public:
    CacheString Hardpoint; // +0x28 "hardpoint"
    float Force;           // +0x2c "force"
    float Radius;          // +0x30 "radius"
    float Damage;          // +0x34 "damage"
    Vector PosOffset;      // +0x38 "pos_offset"
};

// ---------------------------------------------------------------------------
// TumbleAction -- vftable module storage, sizeof 0x50 ([tumble]).
// ---------------------------------------------------------------------------
class TumbleAction : public ServerFuseAction
{
public:
    TumbleAction();

    FuseAction* Slot04() const override;
    bool Slot08(INI_Reader& Reader) override;
    void Trigger(unsigned SourceId, unsigned short SubObjId) override;

protected:
    virtual void Slot30(TumbleAction const& Other);

public:
    float AngDragScale;      // +0x28 "ang_drag_scale" (1.0f by default)
    // "turn_throttle_x/y/z": value 0 goes to TurnThrottleLow, value 1 to TurnThrottleHigh.
    Vector TurnThrottleLow;  // +0x2c (0, 0, 0 by default)
    Vector TurnThrottleHigh; // +0x38 (1, 1, 1 by default)
    float ThrottleLow;       // +0x44 "throttle" value 0
    float ThrottleHigh;      // +0x48 "throttle" value 1
    bool bEngineKill;        // +0x4c "engine_kill"
};

// ---------------------------------------------------------------------------
// MakeInvincibleAction -- vftable module storage, sizeof 0x2c ([make_invincible]).
// ---------------------------------------------------------------------------
class MakeInvincibleAction : public ServerFuseAction
{
public:
    MakeInvincibleAction();

    FuseAction* Slot04() const override;
    bool Slot08(INI_Reader& Reader) override;
    void Trigger(unsigned SourceId, unsigned short SubObjId) override;

protected:
    virtual void Slot30(MakeInvincibleAction const& Other);

public:
    bool bTurnOn; // +0x28 "turn_on" (1 by default)
};

// ---------------------------------------------------------------------------
// DumpCargoAction -- vftable module storage, sizeof 0x2c ([dump_cargo]).
// ---------------------------------------------------------------------------
class DumpCargoAction : public ServerFuseAction
{
public:
    DumpCargoAction();

    FuseAction* Slot04() const override;
    bool Slot08(INI_Reader& Reader) override;
    void Trigger(unsigned SourceId, unsigned short SubObjId) override;

protected:
    virtual void Slot30(DumpCargoAction const& Other);

public:
    CacheString OriginHardpoint; // +0x28 "origin_hardpoint"
};

// ---------------------------------------------------------------------------
// The Server fuse prototype database: std::map<unsigned, ServerFuse*> at .data:the field,
// keyed by the fuse nickname id. Derives from FuseDB to read the protected path list.
// ---------------------------------------------------------------------------
class ServerFuseDB : public FuseDB
{
public:
    static std::map<unsigned, ServerFuse*>& Get();
    static void Clear();
    static void LoadAll();

private:
    static void LoadFile(char const* Path);        // (thiscall on the map)

    static std::map<unsigned, ServerFuse*> Fuses;
};

// "random" (module storage) interned once at load time: the group / hardpoint name the
// DestroyGroupAction (the branch -> the field) and DestroyHardpointAttachmentAction
// (the branch -> the field) Trigger bodies compare against with stricmp.
extern char const* GRandomGroupName;
extern char const* GRandomHardpointName;
