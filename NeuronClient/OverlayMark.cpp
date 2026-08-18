#include "pch.h"

#include "OverlayMark.h"

#include <algorithm>

using namespace DirectX;

namespace Neuron
{
namespace
{

/// The entity with this id, or null. Linear because a selection is a handful
/// of ships and the entity list is a few hundred: a map would cost more to
/// build every frame than the scan costs to run.
[[nodiscard]] const SceneEntity* FindEntity(std::span<const SceneEntity> _entities, std::uint32_t _id) noexcept
{
  const auto found = std::find_if(_entities.begin(), _entities.end(), [_id](const SceneEntity& _e) { return _e.id == _id; });
  return found == _entities.end() ? nullptr : &*found;
}

/// A gauge as the shader wants it: 0-255 in, 0-65535 out. Not a percentage --
/// a full ship is 255, and dividing by a hundred would put every bar past its
/// own end.
[[nodiscard]] std::uint16_t GaugeToFill(std::uint8_t _gauge) noexcept
{
  return static_cast<std::uint16_t>(_gauge * 257); // 255 * 257 == 65535, exactly.
}

} // namespace

void BuildOverlayMarks(std::span<const SceneEntity> _entities, std::span<const std::uint32_t> _selectedIds,
                       const OverlayTuning& _tuning, float _metresPerPixel, OverlayMarkList& _outMarks)
{
  _outMarks.Clear();
  if (_selectedIds.empty())
  {
    return;
  }

  // Rings first, then bars, because the two halves are two draws with
  // different depth state and the split has to be a contiguous range.
  const float minRadiusMetres = _tuning.ringMinRadiusPixels * _metresPerPixel;

  for (const std::uint32_t id : _selectedIds)
  {
    const SceneEntity* entity = FindEntity(_entities, id);
    if (entity == nullptr)
    {
      continue; // Selected but gone. Retain should have caught it; this is belt.
    }

    OverlayMark ring;
    ring.anchorPlane = entity->planeMetres;
    ring.radiusMetres = std::max(entity->pickRadiusMetres + _tuning.ringPadMetres, minRadiusMetres);
    ring.colourRgba = entity->stale ? _tuning.staleRingColourRgba : _tuning.ringColourRgba;
    ring.kind = static_cast<std::uint16_t>(OverlayKind::SelectionRing);
    _outMarks.marks.push_back(ring);
  }
  _outMarks.ringCount = static_cast<std::uint32_t>(_outMarks.marks.size());

  for (const std::uint32_t id : _selectedIds)
  {
    const SceneEntity* entity = FindEntity(_entities, id);
    if (entity == nullptr)
    {
      continue;
    }

    OverlayMark bar;
    bar.anchorPlane = entity->planeMetres;
    bar.halfWidthPixels = _tuning.barHalfWidthPixels;
    bar.halfHeightPixels = _tuning.barHalfHeightPixels;

    // Hull under shield: the shield is what depletes first, so it reads
    // top-down as the order the player loses them in.
    bar.offsetUpPixels = _tuning.barOffsetUpPixels;
    bar.colourRgba = _tuning.hullColourRgba;
    bar.kind = static_cast<std::uint16_t>(OverlayKind::HullBar);
    bar.fill = GaugeToFill(entity->hullGauge);
    _outMarks.marks.push_back(bar);

    bar.offsetUpPixels = _tuning.barOffsetUpPixels + 2.0f * _tuning.barHalfHeightPixels + _tuning.barGapPixels;
    bar.colourRgba = _tuning.shieldColourRgba;
    bar.kind = static_cast<std::uint16_t>(OverlayKind::ShieldBar);
    bar.fill = GaugeToFill(entity->shieldGauge);
    _outMarks.marks.push_back(bar);
  }
}

} // namespace Neuron
