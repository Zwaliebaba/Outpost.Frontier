#pragma once

#include "OverlayMark.h"
#include "RenderWorld.h"
#include "UiDrawList.h"

#include <cstdint>

/*
 * How much one frame may put through the upload ring (ADR-018 A20).
 *
 * **Sized from what this client is built to draw, never from what it currently
 * draws.** That is A20's rule and it is the whole reason this file exists as
 * arithmetic rather than as a number: content grows between slices and a
 * constant picked from today's content is a constant that will be wrong on the
 * day the map lands, with no test failing and nobody looking. The ceilings
 * below are the corpus caps, and the total is derived from them and from the
 * strides the input layouts declare -- so a stream that grows a field moves
 * this number by itself.
 *
 * **The failure it is sized against is silent and total.** Every pass writes
 * its stream as one allocation and returns if it does not fit
 * (`GpuPasses.cpp`), so a frame short of ring memory does not draw a partial
 * HUD -- it draws *no* HUD, counts a drop, and looks exactly like a HUD that
 * was not asked for. At 256 KiB, the size this replaced, the strategic map's
 * own instances are four times the whole frame's segment: U5's acceptance as
 * written would have rendered a blank screen until somebody bumped a constant
 * mid-slice, which is precisely what A20 is scheduled before U5 to prevent.
 *
 * **Device-free**, which is what lets the number be tested rather than only
 * logged: nothing here reaches D3D12, and `GpuUploadRing` is handed the answer
 * rather than asked for it.
 *
 * The total is a **default**, not a ceiling on the ceiling:
 * `client.renderer.uploadBytesPerFrame` overrides it, so a shard running
 * unusual content can be retuned without a rebuild (ADR-012). Zero there means
 * "use this", which is what a config written before this existed says.
 */

