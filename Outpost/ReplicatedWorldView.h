#pragma once

#include "ReplicatedView.h"
#include "WorldView.h"

#include "EconomyDef.h"
#include "EconomyMessages.h"
#include "Validate.h"
#include "FleetSummary.h"
#include "Station.h"
#include "Universe.h"
#include "UniverseRoute.h"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
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
  /// One anchor's display name. Not stations only: a fleet can be standing on a
  /// planet's grid, a gate's or a field's, and a block that cannot name where it
  /// is draws a question mark at the player.
  struct AnchorName
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

    /*
     * Call signs held back for wings the *player* makes (ADR-017 §6).
     *
     * A wing is emergent -- it exists iff a ship carries its number -- so
     * creating one costs nothing on the wire and everything on the screen: the
     * roster draws the wings it has a *name* for, deliberately, so a wing with
     * no call sign is a wing the player was never told about and cannot pick
     * out of a list. Somebody therefore has to supply a word at the moment the
     * player presses the button, and it is this project, for `wingNames`'
     * reason exactly: the call signs are content, and the composition root is
     * where this game's content is spelled.
     *
     * Consumed in order and never returned: a wing that empties keeps its name
     * for the same reason an authored one does (see `BuildRoster`), so handing
     * a used call sign to a second wing would put one word on two things the
     * player has told apart.
     *
     * **They are the save's now, not only the session's** (N2, 2026-08-22).
     * ADR-017 §6 puts wing names in ADR-012's user settings layer, and that
     * layer is written: a name minted today comes back tomorrow through
     * `savedWingNames` below, and the call sign it spent is struck off this
     * list at construction so a second wing is never handed the same word.
     * `EnsureWingName`'s dull fallback is what covers a wing whose name did not
     * survive -- a save from before the layer existed, or one a player deleted.
     */
    std::vector<std::string> spareWingNames;

    /*
     * The call signs the player has already chosen, from the user settings
     * layer (ADR-012 §3, ADR-017 §6).
     *
     * **Renames and names-for-new-wings are the same thing here**, which is
     * what ADR-017 §6 means by both living in that layer: an entry naming a
     * wing the content already named is a rename and outranks the authored
     * word, and an entry naming a number beyond the authored table is a wing
     * the player composed. One list covers both because the player cannot tell
     * them apart either -- they renamed a row.
     *
     * Applied by the constructor rather than merged into `wingNames` by the
     * caller, because the row cap and the pointer-stability reserve are this
     * class's invariants and an entry that breaks them has to be refused where
     * they are enforced.
     */
    std::vector<std::pair<Game::WingId, std::string>> savedWingNames;

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
     * What each anchor is called, for the panel that lists the places this
     * commander has ships (ADR-017 1, ADR-016 9).
     *
     * **Every anchor kind, not only stations**, and that was a defect before it
     * was a design: the panel began as the hangar's docked roster, so the table
     * held station names, and the first time a fleet stood on a grid that was
     * not a station's the block drew `?` at the player. A location block can be
     * anywhere a fleet can be.
     *
     * A pair list rather than a table indexed by anchor: anchor ids are unique
     * across the whole universe and nothing makes them dense, so an array would
     * be one string slot per anchor in the galaxy to name the few in play.
     *
     * Here rather than in GameLogic for the same reason `wingNames` is: the
     * name lives in the parsed universe, and the composition root is the one
     * project holding both that and the client's vocabulary.
     */
    std::vector<AnchorName> anchorNames;

    /*
     * The parsed economy content, or null for a build without it.
     *
     * Borrowed rather than copied -- the composition root loads it once and
     * outlives every view. It is here because two client-side answers need
     * authored numbers and must not re-author them: how much room a hull's ore
     * hold has, and what a unit of each ore displaces. A client that hard-coded
     * either would be a second copy of the balance file, drifting the moment
     * somebody retuned the real one (ADR-024 7).
     */
    const Game::EconomyDef* economy = nullptr;

    /*
     * The parsed universe, or null for a client with none.
     *
     * Borrowed rather than copied, like `economy` and for its reason: the
     * composition root loads it once and outlives every view. It is here
     * because the strategic map's seam needs two different things from it --
     * a graph to hand over once at boot, and a route solved on demand
     * (ADR-018 D14) -- and the second cannot be pre-derived into a `Desc`
     * field because nobody knows yet where the player will point.
     *
     * A view with none answers the map calls with nothing, and the surface
     * opens on an empty viewport rather than refusing.
     */
    const Game::UniverseDef* universe = nullptr;
  };

  static constexpr std::uint16_t INVALID_RENDER_CLASS = 0xffffu;

  explicit ReplicatedWorldView(Desc _desc);

  [[nodiscard]] bool ApplyFrame(const Neuron::ReplicatedFrame& _frame) override;
  void BuildScene(double _renderTick, Neuron::RenderScene& _outScene) override;

  [[nodiscard]] Neuron::OrderVerdict PreCheck(const Neuron::OrderIntent& _intent) override;
  void SolvePreview(const Neuron::OrderIntent& _intent, Neuron::OrderPreview& _outPreview) override;
  [[nodiscard]] bool EncodeOrder(const Neuron::OrderIntent& _intent, Neuron::ByteWriter& _writer) override;
  [[nodiscard]] Neuron::OrderDefaults DefaultOrder() const override;
  [[nodiscard]] std::uint32_t OrderOptions(std::uint16_t _kind, std::span<Neuron::OrderOption> _outOptions) const override;
  [[nodiscard]] std::uint32_t OrderKinds(std::span<const EntityId> _selectedIds,
                                         std::span<Neuron::OrderKindOption> _outKinds) const override;
  [[nodiscard]] std::uint32_t BuildRoster(std::span<const EntityId> _selectedIds,
                                          std::span<Neuron::RosterRow> _outRows) const override;
  [[nodiscard]] bool ContextActionFor(Neuron::EntityId _entityId, std::span<const Neuron::EntityId> _selectedIds,
                                      Neuron::ContextAction& _outAction) const override;
  [[nodiscard]] bool ApplySummary(std::span<const std::uint8_t> _payload) override;
  [[nodiscard]] std::uint32_t BuildLocationBlocks(std::span<Neuron::LocationBlock> _outBlocks) const override;
  [[nodiscard]] bool BuildFieldReadout(Neuron::FieldReadout& _outReadout) const override;

  /*
   * --- the station surface (ADR-017 §6, ADR-020 §6) ------------------------
   */
  [[nodiscard]] std::uint32_t BuildStationTabs(std::uint16_t _anchor,
                                               std::span<Neuron::StationTab> _outTabs) const override;
  [[nodiscard]] Neuron::StationRosterCounts BuildStationRoster(std::uint16_t _anchor,
                                                               std::span<const std::uint32_t> _selectedIds,
                                                               std::span<Neuron::StationGroup> _outGroups,
                                                               std::span<Neuron::StationChip> _outChips) const override;
  [[nodiscard]] std::uint32_t BuildGroupMembers(std::uint16_t _groupId,
                                                std::span<Neuron::EntityId> _outIds) const override;
  [[nodiscard]] bool RenameGroup(std::uint16_t _groupId, std::string_view _name) override;
  [[nodiscard]] std::uint32_t BuildStationActions(std::uint16_t _anchor,
                                                  std::span<Neuron::StationAction> _outActions) const override;
  [[nodiscard]] std::uint32_t StationActionOptions(std::uint16_t _verb,
                                                   std::span<Neuron::OrderOption> _outOptions) const override;
  [[nodiscard]] Neuron::OrderVerdict PreCheckStation(const Neuron::StationIntent& _intent) override;
  [[nodiscard]] bool EncodeStationCommand(const Neuron::StationIntent& _intent, Neuron::ByteWriter& _writer) override;
  [[nodiscard]] std::uint32_t ShipsPerStationCommand() const override;

  /*
   * --- the strategic map (ADR-018 D14, ADR-016 §9, U5) ---------------------
   *
   * The composition root doing what ADR-014 §6 says it is for: reading the
   * universe and handing over neutral data. Every game word on that screen --
   * a system's name, its security badge, an overlay's name and why it is off,
   * a route's ETA -- is written here, and the engine draws it without learning
   * what any of it means.
   */
  [[nodiscard]] bool BuildMapTopology(Neuron::MapTopology& _outTopology) const override;
  [[nodiscard]] std::uint32_t BuildMapOverlays(std::span<Neuron::MapOverlayOption> _outOverlays) const override;
  [[nodiscard]] std::uint32_t BuildMapLegend(std::uint16_t _overlay,
                                             std::span<Neuron::MapLegendRow> _outRows) const override;
  [[nodiscard]] std::uint32_t BuildMapFacts(std::uint16_t _systemId,
                                            std::span<Neuron::MapFactRow> _outRows) const override;
  [[nodiscard]] std::uint32_t SolveMapRoute(std::uint16_t _toSystem, std::span<Neuron::MapRouteHop> _outHops,
                                            Neuron::MapRouteSummary& _outSummary) const override;

  /// Which system this client's view is standing in, for the route's origin and
  /// for the top bar's region line. `INVALID_ID` before a world has arrived.
  [[nodiscard]] Game::SystemId MapOriginSystem() const noexcept;
  [[nodiscard]] std::uint32_t PollNotices(std::span<Neuron::Notice> _outNotices) override;
  void PollOrderFeedback(Neuron::OrderFeedback& _outFeedback) override;
  [[nodiscard]] const char* ReasonText(std::uint16_t _reasonCode) const override;

  [[nodiscard]] std::uint64_t SchemaHash() const override;
  [[nodiscard]] std::uint64_t ContentHash() const override { return m_desc.contentHash; }

  /// What the HUD will ask for (S11), and what a test asserts against.
  [[nodiscard]] std::uint16_t ShipCount() const noexcept { return m_view.LatestShipCount(); }
  [[nodiscard]] std::uint32_t LatestTick() const noexcept { return m_view.LatestTick(); }
  [[nodiscard]] std::uint64_t RejectedSnapshotCount() const noexcept { return m_rejectedSnapshots; }
  [[nodiscard]] std::uint64_t RejectedSummaryCount() const noexcept { return m_rejectedSummaries; }

  /// What the summary family last said is docked where, for a test to assert
  /// against without going through the HUD's span.
  [[nodiscard]] std::uint16_t DockedCountAt(Game::AnchorId _anchor) const noexcept;

  /*
   * A `StationIntent` as the game's own command, or false.
   *
   * One place rather than two, because `PreCheckStation` and
   * `EncodeStationCommand` must judge and send the *same* command -- a screen
   * whose pre-check said yes to something slightly different from what went on
   * the wire would have BounceParity in name only.
   *
   * False when the intent names more ships than one command holds. A refusal
   * rather than a truncation: sending the first 64 of 66 would undock most of a
   * fleet and report success, and splitting into waves is the screen's job to
   * have done already.
   */
  [[nodiscard]] bool MakeStationCommand(const Neuron::StationIntent& _intent,
                                        Game::StationCommand& _outCommand) const noexcept;

  /*
   * The call signs the player owns, for the settings layer to keep
   * (ADR-012 §3).
   *
   * Handed out as a copy rather than a span: the composition root reads this
   * once, at shutdown, and a borrowed view of a member whose whole purpose is
   * pointer stability is a loan nobody needs to take.
   *
   * Empty for a player who has renamed nothing and composed no wing, which is
   * what keeps an untouched installation free of a settings file at all.
   */
  [[nodiscard]] std::vector<std::pair<Game::WingId, std::string>> PlayerWingNames() const;

  /// What the economy summaries last said, for a test or a diagnostic to assert
  /// against without going through a HUD span.
  [[nodiscard]] std::size_t SiteStatusCount() const noexcept { return m_siteStatus.size(); }
  [[nodiscard]] std::size_t CargoRowCount() const noexcept { return m_cargo.size(); }
  [[nodiscard]] std::size_t BayCount() const noexcept { return m_bays.size(); }

