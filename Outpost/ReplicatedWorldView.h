#pragma once

#include "ReplicatedView.h"
#include "WorldView.h"

#include "FleetSummary.h"
#include "Station.h"
#include "SummaryView.h"

#include <cstdint>
#include <span>
#include <string>
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
  /// One station's display name, by the anchor it stands on.
  struct StationName
  {
    Game::AnchorId anchor = Game::INVALID_ID;
    std::string name;
  };

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

    /*
     * What each wing is called, indexed by `WingId`. Index 0 is
     * `INVALID_WING_ID` and is never drawn -- the stations belong to no wing.
     *
     * Here rather than in GameLogic because the MVP's fleet is authored in the
     * composition root (`MakeStartingWorld`) and its names belong beside it. A
     * fleet file would move both together; splitting them now would put the
     * ships in one place and their call signs in another.
     */
    std::vector<std::string> wingNames;

    /// The universe hash, which is what the handshake fails closed on.
    std::uint64_t contentHash = 0;

    /*
     * Which anchor the grid this client watches stands on, from the `Welcome`
     * (ADR-017 §8).
     *
     * The client had no way to *name* a station before this existed: it could
     * be told about one and still had no number to address one with, so a Dock
     * could be validated and never composed. The composition root supplies it
     * because the `Welcome` is the engine's message and the anchor's meaning
     * is the game's.
     */
    Game::AnchorId gridAnchor = Game::INVALID_ID;

    /*
     * What each station is called, for the panel that lists the ones this
     * commander has ships in (ADR-017 1).
     *
     * A pair list rather than a table indexed by anchor: anchor ids are unique
     * across the whole universe and nothing makes them dense, so an array would
     * be one string slot per anchor in a galaxy to name the handful that are
     * stations.
     *
     * Here rather than in GameLogic for the same reason `wingNames` is: the
     * name lives in the parsed universe, and the composition root is the one
     * project holding both that and the client's vocabulary.
     */
    std::vector<StationName> stationNames;
  };

  static constexpr std::uint16_t INVALID_RENDER_CLASS = 0xffffu;

  explicit ReplicatedWorldView(Desc _desc);

  [[nodiscard]] std::uint32_t ApplySnapshot(std::span<const std::uint8_t> _payload) override;
  void BuildScene(double _renderTick, Neuron::RenderScene& _outScene) override;

  [[nodiscard]] Neuron::OrderVerdict PreCheck(const Neuron::OrderIntent& _intent) override;
  void SolvePreview(const Neuron::OrderIntent& _intent, Neuron::OrderPreview& _outPreview) override;
  [[nodiscard]] bool EncodeOrder(const Neuron::OrderIntent& _intent, Neuron::ByteWriter& _writer) override;
  [[nodiscard]] Neuron::OrderDefaults DefaultOrder() const override;
  [[nodiscard]] std::uint32_t OrderOptions(std::uint16_t _kind, std::span<Neuron::OrderOption> _outOptions) const override;
  [[nodiscard]] std::uint32_t OrderKinds(std::span<Neuron::OrderKindOption> _outKinds) const override;
  [[nodiscard]] std::uint32_t BuildRoster(std::span<const std::uint16_t> _selectedIds,
                                          std::span<Neuron::RosterRow> _outRows) const override;
  [[nodiscard]] bool ContextActionFor(std::uint16_t _entityId, std::span<const std::uint16_t> _selectedIds,
                                      Neuron::ContextAction& _outAction) const override;
  [[nodiscard]] bool ApplySummary(std::span<const std::uint8_t> _payload) override;
  [[nodiscard]] std::uint32_t BuildDockedBlocks(std::span<Neuron::DockedBlock> _outBlocks) const override;
  [[nodiscard]] std::uint32_t PollNotices(std::span<Neuron::Notice> _outNotices) override;
  void PollOrderFeedback(Neuron::OrderFeedback& _outFeedback) override;
  [[nodiscard]] const char* ReasonText(std::uint16_t _reasonCode) const override;

  [[nodiscard]] std::uint64_t SchemaHash() const override;
  [[nodiscard]] std::uint64_t ContentHash() const override { return m_desc.contentHash; }

  /// What the HUD will ask for (S11), and what a test asserts against.
  [[nodiscard]] std::uint16_t ShipCount() const noexcept { return m_view.LatestShipCount(); }
  [[nodiscard]] std::uint32_t LatestTick() const noexcept { return m_view.LatestTick(); }
  [[nodiscard]] std::uint64_t RejectedSnapshotCount() const noexcept { return m_rejectedSnapshots; }
  [[nodiscard]] std::uint64_t RejectedSummaryCount() const noexcept { return m_summary.RejectedFrames(); }

  /// What the summary family last said is docked where, for a test to assert
  /// against without going through the HUD's span.
  [[nodiscard]] std::uint16_t DockedCountAt(Game::AnchorId _anchor) const noexcept;

  /// The decoded family itself, for the surfaces that will read the economy's
  /// four kinds (E5b). Exposed rather than forwarded one accessor at a time,
  /// because forwarding six queries would be six lines of nothing -- and what
  /// the seam to NeuronClient should look like is a screen question that is
  /// still open (D-P2, D-P3), so nothing is invented here to answer it early.
  [[nodiscard]] const Game::SummaryView& Summaries() const noexcept { return m_summary; }