namespace Neuron
{

/*
 * Hulls one grid may hold (ADR-018 D4), and the client draws one grid.
 *
 * The cap is normative rather than aspirational -- ADR-022's whole job is
 * delivering a bounded view of it -- so a frame that wanted more instances than
 * this would be a frame drawing a grid the authority is not allowed to produce.
 */
inline constexpr std::uint32_t MAX_DRAWN_ENTITIES = 1024;

/*
 * Marks one hull can wear at once, counted from the producers rather than from a
 * glance at a screen: a selection ring, a stale marker and the two gauge bars
 * (`BuildOverlayMarks`), plus a status marker (`BuildStatusMarks`).
 *
 * All five together is unusual and not impossible -- a selected, damaged,
 * shimmering ship whose extrapolation has frozen wears every one -- and
 * "unusual" is not a size to budget from.
 *
 * **It is exact rather than generous, and measured rather than read.** A full
 * grid in that state emits five marks a hull and this reserves five, which is
 * only safe because `OverlayMarkCeilingTests` measures it: a sixth producer
 * breaks the test on the day it is added rather than on the day a busy frame
 * finds the ring short. Counting it by eye is how it was first got wrong here
 * -- `BuildStatusMarks` lives in a different function from the other four and
 * was missed, which is a thousand marks a frame.
 */
inline constexpr std::uint32_t OVERLAY_MARKS_PER_ENTITY = 5;

/*
 * And the transit rings, which are the one overlay stream not bounded by what
 * is *on* the grid (`BuildTransitMarks`).
 *
 * A ring lasts a second and marks a hull arriving or leaving, so the worst case
 * is a grid that turned over inside one: every previous occupant still fading
 * out while every new one fades in. Two per entity is that, and it is the
 * honest ceiling rather than the likely one.
 */
inline constexpr std::uint32_t TRANSIT_MARKS_PER_ENTITY = 2;

/*
 * Plus the order ghosts, which are marks about *intent* rather than about a
 * hull: one footprint each, and up to a preview's worth of stations behind it.
 */
inline constexpr std::uint32_t MAX_OVERLAY_MARKS =
    MAX_DRAWN_ENTITIES * (OVERLAY_MARKS_PER_ENTITY + TRANSIT_MARKS_PER_ENTITY) +
    static_cast<std::uint32_t>(OrderGhostList::MAX_GHOSTS) * (1u + MAX_ORDER_PREVIEW_MARKS);

/*
 * The largest surface this client is built to draw, and it is the strategic map
 * rather than anything in the HUD (ADR-016 §9, U5).
 *
 * At the corpus's full size that is a quad per system, a segment per gate link,
 * and a glyph per character of every label -- and the glyphs dominate by an
 * order of magnitude, which is the part that is easy to forget when sizing from
 * "how many things are on screen".
 *
 * **Stated in the client's own terms on purpose.** 2,500 is the corpus's system
 * count, but the engine may not name a game constant to say so (ADR-014), and
 * it would be the wrong dependency even if it could: this is a statement about
 * what this renderer is built for, and a shard whose content outgrows it is a
 * shard that retunes the config rather than one that rebuilds the client.
 */
inline constexpr std::uint32_t MAX_MAP_NODES = 2500;

/*
 * And the gate graph over them, which is the same statement about the seam.
 *
 * **Here rather than beside the graph type, so the two cannot disagree.** The
 * cap the client will *accept* across the seam and the quads it has *room* for
 * are one number; declaring it twice is exactly the drift ADR-013 §3's
 * duplicate-constant guard exists to stop, and that guard would not have caught
 * it -- one of the two was an unnamed `3000u` in an arithmetic expression.
 *
 * **Measured, and it is exactly what the corpus produces** (U5, 2026-08-23):
 * the full 2,500-system bake makes 6,000 directed gates, which is 3,000 links
 * stored once. So there is no headroom at all, and that is the same bargain
 * `MAX_MAP_NODES` states one line up -- a bake that grows past it is a config
 * retune rather than a rebuild. What makes it safe to leave tight is that the
 * builder reports what it wrote: a truncated graph is a graph missing its tail
 * and saying so, never a graph that quietly lost a gate.
 */
inline constexpr std::uint32_t MAX_MAP_LINKS = 3000;

/*
 * And the constellation hulls over those, which the map draws at its two
 * coarsest levels.
 *
 * 512 where the corpus produces 250, and the looser bargain is deliberate: a
 * constellation count is `regionCount × constellationsPerRegion` -- *two* config
 * numbers multiplied -- so the tight cap `MAX_MAP_NODES` takes would be tripped
 * by a change to either. It costs nothing to hold: a hull is a centre and a
 * radius, and they are not instanced.
 */
inline constexpr std::uint32_t MAX_MAP_GROUPS = 512;

inline constexpr std::uint32_t MAX_UI_INSTANCES = MAX_MAP_NODES        // A quad per system.
                                                  + MAX_MAP_NODES * 6u // Its label, at six glyphs.
                                                  + MAX_MAP_LINKS      // One oriented quad each.
                                                  + 2048u;             // The HUD over the top of it all.

/*
 * Room for the frame's constant buffers, which are two small writes rounded up
 * to a 256-byte boundary each.
 *
 * Budgeted as a slab rather than as `sizeof` the two structs, deliberately: it
 * is four kilobytes against a megabyte, and pinning it to the current pair
 * would mean a pass that adds a constant buffer has to find this line. The
 * headroom is cheaper than the coupling.
 */
inline constexpr std::uint32_t UPLOAD_CONSTANT_BYTES_PER_FRAME = 4096;

/*
 * The default segment size, per frame in flight.
 *
 * `GpuUploadRing::Create` multiplies by the number of frames, so the buffer
 * this actually reserves is this times three -- a few megabytes of upload heap,
 * which is not a number worth economising on against a HUD that vanishes.
 */
inline constexpr std::uint32_t UPLOAD_BYTES_PER_FRAME =
    MAX_DRAWN_ENTITIES * static_cast<std::uint32_t>(sizeof(InstanceRecord)) +
    MAX_OVERLAY_MARKS * static_cast<std::uint32_t>(sizeof(OverlayMark)) +
    MAX_UI_INSTANCES * static_cast<std::uint32_t>(sizeof(UiInstance)) + UPLOAD_CONSTANT_BYTES_PER_FRAME;

/*
 * A floor under a hand-edited config, and it is the *entity* streams it
 * protects rather than the total.
 *
 * A ring too small to hold one grid's hulls and their marks cannot draw the
 * tactical view at all, which is a broken game rather than a retuned one. The
 * map's share is left out: a shard that never opens the strategic map may
 * legitimately want the smaller ring, and it will get an honest counted drop if
 * it is wrong about that.
 */
inline constexpr std::uint32_t MIN_UPLOAD_BYTES_PER_FRAME =
    MAX_DRAWN_ENTITIES * static_cast<std::uint32_t>(sizeof(InstanceRecord)) +
    MAX_OVERLAY_MARKS * static_cast<std::uint32_t>(sizeof(OverlayMark)) + UPLOAD_CONSTANT_BYTES_PER_FRAME;

/*
 * The facts the sum has to keep, stated where they cannot rot.
 *
 * Compile-time rather than a test, because they are arithmetic about arithmetic
 * -- a test would restate the expression and pass for the wrong reason. What a
 * *test* is for is the input to that arithmetic, and that is what
 * `OverlayMarkCeilingTests` covers: whether the per-entity mark count is still
 * what the builders emit.
 */
static_assert(UPLOAD_BYTES_PER_FRAME >= MAX_DRAWN_ENTITIES * sizeof(InstanceRecord) +
                                          MAX_OVERLAY_MARKS * sizeof(OverlayMark) + MAX_UI_INSTANCES * sizeof(UiInstance),
              "the budget no longer covers a frame at every ceiling it names: a stream was dropped from the sum");

static_assert(MIN_UPLOAD_BYTES_PER_FRAME < UPLOAD_BYTES_PER_FRAME,
              "the floor has met the default, which means the map's share has gone missing from one of them");

/*
 * And the number this replaced, kept as a floor with its reason attached.
 *
 * 256 KiB was not a bad guess -- it is about what the tactical view costs, and
 * `MIN_UPLOAD_BYTES_PER_FRAME` lands near it from the arithmetic. What it was
 * missing was the map, and an assertion that the new number is larger is the
 * cheapest guard against somebody "tidying" the sum back to a round constant.
 */
static_assert(UPLOAD_BYTES_PER_FRAME > 256u * 1024u,
              "the derived budget is no larger than the fixed 256 KiB it replaced, which was too small for the map");

} // namespace Neuron
