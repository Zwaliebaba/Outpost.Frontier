#include "pch.h"

#include "ParkedFleetView.h"

#include <algorithm>
#include <iterator>
#include <utility>

using namespace Neuron;

namespace Outpost
{
namespace
{

/*
 * Hull radii for wing spacing, in metres, by classId.
 *
 * The renderer knows the real radii -- it parsed the meshes -- but the seam
 * does not carry them, and it should not: a real world view (S7) places ships
 * where the simulation says they are and never asks how big their meshes are.
 * These are the placeholder's own numbers, roughly the shipped hulls' order of
 * size, and they exist only so a Carrier wing is not parked at Interceptor
 * spacing. They die with this class.
 */
constexpr float PLACEHOLDER_CLASS_RADII_METRES[] = {
    18.0f,  // Interceptor
    26.0f,  // Bomber
    34.0f,  // Corvette
    52.0f,  // Frigate
    68.0f,  // Hauler
    74.0f,  // Miner
    130.0f, // Carrier
    160.0f, // Battleship
    240.0f, // Structure
};

} // namespace

ParkedFleetView::ParkedFleetView(Desc _desc)
  : m_desc(std::move(_desc))
{
}

void ParkedFleetView::ApplySnapshot(std::uint32_t, std::span<const std::uint8_t>)
{
  // Nothing replicates yet (S7). Dropping the payload is the honest answer:
  // there is no state for it to update, and buffering bytes nothing reads
  // would only make the first real snapshot arrive into a queue of fiction.
}

void ParkedFleetView::BuildScene(double, Neuron::RenderScene& _outScene)
{
  // The render tick is ignored because nothing here moves. When S7 gives this
  // seam a real implementation, that argument is the whole point of it.
  const std::uint32_t classCount = std::min(m_desc.classCount, static_cast<std::uint32_t>(std::size(PLACEHOLDER_CLASS_RADII_METRES)));

  ParkedFleetDesc fleet;
  // The last class is the structure and arrives as authored scenery, so the
  // invented half is ships only (ADR-009 §9a).
  fleet.shipClassCount = classCount > 0 ? classCount - 1 : 0;
  BuildParkedFleet(fleet, std::span<const float>{PLACEHOLDER_CLASS_RADII_METRES, classCount}, _outScene);

  AddScenery(m_desc.scenery, _outScene);

  // BuildParkedFleet leaves its own scene sorted -- it is a standalone helper
  // with standalone tests -- so this re-sorts a merged array rather than an
  // unsorted one. Forty-odd instances make that free, and the alternative is a
  // producer whose output is only valid if the caller remembers to finish it.
  _outScene.SortByClass(classCount);
}

OrderVerdict ParkedFleetView::PreCheck(const OrderIntent&)
{
  // Refuses everything, with reason code zero. There is nothing to command and
  // no reason enum to quote -- GameLogic owns those, and it has no orders yet.
  return OrderVerdict{};
}

void ParkedFleetView::SolvePreview(const OrderIntent&, OrderPreview& _outPreview)
{
  _outPreview.Clear();
}

bool ParkedFleetView::EncodeOrder(const OrderIntent&, ByteWriter&)
{
  return false; // Nothing to send, so "not sent" rather than an empty message.
}

} // namespace Outpost
