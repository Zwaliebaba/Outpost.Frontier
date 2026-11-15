// Author: Giperion.

#pragma once

// OutpostFrontier.exe fuse database and fuse-action classes -- baseline module storage-0052c7f1
// (source path Client\Space\...; RTTI names ClientFuse, ClientFuseAction, StartExplosionAction,
// StartCameraParticlesAction, StartEffectAction, ClientImpulseAction, ClientIgniteFuseAction).

// Common.dll only owns the Fuse / FuseAction base classes and the list of fuse INI paths
// (FuseDB::m_FuseINIFiles, filled by FuseDB::AddFuseINIFile). The exe reads those files itself
// (the routine) into its own map of ClientFuse prototypes (the field), creating one exe
// FuseAction subclass per action section. A ship ignites a fuse by copy-constructing the
// prototype into its own burn list (the routine, client_equip_fx.cpp).

// Port status: the database, the INI reader, the host binding, every constructor / clone / Read /
// slot-12 copy, the UnTriggers, every action's Trigger and every Load / UnLoad follow the listing.
// Slots a baseline class does not override fall through to the Common.dll FuseAction bodies.

#include "common_archetype.h" // CacheString
#include "common_csys.h"      // Matrix
#include "common_fuses.h"

#include <map>
#include <vector>

class FClientGameobj;
class INI_Reader;
struct FEffectArch; // vis_effect_arch.h

// ---------------------------------------------------------------------------
// ClientFuse -- ??_7ClientFuse@@6B@ (module storage): [the routine scalar deleting dtor
// (Fuse::~Fuse + delete), Fuse::FreeActions]. sizeof 0x38 = Fuse (0x34) + Host at +0x34.
// ---------------------------------------------------------------------------
class ClientFuse : public Fuse
{
public:
    // module storage-0052a89a: Fuse(ID_String{CreateID("")}, -1.0f), Host = 0.
    ClientFuse(ID_String const& Nickname = ID_String{CreateID("")}, float Lifetime = -1.0f);
    // module storage-005342e4 (and the node copy in the routine): the Common copy
    // constructor (which clones every action through FuseAction slot 1), then +0x34 copied.
    ClientFuse(ClientFuse const& Other);
    // Fuse::~Fuse only.
    ~ClientFuse() override;

    // reads one [fuse] section and the action sections that follow it.
    bool Read(INI_Reader& Reader);
    // stores the host game object in the fuse and in every action.
    void BindHost(FClientGameobj* NewHost);

    FClientGameobj* Host; // +0x34
};

// ---------------------------------------------------------------------------
// ClientFuseAction -- ??_7ClientFuseAction@@6B@ (module storage), 12 slots: the routine
// scalar deleting dtor, _purecall x2, every other slot the Common.dll FuseAction body. Only adds
// the host pointer at +0x24. Abstract, like FuseAction.
// ---------------------------------------------------------------------------
class ClientFuseAction : public FuseAction
{
public:
    // Inlined at every construction site (e.g. module storage-00529e9b): FuseAction(), +0x24 = 0.
    ClientFuseAction();
    // the inlined ~FuseAction (restore the Common vftable, free the sub-object
    // vector). the routine is its scalar deleting wrapper.
    ~ClientFuseAction() override;

    FClientGameobj* Host; // +0x24

protected:
    // the UnTrigger body shared (one address) by StartExplosionAction and
    // StartCameraParticlesAction's vftable slot 8. Unlike Common's FuseAction::UnTrigger it also
    // clears bTriggered once the sub-object list is empty.
    void UnTriggerAndSettle(unsigned short SubObjId);
};

// Slot 12 (+0x30) of every action subclass below is a new virtual that copies the archetype part
// of another instance of the same class (FuseAction::CopyArchProperties + the subclass fields);
// the slot-1 clones call it on the fresh object. Its baseline name is unknown, hence Slot30.

// ---------------------------------------------------------------------------
// StartExplosionAction -- vftable module storage, sizeof 0x40 ([start_explosion]).
// ---------------------------------------------------------------------------
class StartExplosionAction : public ClientFuseAction
{
public:
    StartExplosionAction();

    FuseAction* Slot04() const override;
    bool Slot08(INI_Reader& Reader) override;
    // the explosion archetype resolved into +0x3c and AddRef'd (the routine),
    // then FuseAction::Load (tail JMP).
    int Load() override;
    // the archetype released (the routine) when set, then FuseAction::UnLoad.
    int UnLoad() override;
    // (RET 8): unless already triggered, a process-2 explosion starts at the
    // host's hardpoint plus pos_offset as an Action_Explode (the routine) on the host's action
    // list; then the FuseAction::Trigger tail (inlined in baseline).
    void Trigger(unsigned SourceId, unsigned short SubObjId) override;
    void UnTrigger(unsigned short SubObjId) override;

protected:
    virtual void Slot30(StartExplosionAction const& Other);

public:
    ID_String Explosion;                      // +0x28 "explosion"
    CacheString Hardpoint;                    // +0x2c "hardpoint"
    Vector PosOffset;                         // +0x30 "pos_offset"
    Archetype::Explosion* ExplosionArch;      // +0x3c set by Load (the routine)
};

// ---------------------------------------------------------------------------
// StartCameraParticlesAction -- vftable module storage, sizeof 0x30 ([start_cam_particles]).
// ---------------------------------------------------------------------------
class StartCameraParticlesAction : public ClientFuseAction
{
public:
    StartCameraParticlesAction();

