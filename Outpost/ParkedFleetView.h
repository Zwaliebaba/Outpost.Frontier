#pragma once

#include "RenderWorld.h"
#include "WorldView.h"

#include <cstdint>
#include <vector>

/*
 * The placeholder world (Build Order S5/S5b, behind S5c's seam).
 *
 * A `Neuron::WorldView` that invents a parked fleet and places the authored
 * scenery the universe definition asked for. It replicates nothing, validates
 * nothing and encodes nothing, because there is no simulation yet -- S6 builds
 * one and S7 replaces this class with a view over real snapshots.
 *
 * **Why it lives in the executable.** ADR-014 §2 says GameLogic implements the
 * engine's interfaces, and ADR-014 §1 says GameLogic depends on NeuronCore
 * only. Both cannot hold: `WorldView.h` is NeuronClient's. Resolved in favour
 * of §1 (ADR-014 §2a), because GameLogic's freedom from Windows, D3D12 and file
 * IO is what lets `GameLogicTests` run with no device and no fixtures -- and
 * putting `$(SolutionDir)NeuronClient` on GameLogic's include path would make
 * that a convention rather than a structural fact. The composition root already
 * knows both halves, so it is where the vtable goes.
 *
 * What that costs, said plainly: this class is in the one project with no test
 * project of its own. The answer is to keep it thin enough that there is
 * nothing to test -- it holds data and forwards to engine helpers that *are*
 * tested (`BuildParkedFleet`, `AddScenery`, `RenderScene::SortByClass`).
 */

namespace Outpost
{

class ParkedFleetView final : public Neuron::WorldView
{
public:
  struct Desc
  {
    /// Placements read from the universe definition, in the grid's local
    /// metres. The station is one of these (ADR-009 §9a).
    std::vector<Neuron::ScenePlacement> scenery;

    /// How many mesh classes the content list declares. The last is the
    /// structure; the rest are ships, smallest first (AppConfig.h).
    std::uint32_t classCount = 0;

    /// The content hash the client compares at the handshake. It is the
    /// universe hash, because the universe is the only authored content the
    /// client loads so far (ADR-009 §8).
    std::uint64_t contentHash = 0;
  };

  explicit ParkedFleetView(Desc _desc);

  void ApplySnapshot(std::uint32_t _tick, std::span<const std::uint8_t> _payload) override;
  void BuildScene(double _renderTick, Neuron::RenderScene& _outScene) override;

  [[nodiscard]] Neuron::OrderVerdict PreCheck(const Neuron::OrderIntent& _intent) override;
  void SolvePreview(const Neuron::OrderIntent& _intent, Neuron::OrderPreview& _outPreview) override;
  [[nodiscard]] bool EncodeOrder(const Neuron::OrderIntent& _intent, Neuron::ByteWriter& _writer) override;

  /// Zero until GameLogic has wire types of its own (S6). Reporting something
  /// plausible would be worse than reporting nothing: the handshake would then
  /// agree about a layout that does not exist.
  [[nodiscard]] std::uint64_t SchemaHash() const override { return 0; }
  [[nodiscard]] std::uint64_t ContentHash() const override { return m_desc.contentHash; }

private:
  Desc m_desc;
};

} // namespace Outpost
