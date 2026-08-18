#pragma once

#include "ShipClass.h"

#include <span>

/*
 * How long a journey takes, from the class table (ADR-005 §2).
 *
 * The ghost's label says `18.4 km - ETA 41s` (`tactical-hud.png`), and this is
 * the second half of that. It is deliberately a *function of the content
 * tables* rather than a method on `World`, because two callers need it and only
 * one of them has a world: the client's pre-check answers the label before the
 * order is sent (`overlay-pass.png` §2 is explicit that the draw list's content
 * "comes from the client pre-check ... not from replication"), and S12's
 * authority answers it again per leg for the replicated order state. One
 * function means the number does not change when the source does.
 *
 * **It is a prediction, and the prediction is a straight run.** `World`'s
 * steering also slows a ship that is pointing the wrong way -- `desiredSpeed`
 * is scaled by alignment and capped by the turn radius -- and this ignores
 * both. Measured against the simulation across three hulls and three distances:
 *
 *   already aligned   0.4 % worst
 *   ninety degrees    4.7 % worst
 *   turned right round 33 %  worst (a Battleship on a 900 m hop)
 *
 * and **every one of them optimistic** -- the ship always takes at least as
 * long as this says. That is the direction the error has to run in: a player
 * told forty seconds who gets thirty is early, and one told forty who gets
 * fifty-three has planned around a number that was never achievable. It is
 * also the case S12's accept criterion excludes, by saying "ETA error < 10 %
 * on straight runs".
 *
 * **The prints' own numbers are illustrative and must not be matched.**
 * `puck-and-wheel.png` §2 labels 18.4 km as `1m 41s` and `tactical-hud.png`
 * labels the same distance `ETA 41s`; no speed in the class table produces
 * either from the other. They are layout, not arithmetic, and tuning this model
 * to reproduce one of them would be fitting the physics to a mock-up.
 */

namespace Game
{

/*
 * One member's journey: the hull that has to make it, and how far.
 *
 * A struct rather than two parallel spans for the reason `SolveFormation` gives
 * for taking a callback: a caller cannot pass one of the wrong length.
 */
struct TravelLeg
{
  HullClass hullClass = HullClass::Interceptor;
  float distanceMetres = 0.0f;

  /// How fast it is already going, along the way it is going. Zero for a
  /// journey that has not started, which is what a *preview* asks about; the
  /// authority's own ETA fills it in, because a group half way through a leg is
  /// not at rest and charging it a full acceleration ramp makes the number
  /// stall as the ships arrive.
  float speedMetresPerSec = 0.0f;
};

/*
 * Seconds for one hull to cross `_distanceMetres`, at rest at both ends.
 *
 * A trapezoid: accelerate at the class's rate, cruise at its top speed, brake
 * at the same rate. That is what `World::Integrate` actually does -- its
 * `ArrivalSpeed` is "the fastest you could still stop from", which is the
 * braking leg of exactly this profile -- so the estimate and the simulation
 * share a shape rather than merely a table.
 *
 * Short hops never reach top speed and get the triangular case, which matters
 * more than it looks: a station-keeping nudge of forty metres is the common
 * order, and `distance / maxSpeed` would call it instant.
 *
 * Returns **-1** for a hull that cannot make the journey at all -- a Structure
 * has no speed and no acceleration, and reporting 0 or a huge number would both
 * read as an answer. Callers show nothing rather than showing "never".
 */
[[nodiscard]] float TravelSeconds(HullClass _hullClass, float _distanceMetres) noexcept;

/*
 * The same journey, from a hull that is **already moving**.
 *
 * `TravelSeconds` is this with `_speedMetresPerSec` of zero, and the two agree
 * exactly there rather than approximately -- one is written in terms of the
 * other, so there is one profile and not two that have to be kept in step.
 *
 * This is the form the authority replicates (S12). A rest-to-rest estimate of a
 * *remaining* distance charges an acceleration ramp the ships already paid, and
 * the error grows as they arrive: a Battleship with a kilometre left is 7.5
 * seconds into a 9.5-second finish, and the rest-to-rest answer is 17. An ETA
 * that visibly stalls in the last few seconds is worse than no ETA, because it
 * is the moment a player is actually watching it.
 */
[[nodiscard]] float RemainingSeconds(HullClass _hullClass, float _distanceMetres, float _speedMetresPerSec) noexcept;

/*
 * The group's ETA: **the slowest member's own journey**, not the group's centre
 * over the group's mean speed.
 *
 * A group advances a leg when every member is within station tolerance
 * (ADR-005 §2), so the arrival that matters is the last one. Averaging would
 * promise a fleet of Interceptors and one Battleship the Interceptors' time,
 * which is the number that is wrong at exactly the moment the player is
 * counting on it.
 *
 * Members that can never arrive are skipped rather than making the whole answer
 * -1: a selection containing a station should still report when its ships get
 * there. Returns -1 only when *nothing* in the group can move.
 */
[[nodiscard]] float GroupTravelSeconds(std::span<const TravelLeg> _legs) noexcept;

} // namespace Game
