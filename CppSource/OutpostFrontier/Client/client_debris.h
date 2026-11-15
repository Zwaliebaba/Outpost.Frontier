// Author: Giperion.

#pragma once

// OutpostFrontier.exe Debris.cpp -- baseline Client\space\Debris.cpp: the [Debris]
// archetype table (debris= in DATA\OutpostFrontier.ini's [Data], the field) and the Debris
// gameobj (RTTI .?AVDebris@@) that carries a flying wreck piece until it explodes or vanishes.

//   Debris : GameObject   ctor inlined in the routine / the routine   0xc0
//                         IObjRW 005dddd4, IObjAffect 005ddd54 (GameObject's entries),
//                         CBase 005ddd4c -> the routine (body the routine)

// The piece's CObject is either handed over (the routine: explosion shatter pieces and parts
// torn off an EquippedObj) or created from a simple archetype (the routine: explosion
// innards).

#include "client_gameobj.h"
#include "common_archetype.h"
#include "common_id.h"

class INI_Reader;

// One [Debris] section (the routine reads it; the default below is the same values). The
// baseline map node keeps the nickname at +0x0c and this record at +0x10 (the routine returns
// node + 0x10), 0x24 bytes (the MOVSD.REP ECX=9 at 0x00530caa).
struct FDebrisArch
{
    // The defaults both baseline builders store inline -- the section reader's local
    // (0x00530a30..0x00530a74) and the static initialiser of the default record the branch:
    // 0, FLT_MAX, 1, both ids cleared, 0, {0, 0, 0}.
    FDebrisArch();

    float LifetimeMin;    // +0x00  lifetime = min[, max]; default 0
    float LifetimeMax;     // +0x04  default FLT_MAX (0x7f7fffff); max < min -> min
    int DeathMethod;       // +0x08  death_method: "exploding" 0, "vanishing" 1; default 1
    ID_String Explosion;   // +0x0c  explosion = nickname (CreateID)
    ID_String Trail;       // +0x10  trail = effect nickname (CreateID)
    float LinearDrag;      // +0x14  linear_drag
    Vector AngularDrag;    // +0x18  angular_drag = x, y, z
};
static_assert(sizeof(FDebrisArch) == 0x24, "Debris archetype 0x24");

// (cdecl) -- read one [Debris] section. A nicknamed section goes into the table;
// a nickname already there is kept and, unless bQuiet, FDUMPs the duplicate warning. True
// only when the section was added.
bool ReadDebrisArch(INI_Reader* Reader, bool bQuiet);
// (cdecl) -- read every [Debris] section of Path. Returns how many entries the
// table gained. Caller: DataConfig's debris= (0x005b0e3d, bQuiet = false).
int LoadDebrisArchFile(char const* Path, bool bQuiet);
// empty the table (main.cpp's shutdown, 0x005b22a5).
void ShutdownDebrisArchDb();
// (cdecl) -- the entry named *Nickname, null when there is none.
FDebrisArch* FindDebrisArch(ID_String const* Nickname);
// &the field, the default record (static initialiser the branch).
FDebrisArch* GetDefaultDebrisArch();

class FClientDebrisGameobj : public FClientGameobj
{
public:
    // (0xa), +0xb4 = 0, +0xb8 = 0, +0xbc = 1, Debris's vtables (0x00530656..
    // ). +0xa8..+0xb0 are left for the routine.
    FClientDebrisGameobj();
    // (this = the CBase sub-object); deleting wrapper the routine. Releases the
    // explosion (the routine) when there is one, then GameObject's destructor.
    ~FClientDebrisGameobj() override;

    // IObjInspect 34 +0x88 the branch (RET 4): *OutType = 0x80000000, returns 0.
    int get_type(uint& OutType) const override;

    // 90 +0x168 the routine (RET 4): the routine(1, Arch), then the object's vt+0x4c
    // open(Arch).
    void create(Archetype::Root* Arch) override;
    // 91 +0x16c the routine: XOR AL,AL / RET 4 (the body Beam shares).
    bool find(unsigned const& Id) override;
    // 93 +0x174 the branch (RET 4): the object's update, GameObject's update, then the
    // lifetime countdown -- explode (slot 108) or vanish (slot 87) at 0.
    void update(float Dt) override;
    // 95 +0x17c the routine (RET 0xc): GameObject's LOD pick; a picked LOD whose low word is
    // below 3, or whose high word is 2 or more, clears Field_0xbc.
    unsigned* Slot17c(unsigned* OutLod, void* Camera, unsigned const* CachedLod) override;
    // 108 +0x1b0 the branch (RET 4): with an explosion, the routine(Explosion, this, Pos or
    // get_position()) into the action list; without one, GameObject's slot 108.
    void Slot1b0(Vector const* Pos) override;

    // (RET 4): bind the piece to its [Debris] record -- GameObject's slot 92, the
    // record's lifetime / death method / explosion, the trail effect, the drag, the nebula.
    void Init(FDebrisArch const* Arch);

    // baseline keeps the death method as the dword at +0xa8, over GameObject's bWatchListed
    // byte; the port's GameObject owns that byte, so the dword moves to +0xac and Init also
    // stores its low byte into bWatchListed the way the baseline store does.
    int DeathMethod;                     // baseline +0xa8  FDebrisArch +0x08
    float LifetimeMin;                   // baseline +0xac  FDebrisArch +0x00
    float LifetimeMax;                   // baseline +0xb0  FDebrisArch +0x04
    Archetype::Explosion* Explosion;     // baseline +0xb4  GetExplosion(record +0x0c)
    float Remaining;                     // baseline +0xb8  rand between min and max, counts down
    unsigned char Field_0xbc;            // baseline +0xbc  ctor 1; the routine clears it. TODO:
                                         //  meaning (the LOD's low word < 3 or high word >= 2)
    unsigned char Padbd[3];
};

// (cdecl) -- a Debris gameobj around an existing object: new Debris, +0x10 =
// Object, CBase::advise(true), Init(Arch), the routine(Debris, 0). Callers: the explosion
// shatter the routine (0x00536995) and EquippedObj the routine (0x00533d77).
FClientDebrisGameobj* CreateDebrisGameobjForObject(CObject* Object, FDebrisArch const* Arch);
// (cdecl) -- a Debris gameobj built from a simple archetype at Pos: Arch->load,
// the companion's Load(1), create(Arch), Arch->Release, the position, PhySys::BuildIVP_Real,
// Init(DebrisArch), the routine(Debris, 0). Caller: the explosion innards the routine.
FClientDebrisGameobj* CreateDebrisGameobjFromArch(Archetype::Root* Arch,
                                                  FDebrisArch const* DebrisArch,
                                                  Vector const* Pos);