    FuseAction* Slot04() const override;
    bool Slot08(INI_Reader& Reader) override;
    // +0x2c = the routine(+0x28), a found arch referenced (the routine(0)); then
    // FuseAction::Load (tail JMP).
    int Load() override;
    // +0x2c released (the routine) when set, the pointer left as it is; then
    // FuseAction::UnLoad (tail JMP).
    int UnLoad() override;
    // (RET 8): unless already triggered, a named effect is started as a world
    // one-shot (the routine on the CameraParticles factory the field) at the host's
    // transform; then the FuseAction::Trigger tail (inlined in baseline).
    void Trigger(unsigned SourceId, unsigned short SubObjId) override;
    void UnTrigger(unsigned short SubObjId) override;

protected:
    virtual void Slot30(StartCameraParticlesAction const& Other); // (ICF)

public:
    ID_String Effect;         // +0x28 "effect"
    FEffectArch* EffectArch;  // +0x2c set by Load (the routine); Trigger looks the id up again
};

// One [start_effect] "hardpoint" entry: the 0x34-byte element of StartEffectAction+0x30.
struct FStartEffectHardpoint
{
    CacheString Hardpoint; // +0x00
    Matrix Orientation;    // +0x04 "ori_offset" (identity by default, the routine)
    Vector PosOffset;      // +0x28 "pos_offset"
};

// ---------------------------------------------------------------------------
// StartEffectAction -- vftable module storage, sizeof 0x54 ([start_effect]).
// ---------------------------------------------------------------------------
class StartEffectAction : public ClientFuseAction
{
public:
    StartEffectAction();
    // -> the routine: the effect companion at +0x44 released (twice: the inlined
    // release, then the companion member's own destructor), the hardpoint vector freed, then
    // ~FuseAction.
    ~StartEffectAction() override;

    FuseAction* Slot04() const override;
    bool Slot08(INI_Reader& Reader) override;
    // +0x40 = the routine(+0x28), a found arch referenced (the routine(0)); then
    // FuseAction::Load (tail JMP).
    int Load() override;
    // +0x40 released (the routine) when set, the pointer left as it is; then
    // FuseAction::UnLoad (tail JMP).
    int UnLoad() override;
    // (RET 8): unless already triggered, the effect starts at the host's root or
    // at one of the listed hardpoints (random start, first that resolves) -- attached to the
    // host's effect list (the routine) or as a world one-shot (the routine); then the
    // FuseAction::Trigger tail (inlined in baseline).
    void Trigger(unsigned SourceId, unsigned short SubObjId) override;
    void UnTrigger(unsigned short SubObjId) override;

protected:
    virtual void Slot30(StartEffectAction const& Other);

public:
    ID_String Effect;                               // +0x28 "effect"
    bool bAttached;                                 // +0x2c "attached"
    std::vector<FStartEffectHardpoint> Hardpoints;  // +0x30
    FEffectArch* EffectArch;                        // +0x40 set by Load (the routine)
    // +0x44: the effect companion {slot0, sound, particle, light} (the layout the routine fills
    // and the routine / the routine release). Constructed zero (the routine(0,0,0,0)).
    void* Companion[4]; // +0x44
};

// ---------------------------------------------------------------------------
// ClientImpulseAction -- vftable module storage, sizeof 0x40 ([impulse]).
// ---------------------------------------------------------------------------
class ClientImpulseAction : public ClientFuseAction
{
public:
    ClientImpulseAction();

    FuseAction* Slot04() const override;
    bool Slot08(INI_Reader& Reader) override;
    // (RET 8): unless already triggered, every ObjDB kind-10 object within radius
    // of the host's hardpoint (+ pos_offset) gets a linear impulse (PhySys::LinearImpulse);
    // then the FuseAction::Trigger tail (inlined in baseline).
    void Trigger(unsigned SourceId, unsigned short SubObjId) override;

protected:
    virtual void Slot30(ClientImpulseAction const& Other);

public:
    CacheString Hardpoint; // +0x28 "hardpoint" (NOT copied by Slot30 -- baseline)
    float Force;           // +0x2c "force"
    float Radius;          // +0x30 "radius"
    Vector PosOffset;      // +0x34 "pos_offset"
};

// ---------------------------------------------------------------------------
// ClientIgniteFuseAction -- vftable module storage, sizeof 0x30 ([ignite_fuse]).
// ---------------------------------------------------------------------------
class ClientIgniteFuseAction : public ClientFuseAction
{
public:
    ClientIgniteFuseAction();

    FuseAction* Slot04() const override;
    bool Slot08(INI_Reader& Reader) override;
    int Load() override;
    int UnLoad() override;

protected:
    virtual void Slot30(ClientIgniteFuseAction const& Other); // (ICF)

public:
    ID_String FuseId;  // +0x28 "fuse"
    bool bFuseLoaded;  // +0x2c
};

// ---------------------------------------------------------------------------
// The exe's fuse prototype database: std::map<unsigned, ClientFuse*> at .data:the field,
// keyed by the fuse nickname id. Derives from FuseDB to read the protected path list.
// ---------------------------------------------------------------------------
class ClientFuseDB : public FuseDB
{
public:
    static std::map<unsigned, ClientFuse*>& Get();
    static void Clear();
    static void LoadAll();

    // The `find` + `->second` every baseline caller inlines (the routine / the routine then
    // [node+0x10]). On a miss baseline reads the value slot of the VC6 tree head node, which
    // _Buynode never initialises; the port returns nullptr there, and every baseline caller
    // tests the loaded pointer for null before use.
    static ClientFuse* Find(unsigned FuseId);

private:
    static void LoadFile(char const* Path);        // (thiscall on the map)

    static std::map<unsigned, ClientFuse*> Fuses;
};

// One node of the host game object's burning-fuse list (FClientGameobj+0xc4, a VC6 std::list
// of ClientFuse; baseline node 0x40 = next, prev, ClientFuse 0x38).
struct FFuseBurnNode
{
    FFuseBurnNode* Next;
    FFuseBurnNode* Prev;
    ClientFuse Live;
};
