#pragma once

#include "ReplicatedView.h"
#include "WorldView.h"

#include <cstdint>
#include <vector>

/*
 * The client's world, from the server (Build Order S7).
 *
 * This replaces `ParkedFleetView`, which invented a fleet because there was
 * nothing to replicate yet. There is now: the client draws what the server
 * simulates, interpolated between snapshots, and nothing on screen is local
 * fiction. That is F10 made structural rather than promised -- kill the feed and
 * the fleet freezes and flags itself, because there is no other source for it
 * to fall back on.
 *
 * **The station is a ship.** It used to arrive as authored scenery converted
 * into `ScenePlacement`s by the composition root, alongside an invented fleet.
 * Now the server spawns it as a `Structure` -- a hull with zero speed and zero
 * turn rate (ADR-005 §1) -- and it replicates through the same twenty bytes as
 * everything else. One path, and a station that can be selected, targeted and
 * interpolated by the code that already does those things.
 *
 * **Why it lives in the executable:** ADR-014 §2a. `WorldView.h` is
 * NeuronClient's and GameLogic may not include it, so the composition root holds
 * the vtable and forwards. Everything here is a line of wiring or a table.
 */

namespace Outpost
{

class ReplicatedWorldView final : public Neuron::WorldView
{
public:
  struct Desc
  {
    /*
     * `HullClass` to render classId, and the only place the two orderings meet.
     *
     * The game's taxonomy is the icon sheet's closed eleven, ordered so wire
     * values and palette indices never renumber (ADR-009 §6). The renderer's
     * ids are the order of the mesh list in `Outpost.json`, smallest hull to
     * largest. They are different orderings of overlapping sets, and neither
     * side should learn the other's -- so the mapping is here, in the one
     * project entitled to know both.
     *
     * Indexed by `HullClass`; `INVALID_RENDER_CLASS` for a hull with no mesh,
     * which is drawn as nothing rather than as something wrong.
     */
    std::vector<std::uint16_t> renderClassByHull;

    /// The universe hash, which is what the handshake fails closed on.
    std::uint64_t contentHash = 0;
  };

  static constexpr std::uint16_t INVALID_RENDER_CLASS = 0xffffu;

  explicit ReplicatedWorldView(Desc _desc);

  [[nodiscard]] std::uint32_t ApplySnapshot(std::span<const std::uint8_t> _payload) override;
  void BuildScene(double _renderTick, Neuron::RenderScene& _outScene) override;

  [[nodiscard]] Neuron::OrderVerdict PreCheck(const Neuron::OrderIntent& _intent) override;
  void SolvePreview(const Neuron::OrderIntent& _intent, Neuron::OrderPreview& _outPreview) override;
  [[nodiscard]] bool EncodeOrder(const Neuron::OrderIntent& _intent, Neuron::ByteWriter& _writer) override;
  [[nodiscard]] Neuron::OrderDefaults DefaultOrder() const override;
  void PollOrderFeedback(Neuron::OrderFeedback& _outFeedback) override;
  [[nodiscard]] const char* ReasonText(std::uint16_t _reasonCode) const override;

  [[nodiscard]] std::uint64_t SchemaHash() const override;
  [[nodiscard]] std::uint64_t ContentHash() const override { return m_desc.contentHash; }

  /// What the HUD will ask for (S11), and what a test asserts against.
  [[nodiscard]] std::uint16_t ShipCount() const noexcept { return m_view.LatestShipCount(); }
  [[nodiscard]] std::uint32_t LatestTick() const noexcept { return m_view.LatestTick(); }
  [[nodiscard]] std::uint64_t RejectedSnapshotCount() const noexcept { return m_rejectedSnapshots; }

private:
  Desc m_desc;
  Game::ReplicatedView m_view;

  /// Reused across frames rather than allocated per frame: this runs once per
  /// frame at whatever rate the display asks for.
  std::vector<Game::ReplicatedShip> m_sampled;

  /*
   * The ids `ValidateOrder` is given, refilled beside `m_sampled`.
   *
   * The same ships the frame drew, which is what makes a pre-check answer the
   * question the player actually asked: they clicked something they could see,
   * so the validation runs against what was on screen (ADR-006 §11). A separate
   * array because `ValidationView` wants them contiguous, and because a ship
   * this build has no mesh for is drawable-and-orderable in neither.
   *
   * A pre-check therefore runs against the *previous* frame's list, the client
   * ordering before it extracts. That is one frame on top of the two ticks the
   * replicated view is already behind by design (ADR-002 §4), and it is the
   * same reason ADR-005 §4 says the authority's verdict is the only one that
   * counts: a pre-check exists to make the common refusals instant, not to be
   * right about all of them.
   */
  std::vector<Game::ShipId> m_validationIds;

  std::uint64_t m_rejectedSnapshots = 0;
};

} // namespace Outpost
