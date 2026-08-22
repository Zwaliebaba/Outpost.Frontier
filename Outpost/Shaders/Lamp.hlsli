#ifndef OUTPOST_LAMP_HLSLI
#define OUTPOST_LAMP_HLSLI

// What LampVS and LampPS both have to agree about (ADR-006 §6a).
//
// One additive glow billboard per lamp per ship. The lamp table is static and
// hull-local; the transform comes from the *opaque pass's own instance stream*,
// bound again here, so a fleet costs no per-lamp CPU work at all -- the draw
// picks a lamp with a root constant and instances it over every ship of the
// class.

#include "FrameConstants.hlsli"

// Must match LampMode in SignalLamp.h. The shader compares rather than
// switches, so these are the wire between the two files.
#define LAMP_MODE_STEADY 0
#define LAMP_MODE_STROBE 1
#define LAMP_MODE_PULSE 2
#define LAMP_MODE_CHASE 3

// The brightness envelope, and the same numbers `SignalLamp.cpp` declares. A
// lamp never reaches zero: an unlit fitting is still a fitting, and one that
// vanishes reads as a hole in the hull.
#define LAMP_OPACITY_MIN 0.18f
#define LAMP_OPACITY_MAX 0.80f
#define LAMP_SCALE_SWELL 0.28f

/*
 * The lamps, all of them, for every class.
 *
 * A fixed array in a root CBV rather than a structured buffer: the whole table
 * is under five kilobytes, it is written once at boot, and a root CBV costs no
 * descriptor heap slot and no barrier. `LAMP_TABLE_CAPACITY` in GpuLamps.h is
 * the same number, and the buffer is created at that size whatever the corpus
 * actually uses -- so an index past the last lamp reads zeroes and draws
 * nothing rather than reading whatever the heap came with.
 *
 * Three registers per lamp, which is `sizeof(LampInstance)` exactly. The mode
 * rides in a float lane because a cbuffer array element is padded to 16 bytes
 * regardless, so packing it there is free.
 */
#define LAMP_TABLE_CAPACITY 96

struct LampDefinition
{
  float4 positionAndSize; // xyz = hull-local position, w = size in metres.
  float4 colourAndMode;   // rgb = linear colour, w = LampMode.
  float4 timing;          // x = phase turns, y = rate Hz, z = duty, w = floor.
};

cbuffer LampConstants : register(b3)
{
  LampDefinition g_lamps[LAMP_TABLE_CAPACITY];
};

cbuffer LampDrawConstants : register(b2)
{
  uint g_lampIndex;
};

// Per instance -- the same stream the opaque pass binds, minus the fields a
// lamp has no use for. The input layout declares only what is read here; the
// stride is InstanceRecord's, so the offsets are the ones it already had.
struct LampInstanceInput
{
  float3 instancePosition : INSTANCE_POSITION;
  float instanceHeading : INSTANCE_HEADING;
  float instanceBank : INSTANCE_BANK;
  float instanceLampPhase : INSTANCE_LAMPPHASE; // 0..1, hashed from the entity id.
};

struct VertexOutput
{
  float4 clipPosition : SV_Position;
  float2 local : LOCAL;    // The quad corner, -1..1 on both axes.
  float3 colour : COLOUR;
  nointerpolation float opacity : OPACITY;
};

/*
 * The animation. Edited in step with `Neuron::LampLevel` in SignalLamp.cpp,
 * which is the same four branches and is the copy a test can reach.
 *
 * One unsynced wall clock drives every lamp in the frame, and the per-lamp and
 * per-instance phases are what stop that reading as a single flashing object:
 * the lamp's own phase spaces a chase around its ring, and the instance's --
 * hashed from the ship's id -- spaces one hull's strobe against its neighbour's.
 */
float LampAnimatedLevel(LampDefinition _lamp, float _instancePhaseTurns, float _seconds)
{
  const float turns = _seconds * _lamp.timing.y + _lamp.timing.x + _instancePhaseTurns;
  const float cycle = frac(turns);
  const uint mode = (uint)_lamp.colourAndMode.w;

  if (mode == LAMP_MODE_STROBE)
  {
    return cycle < _lamp.timing.z ? 1.0f : 0.0f;
  }
  if (mode == LAMP_MODE_PULSE)
  {
    return _lamp.timing.w + (1.0f - _lamp.timing.w) * (0.5f + 0.5f * sin(turns * 6.28318530718f));
  }
  if (mode == LAMP_MODE_CHASE)
  {
    return cycle < _lamp.timing.z ? 1.0f : _lamp.timing.w;
  }
  return 1.0f; // Steady.
}

#endif // OUTPOST_LAMP_HLSLI