private:
  /*
   * Where the summary family last said this commander's ships are -- **all
   * three states**, one entry per place.
   *
   * Docked only, until U3b: the panel began as the hangar's roster and grew the
   * other two cases the summaries had been carrying all along. Keeping them in
   * one list rather than three is what makes the panel one loop and the toasts
   * one comparison.
   *
   * Replaced wholesale on each arrival rather than merged: a frame is a
   * complete statement about where this commander's ships are, and a merge
   * would keep a place the authority has stopped listing -- which on this
   * panel is a hangar that never empties, or a crossing that never lands.
   *
   * Sorted by anchor so the panel's order is the universe's rather than
   * arrival's: a list that reshuffles because a datagram came in a different
   * sequence is a list the player cannot point at.
   */
  struct FleetPlace
  {
    Game::AnchorId anchor = Game::INVALID_ID;

    /// Which of the three ways this fleet is somewhere (ADR-016 6). Kept rather
    /// than collapsed to a bool, because the panel says the word and the toasts
    /// only ever compare the docked ones.
    Game::FleetState state = Game::FleetState::OnGrid;

    /// Seconds until a crossing lands, or `FLEET_ETA_NONE`. Meaningless for the
    /// two states that are already somewhere.
    std::uint16_t etaSeconds = Game::FLEET_ETA_NONE;

    /// From the *fleet summary* row rather than from `docked.size()`, and the
    /// difference matters: the summaries are always written while a roster is
    /// dropped first when the frame runs out of room, so this is the count that
    /// survives a truncated frame and the list behind it is what may not.
    std::uint16_t shipCount = 0;

    /// The ships themselves, for the hangar screen T3 builds. Empty for a
    /// station whose roster did not fit.
    std::vector<Game::RosterEntry> docked;
  };

  /*
   * The half of the world the shared validator judges against, filled from what
   * this client actually knows.
   *
   * One function because there is one answer: `PreCheck` and the command row's
   * availability must be judged against the *same* view, or the row would offer
   * a verb the pre-check refuses a frame later. It is also the whole of
   * ADR-014 3's parity claim on this side -- what the client cannot fill, it
   * leaves empty, and the validator's optional-field rules turn that into "the
   * authority decides" rather than into a wrong answer.
   */
  /*
   * The strategic map's graph, built once in the constructor.
   *
   * **Built rather than derived per call**, because `MapTopology` hands over
   * spans into storage the game owns and promises they stay valid for the
   * session -- so the vectors are filled once and never touched again. The
   * `const char*` in a node points either into the universe's own `std::string`
   * (a system's name, which the bake owns and the composition root outlives) or
   * into `m_mapBadges`, which is why that one is sized and filled *before* the
   * nodes that point into it.
   *
   * Empty for a client with no universe, which `BuildMapTopology` reports as
   * false and the screen draws as an empty viewport.
   */
  std::vector<Neuron::MapNode> m_mapNodes;
  std::vector<Neuron::MapLink> m_mapLinks;
  std::vector<Neuron::MapGroup> m_mapGroups;
  std::vector<Neuron::MapRegion> m_mapRegions;

  /// `SystemId` to node index, so a route's hops can be reported as indices --
  /// which is what `MapRouteHop` takes, for `MapLink`'s reason. `0xffff` for a
  /// system with no node, which authored content cannot produce.
  std::vector<std::uint16_t> m_mapNodeBySystem;

  /// Backing storage for the badges the nodes and regions point at -- `SEC 0.2`
  /// and `FRONTIER`. Filled before the records that borrow from it, and never
  /// resized after, which is `m_playerWingNames`' invariant one file along.
  std::vector<std::string> m_mapBadges;

  /// And for the route panel, which is rebuilt on each solve rather than at
  /// boot: a route is a question about where the player pointed.
  mutable std::vector<std::string> m_routeBadges;
  mutable std::string m_routeNote;
  mutable std::string m_routeEta;

  /// The selected system's facts, same arrangement: rebuilt per query, and the
  /// span handed out points into it until the next one.
  mutable std::vector<std::string> m_factValues;

  /// Turns the parsed universe into the neutral graph. Called once, from the
  /// constructor, for the lifetime reason above.
  void BuildMapGraph();

  /// This grid's station, or `INVALID_ID` before one has been drawn. One
  /// function because the validation view, the submit and the command row's
  /// availability must all mean the same station or none.
  [[nodiscard]] Game::AnchorId StationAnchor() const noexcept;

  [[nodiscard]] Game::ValidationView MakeValidationView() const noexcept;

  /*
   * Whether a kind whose acceptance depends only on the selection would be
   * taken right now, and the reason if not.
   *
   * **Only kinds that name no destination can be asked this**, which is a real
   * property rather than a special case for Mine: a Move, a Warp or a Dock is
   * judged partly on *where it points*, and there is no point until the player
   * makes the gesture, so pre-judging one would refuse orders the authority
   * would take. A Mine names no destination at all -- the field a wing works is
   * the one it is standing in (ADR-024 4a) -- so everything that decides it is
   * already on screen.
   */
  [[nodiscard]] Game::OrderVerdict SelectionOnlyVerdict(Game::OrderKind _kind,
                                                        std::span<const Neuron::EntityId> _selectedIds) const noexcept;

  /// Free ore-hold litres per ship, parallel to `m_validationIds`, from the
  /// cargo summary and the authored hold sizes. Empty when either is missing,
  /// which the validator reads as "the authority decides".
  void FillHoldRoom(std::vector<std::uint32_t>& _outFree) const;

  /// Raises the dock/undock toasts for the difference between what is held and
  /// what just arrived. Called *before* the new list replaces the old one,
  /// because the comparison is the whole content of the message.
  void NoteRosterChanges(const std::vector<FleetPlace>& _next);

  /// What the universe calls this anchor, or null for one the content does not
  /// name.
  [[nodiscard]] const char* AnchorNameFor(Game::AnchorId _anchor) const;

  /*
   * --- what a wing is called (ADR-017 §6) ----------------------------------
   *
   * Two tables behind one lookup: the authored call signs the composition root
   * handed over, and the ones this session has minted for wings that were not
   * in it. They stay apart rather than being one growing vector because the
   * authored table is indexed by `WingId` and dense from 1, and a player who
   * picked wing 200 would otherwise cost 191 empty strings to say so.
   */

  /// This wing's call sign, or null for a wing nothing has named -- which is
  /// what keeps it off the roster. Never asked about `INVALID_WING_ID`.
  [[nodiscard]] const char* WingName(Game::WingId _wing) const noexcept;

  /*
   * Gives this wing a name if it has none, and answers whether it now has one.
   *
   * Called from the two places a wing id arrives from outside -- a snapshot and
   * a station roster -- so a wing the player made in an earlier session, whose
   * number survived in the save while its name did not, comes back as a row
   * with a dull word on it rather than as ships that have left the roster
   * entirely. That failure is the one worth engineering against: a fleet the
   * player cannot see is worse than a fleet called WING 9.
   *
   * False past the roster's row cap. A name for a wing that can never be drawn
   * is a name for nothing, and minting one would spend a call sign on it.
   */
  bool EnsureWingName(Game::WingId _wing);

  /// The lowest wing number nothing has named, or `INVALID_WING_ID` when the
  /// roster has no room for another row. What the composer offers as NEW WING.
  [[nodiscard]] Game::WingId FreeWingId() const noexcept;

  /// How many wings have a name at all, authored and player-owned counted once
  /// each. The row cap is a cap on *rows*, so a rename must not spend one -- an
  /// entry that renames an authored wing adds a word, not a row.
  [[nodiscard]] std::size_t NamedWingCount() const noexcept;

  /*
   * The docked place for this anchor, or null. Both station calls start here,
   * and neither should walk `m_places` on its own terms.
   *
   * Declared here rather than beside the other station calls because it names
   * `FleetPlace`, which is private and defined below them: a member function's
   * return type is looked up where the declaration is written, so this one has
   * to come after the type it returns.
   */
  [[nodiscard]] const FleetPlace* DockedAt(Game::AnchorId _anchor) const noexcept;

  Desc m_desc;
  Game::ReplicatedView m_view;

  /*
   * The call signs the **player** owns, by wing number -- minted here for a
   * wing the content never named, or restored from the settings layer.
   *
   * It outranks `Desc::wingNames` in `WingName`, and that ordering is the whole
   * of how a rename works: the authored table is content and stays as authored,
   * while an entry here is the word the player put on that row. It is also
   * exactly what is written back to the settings layer, so "what the player
   * owns" is one list rather than a rule applied twice.
   *
   * A pair list rather than a map or an array: it holds a handful of entries at
   * most -- the roster's row cap is the ceiling on how many wings can be named
   * at all -- so a node per wing would be three allocations to avoid a scan of
   * six, and an array indexed by `WingId` would be 256 strings to hold them.
   */
  std::vector<std::pair<Game::WingId, std::string>> m_playerWingNames;

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
   * Parallel to `m_validationIds`: where each of those ships is, and what it is.
   *
   * The ids alone were enough for four slices, because nothing validation
   * decided depended on where a ship *was*. Dock is the first check that does
   * (ADR-017 2), and Mine is the second -- and until this existed the client
   * handed the validator a view with no marks and no station, so **every
   * client-side Dock refused `UnknownStation`**: the pre-check the approach
   * chain waits on could never come true, and the chained verb never went.
   * Filled beside the ids for the reason they are: the ships an order may name
   * are exactly the ships the frame drew.
   */
  std::vector<Game::ShipMark> m_validationMarks;

  /// Where this grid's station is, in the wire's centimetres, refreshed by the
  /// scene build beside `m_stationEntityId`. Meaningless until one has arrived.
  mutable std::int32_t m_stationXCm = 0;
  mutable std::int32_t m_stationYCm = 0;

  std::vector<FleetPlace> m_places;

  /*
   * The economy's three summaries, as last stated (ADR-024 8, E3).
   *
   * Held for the same reason and on the same terms as the docked blocks: a
   * frame is a complete statement, so each list is replaced wholesale and a
   * kind the frame did not mention is a kind that has nothing to say. A grid
   * with no site clears `m_siteStatus`; a fleet holding nothing clears
   * `m_cargo`; and neither goes stale in the direction that matters, which is
   * showing a player something that has stopped being true.
   *
   * **The Bay is decoded and kept even though the tactical HUD never draws it.**
   * It has to be decoded regardless -- the summary frame has no length prefix,
   * so a body nobody reads desynchronises everything after it -- and once it is
   * decoded, throwing it away would only mean decoding it twice when E5's CARGO
   * tab arrives.
   */
  std::vector<Game::SiteStatusRow> m_siteStatus;
  std::vector<Game::CargoStatusRow> m_cargo;

  struct BayHolding
  {
    Game::AnchorId station = Game::INVALID_ID;
    std::uint32_t oreUnits[Game::ORE_COUNT] = {};

    /// E4b gave the Bay alloys as well as ore, which is the refinery's output
    /// landing where the ore came from. Decoded for the same reason the ore is:
    /// the record has to be consumed whether or not this surface draws it.
    std::uint32_t alloyUnits[Game::ALLOY_COUNT] = {};
  };
  std::vector<BayHolding> m_bays;

  /*
   * One commander's refinery at one station (E4b), kept on the Bay's terms: the
   * tactical HUD does not draw it, and it still has to be decoded because the
   * frame has no length prefix. E5's REFINERY tab is what reads it.
   */
  std::vector<Game::RefineryStatusRow> m_refineries;

  /*
   * Whether a summary has ever arrived.
   *
   * The toasts are the reason it exists: a count that goes from nothing to
   * three because the *first* frame landed is not three ships docking, and
   * without this the panel would announce the state of the world as though it
   * had just happened while the player watched.
   */
  bool m_haveSummary = false;

  /// Scratch for one decode, kept rather than made per arrival: this runs at
  /// the summary cadence, but the vector would otherwise be a fresh allocation
  /// every second for the life of the session.
  std::vector<Game::RosterEntry> m_decodedRoster;

  /// Scratch for the hangar's sort (ADR-017 §6a.4), kept rather than made per
  /// call: the screen asks once a frame and `m_places` must not be reordered by
  /// a draw.
  mutable std::vector<Game::RosterEntry> m_rosterSort;
  std::vector<Game::FleetSummary> m_decodedSummaries;
  std::vector<Game::CargoStatusRow> m_decodedCargo;

  /// Scratch for the validation view's hold room, kept because the command row
  /// asks for it every frame.
  mutable std::vector<std::uint32_t> m_holdRoom;

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
  std::uint64_t m_rejectedSummaries = 0;
};

} // namespace Outpost
