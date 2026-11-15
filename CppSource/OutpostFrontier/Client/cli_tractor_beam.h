// Author: Giperion.

#pragma once

// The tractor beam effect -- baseline Client/space (no source path string survives in the
// class's own code). Built only by the tractor equipment-FX wrapper's activation
// (the routine, 0x0052fa2b..0x0052fa60): EffectAlloc(0x84), the Effect base constructor
// with type 0x2d (EFT_TRACTORBEAM), +0x6c..+0x80 zeroed, vtable the branch.

// (9 slots, the Effect table layout):
//   +0x00 the branch  EffectBaseStart          +0x04 the routine EffectKill
//   +0x08 the branch  source position / range  +0x0c OnAddHost (RET 4, empty)
//   +0x10 the routine  `MOV AL,1`               +0x14 the branch -> the routine
//   +0x18 the branch  `MOV AL,1; RET 8`        +0x1c the branch the beam draw
//   +0x20 the routine  scalar deleting destructor (the routine, the routine(this, 0x84))

#include "effect_base.h"

struct FClientEquip;
struct FTexShape;

struct FCliTractorBeam : FEffectBase
{
    FCliTractorBeam();
    float GetWorldPos(FVector* Out) override;
    unsigned char IsAlive() override { return 1; }
    unsigned char UpdateVisibility(float Dt, FSpaceCamera* Cam) override;
    unsigned char TickDraw(float Dt, FSpaceCamera* Cam) override { return 1; }
    unsigned char BuildGeometry(FSpaceCamera* Cam) override;

    FClientEquip* Owner;   // +0x6c the tractor's equipment-FX wrapper (the routine)
    float Color[3];        // +0x70 the CliEQTractor's color bytes / 255 (the field)
    FTexShape* LineShape;  // +0x7c G_EffectShapeDB "line" record
    FTexShape* BulbShape;  // +0x80 G_EffectShapeDB "bulb" record
};
static_assert(sizeof(FCliTractorBeam) == 0x84, "CliTractorBeam 0x84");

// (the routine inlined): EffectAlloc(0x84); non-null ->
// EffectBaseConstruct(0x2d), +0x6c..+0x80 = 0, vtable the branch. Null when the
// allocation failed.
FCliTractorBeam* CliTractorBeamCreate();

// (thiscall, RET 4): Owner, colour, "line" / "bulb" shapes (each a ShapeDB find,
// FDUMP on a miss, false when missing), then the type-bucket link the routine. True on
// success.
unsigned char CliTractorBeamInit(FCliTractorBeam* This, FClientEquip* Owner);