private:
  /*
   * Raises the dock/undock toasts for the difference between the counts this
   * held and the ones that just arrived.
   *
   * `_hadSummary` is passed in rather than read, because by the time this runs
   * the view has already accepted the frame and would answer "yes" to a
   * question about the one before it -- and the whole point of the flag is that
   * **the first frame of a session is a state and not a set of events**.
   */
  void NoteRosterChanges(std::span<const Game::DockedStationView> _next, bool _hadSummary);

  /// What the universe calls the station on this anchor, or null for one the
  /// content does not name.
  [[nodiscard]] const char* StationNameFor(Game::AnchorId _anchor) const;

  Desc m_desc;
  Game::ReplicatedView m_view;

  /// Reused across frames rather than allocated per frame: this runs once per
  /// frame at whatever rate the display asks for.
  std::vector<Game::ReplicatedShip> m_sampled;

  /*
   * Which replicated entity is this grid's station, refreshed by each scene
   * build.
   *
   * Found by hull class rather than carried on the wire: a `Structure` on the
   * grid the `Welcome` anchored is the station, and a field saying so would
   * cost every ship a byte to describe one entity. `INVALID_SHIP_ID` until a
   * frame has been built, which is also the honest answer for a grid whose
   * station has not arrived in a snapshot yet.
   */
  mutable Game::ShipId m_stationEntityId = Game::INVALID_SHIP_ID;

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

  /*
   * The decoded summary family (`Game::SummaryView`, E5a).
   *
   * The decode used to be a `switch` here, which was defensible while it was
   * two kinds and wiring; at six it is bounds checking, staging and a refusal
   * policy, which is logic, and ADR-014 6 says this project does not hold
   * logic. It moved to GameLogic where it can be proved without a device --
   * and where the four economy kinds this file never read finally are.
   */
  Game::SummaryView m_summary;

  /*
   * The docked counts as of the previous frame, for the toasts and nothing
   * else.
   *
   * A dock finishing is an *event* and the only evidence of it is that a count
   * went up, so the message is the difference between two frames -- which means
   * something has to remember the earlier one. Counts rather than whole blocks,
   * because the rosters are what the diff explicitly does not look at: a ship
   * list that arrived one frame and was dropped by the byte budget the next is
   * not sixty undockings.
   */
  struct DockedCount
  {
    Game::AnchorId anchor = Game::INVALID_ID;
    std::uint16_t shipCount = 0;
  };
  std::vector<DockedCount> m_dockedCountsLastFrame;

  /*
   * What the game has to say, waiting to be drained (`PollNotices`).
   *
   * The bodies are owned here because the seam hands over pointers: a station
   * name lives in `Desc` and outlives everything, but "3 SHIPS" is composed per
   * notice and has to live somewhere until the client has copied it.
   */
  struct PendingNotice
  {
    std::uint16_t code = 0;
    const char* title = nullptr;
    std::string body;
  };
  std::vector<PendingNotice> m_notices;

  /// Handed across the seam by the last poll, and kept alive until the next one
  /// because that is exactly what the seam promises.
  std::vector<PendingNotice> m_noticesHandedOver;

  std::uint64_t m_rejectedSnapshots = 0;
};

} // namespace Outpost
