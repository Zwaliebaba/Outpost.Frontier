// Author: Giperion.

#pragma once

// Live per-type effect instance lists — the field/ce4/ce8 (Effect.cpp).
// Walked by the routine / SpaceEffectsDrawFrame. Filled by the routine / the routine
// when an effect instance activates (not by [EffectType] INI meta — that is the routine).

// Neighbor contract (vis_effect_arch / Effect spawn):
 //   After constructing a live Effect* with EffectTypeId at +0x5C, call
 //   EffectTypeListLink(Effect) so SpaceEffectsDrawFrame's bucket walk sees it.
 //   Link fields live at Effect+0x60 (prev), +0x64 (next), +0x68 (self-when-linked).
 //   Type id at +0x5C must be in [0, 0x2f).

// 0x2f buckets: the static initializer the branch zeroes exactly 0x2f {Count, Head, Tail}
// triples from the field, and the teardown walk the routine stops at 0x00674f18 (47 buckets);
// is a different object.
constexpr int kEffectTypeBucketCount = 0x2f;

// One baseline bucket: count / head / tail (stride 3 dwords from the field).
struct FEffectTypeBucket
{
    int Count;   // [i]
    void* Head;  // [i] — points at Effect+0x60 link node
    void* Tail;  // [i]
};

extern FEffectTypeBucket G_EffectTypeBuckets[kEffectTypeBucketCount];

// — prepend Node into Bucket (Node = Effect+0x60).
void EffectTypeBucketPushFront(FEffectTypeBucket* Bucket, void* Node);

// — link Effect into G_EffectTypeBuckets[Effect+0x5C] if not yet linked
// and flag bit1 at Effect+0x8 clear. Increments Effect+0x58.
void EffectTypeListLink(void* Effect);

// The full baseline unlink-and-release (0x004f7910 / 0x004f716a / 0x004f73ea): linked only;
// +0x68 = 0, node out of the bucket, count--, then RefCount-- and, at <= 0, slot +0x20 (1).
void EffectTypeListUnlinkAndRelease(void* Effect);

// — every live effect in every bucket: slot +4 (kill), then unlink-and-release.
void EffectTypeListKillAll();
