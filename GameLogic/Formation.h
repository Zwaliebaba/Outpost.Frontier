#pragma once

#include "Ids.h"
#include "Orders.h"

#include <DirectXMath.h>

#include <cstdint>
#include <span>

/*
 * Where an order puts each ship (ADR-005 §3).
 *
 * A pure function, and the client calls the same one for the order puck's
 * footprint preview -- the corpus demands the footprint be "the real formation
 * solve, one tick per ship, never decorative". A decorative ellipse would be a
 * lie the player discovers by issuing the order, which is the worst moment to
 * discover it.
 *
 * **Assignment is by ascending ShipId.** Stable, cheap, and independent of
 * array order, which matters twice: replay determinism (ADR-005 §5 forbids
 * anything but dense-array order, and this is a sort of ids rather than of
 * pointers), and parity (the client's view and the server's tables order ships
 * differently, and a solve that depended on that would put a preview one
 * station out). A cost-minimising assignment is a later drop-in inside this
 * signature.
 *
 * **Spacing is the largest member's.** A Line of Interceptors is tight and a
 * Line with a Carrier in it is not, because the spacing that keeps the Carrier
 * clear is the spacing that keeps everything clear. `ShipClassTable` carries
 * `formationSpacingMetres` per class for exactly this.
 */

namespace Game
{

/// One ship, once the formation has decided where it goes. Metres, because this
/// is what steering consumes; the leg that produced it was quantised, and this
/// is downstream of that.
struct FormationStation
{
  ShipId shipId = INVALID_SHIP_ID;
  DirectX::XMFLOAT2 positionMetres{};
};

/*
 * Solves `_formation` for `_shipIds` around an anchor.
 *
 * `_hullClassOf` maps a ship id to its hull class, because the solve needs
 * spacing and neither the world's tables nor the client's replicated view is
 * available to a pure function that both must call. A callback rather than a
 * parallel array so a caller cannot pass one of the wrong length.
 *
 * Writes at most `_outStations.size()` stations and returns how many. Ships
 * beyond that are not placed -- the caller sized the buffer, and a solve that
 * grew it would be a solve that allocates on the tick thread.
 */
[[nodiscard]] std::uint32_t SolveFormation(FormationId _formation, std::span<const ShipId> _shipIds,
                                           HullClass (*_hullClassOf)(ShipId, void*), void* _context,
                                           const DirectX::XMFLOAT2& _anchorMetres, float _anchorFacingRadians,
                                           std::span<FormationStation> _outStations) noexcept;

/// The radius of a solved arrangement, for the ring the order puck draws around
/// it. Zero for an empty solve.
[[nodiscard]] float FormationExtentMetres(std::span<const FormationStation> _stations,
                                          const DirectX::XMFLOAT2& _anchorMetres) noexcept;

} // namespace Game
