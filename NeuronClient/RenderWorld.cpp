#include "pch.h"

#include "RenderWorld.h"

#include <algorithm>
#include <cmath>

namespace Neuron
{
namespace
{

using namespace DirectX;

} // namespace

void RenderScene::Clear() noexcept
{
  // Capacity is kept on purpose: this runs once a frame, and the instance count
  // is stable, so releasing the storage would be an allocation every frame for
  // no gain.
  instances.clear();
  classRanges.clear();
}

void RenderScene::SortByClass(std::uint32_t _classCount)
{
  std::stable_sort(instances.begin(), instances.end(),
                   [](const InstanceRecord& _a, const InstanceRecord& _b) noexcept { return _a.classId < _b.classId; });

  classRanges.assign(_classCount, InstanceRange{});
  for (std::uint32_t i = 0; i < instances.size(); ++i)
  {
    const std::uint32_t classId = instances[i].classId;
    if (classId >= _classCount)
    {
      continue; // An instance of a class with no mesh simply is not drawn.
    }
    if (classRanges[classId].instanceCount == 0)
    {
      classRanges[classId].firstInstance = i;
    }
    ++classRanges[classId].instanceCount;
  }
}

void BuildParkedFleet(const ParkedFleetDesc& _desc, std::span<const float> _classRadiiMetres, RenderScene& _outScene)
{
  _outScene.Clear();

  const auto classCount = static_cast<std::uint32_t>(_classRadiiMetres.size());
  if (classCount == 0)
  {
    return;
  }

  // The station sits at the grid anchor. S5b moves the anchor to wherever the
  // universe file puts the station; until then the anchor is the origin.
  if (_desc.structureClassId < classCount)
  {
    InstanceRecord station;
    station.posWorld = XMFLOAT3{0.0f, 0.0f, 0.0f};
    station.heading = 0.0f;
    station.teamColorId = 0;
    station.selectionAndLodBias = 0;
    station.classId = static_cast<std::uint16_t>(_desc.structureClassId);
    _outScene.instances.push_back(station);
  }

  const std::uint32_t wings = std::min(_desc.shipClassCount, classCount);
  for (std::uint32_t wing = 0; wing < wings; ++wing)
  {
    // One wing per class, spread evenly around the station: eight bearings is
    // also the yaw detent spacing, so orbiting always faces a wing squarely.
    const float bearing = (XM_2PI * static_cast<float>(wing)) / static_cast<float>(wings);
    const float centreX = std::cos(bearing) * _desc.wingRadiusMetres;
    const float centreZ = std::sin(bearing) * _desc.wingRadiusMetres;

    // Parked facing the station, which is what a docked wing looks like and
    // what makes a wrong heading obvious on screen.
    const float heading = bearing + XM_PI;

    // Line abreast, perpendicular to the bearing, spaced off the class's own
    // hull radius so a Carrier wing does not intersect itself.
    const float spacing = std::max(60.0f, _classRadiiMetres[wing] * _desc.wingSpacingMultiplier);
    const float acrossX = -std::sin(bearing);
    const float acrossZ = std::cos(bearing);

    for (std::uint32_t index = 0; index < _desc.shipsPerWing; ++index)
    {
      const float offset = (static_cast<float>(index) - 0.5f * static_cast<float>(_desc.shipsPerWing - 1)) * spacing;

      InstanceRecord ship;
      ship.posWorld = XMFLOAT3{centreX + acrossX * offset, 0.0f, centreZ + acrossZ * offset};
      ship.heading = heading;
      ship.teamColorId = 0;
      ship.selectionAndLodBias = 0;
      ship.classId = static_cast<std::uint16_t>(wing);
      _outScene.instances.push_back(ship);
    }
  }

  _outScene.SortByClass(classCount);
}

} // namespace Neuron
