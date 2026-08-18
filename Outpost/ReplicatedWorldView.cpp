#include "pch.h"

#include "ReplicatedWorldView.h"

#include "SchemaHash.h"

#include <algorithm>
#include <utility>

using namespace Neuron;

namespace Outpost
{

ReplicatedWorldView::ReplicatedWorldView(Desc _desc)
  : m_desc(std::move(_desc))
{
}

std::uint32_t ReplicatedWorldView::ApplySnapshot(std::span<const std::uint8_t> _payload)
{
  if (!m_view.ApplySnapshot(_payload))
  {
    // Malformed or truncated. Counted rather than logged per occurrence: on a
    // lossy link this would be the noisiest line in the file, and the number
    // is what actually says whether it is happening.
    ++m_rejectedSnapshots;
    return 0;
  }
  return m_view.LatestTick();
}

void ReplicatedWorldView::BuildScene(double _renderTick, RenderScene& _outScene)
{
  m_view.SampleAt(_renderTick, m_sampled);

  _outScene.Clear();
  _outScene.instances.reserve(m_sampled.size());

  std::uint32_t renderClassCount = 0;
  for (const Game::ReplicatedShip& ship : m_sampled)
  {
    if (ship.classId >= m_desc.renderClassByHull.size())
    {
      continue; // A hull class this build has no mapping for.
    }
    const std::uint16_t renderClass = m_desc.renderClassByHull[ship.classId];
    if (renderClass == INVALID_RENDER_CLASS)
    {
      // A hull with no mesh -- the two reserved classes. Drawing nothing is the
      // honest answer; substituting another hull would put a ship on screen
      // that is not the ship the server is simulating.
      continue;
    }

    InstanceRecord instance;
    // Local plane metres straight into render space: x east, y the cosmetic
    // height, z north (ADR-001 §3). The cosmetic height stays zero until the
    // per-class hover the same ADR reserves.
    instance.posWorld = DirectX::XMFLOAT3{ship.positionMetres.x, 0.0f, ship.positionMetres.y};
    instance.heading = ship.headingRadians;
    instance.teamColorId = 0;
    // Selection and LOD bias are the overlay's channels (S8). The stale flag
    // rides here so the marker the icon sheet draws has something to read.
    instance.selectionAndLodBias = ship.stale ? 1u : 0u;
    instance.classId = renderClass;
    _outScene.instances.push_back(instance);

    renderClassCount = std::max(renderClassCount, static_cast<std::uint32_t>(renderClass) + 1u);
  }

  // Sorted so the opaque pass draws each class in one instanced run. The count
  // is the highest class actually present, not the mesh table's size: a scene
  // with only Interceptors needs one range, not nine.
  _outScene.SortByClass(renderClassCount);
}

OrderVerdict ReplicatedWorldView::PreCheck(const OrderIntent&)
{
  // Still refuses everything. `ValidateOrder` and its reason codes are S9's,
  // and inventing a verdict here would be inventing the parity ADR-014 §3 is
  // about.
  return OrderVerdict{};
}

void ReplicatedWorldView::SolvePreview(const OrderIntent&, OrderPreview& _outPreview)
{
  _outPreview.Clear();
}

bool ReplicatedWorldView::EncodeOrder(const OrderIntent&, ByteWriter&)
{
  return false; // No order format until S9: "not sent" rather than sent empty.
}

std::uint64_t ReplicatedWorldView::SchemaHash() const
{
  // The same number the server states, from the same string in the same build.
  // Asking the game for it rather than passing it in is what makes a content
  // mismatch detectable at all (ADR-004 §2).
  return Game::GameSchemaHash();
}

} // namespace Outpost
