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
[[nodiscard]] const SceneEntity* FindEntity(std::span<const SceneEntity> _entities, std::uint16_t _id) noexcept
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

/// Scales a packed colour's alpha, leaving the three channels alone. Alpha is
/// the high byte, `colourRgba` being r-in-the-low-byte (OverlayMark.h).
[[nodiscard]] std::uint32_t FadeRgba(std::uint32_t _colour, float _alphaScale) noexcept
{
  const float alpha = static_cast<float>((_colour >> 24) & 0xffu) * std::clamp(_alphaScale, 0.0f, 1.0f);
  return (_colour & 0x00ffffffu) | (static_cast<std::uint32_t>(alpha + 0.5f) << 24);
}

} // namespace

void BuildOverlayMarks(std::span<const SceneEntity> _entities, std::span<const std::uint16_t> _selectedIds,
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

  for (const std::uint16_t id : _selectedIds)
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

  for (const std::uint16_t id : _selectedIds)
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

void BuildGhostMarks(std::span<const OrderGhost> _ghosts, const OverlayTuning& _tuning, float _metresPerPixel,
                     double _nowSeconds, OverlayMarkList& _outMarks)
{
  if (_ghosts.empty())
  {
    return;
  }

  // Both in pixels converted to metres, so both hold their size on screen as the
  // camera zooms. The puck deliberately does *not* consult
  // `OrderPreview::extentMetres`: see `OverlayTuning::puckRadiusPixels`.
  const float puckRadiusMetres = _tuning.puckRadiusPixels * _metresPerPixel;
  const float stationRadiusMetres = _tuning.stationRadiusPixels * _metresPerPixel;

  // Built into a scratch run first, then inserted whole at the ring/bar
  // boundary. One insert of N marks rather than N inserts of one, and the
  // boundary is read once instead of moving under each push.
  std::vector<OverlayMark> planeMarks;
  planeMarks.reserve(_ghosts.size() * 4);

  for (const OrderGhost& ghost : _ghosts)
  {
    const float bounce = OrderGhostList::BounceFraction(ghost, _nowSeconds);
    const bool underWay = ghost.state == GhostState::UnderWay;

    std::uint32_t colour = _tuning.ghostPendingColourRgba;
    if (underWay)
    {
      colour = _tuning.ghostUnderWayColourRgba;
    }
    else if (ghost.state == GhostState::Rejected)
    {
      colour = _tuning.ghostRejectedColourRgba;
    }
    colour = FadeRgba(colour, 1.0f - bounce);

    // A refused ghost travels back toward where the fleet was. Both the anchor
    // and every station move together, so the footprint keeps its shape on the
    // way home rather than collapsing into its own centre.
    const XMFLOAT2 offset{(ghost.originMetres.x - ghost.targetMetres.x) * bounce,
                          (ghost.originMetres.y - ghost.targetMetres.y) * bounce};

    OverlayMark footprint;
    footprint.anchorPlane = XMFLOAT2{ghost.targetMetres.x + offset.x, ghost.targetMetres.y + offset.y};
    footprint.radiusMetres = puckRadiusMetres;
    footprint.colourRgba = colour;
    footprint.kind = static_cast<std::uint16_t>(OverlayKind::OrderFootprint);
    // Solid once the authority has agreed, dashed while it is a promise. That
    // one field is the entire difference between the print's PENDING panel and
    // its ACCEPTED one.
    footprint.fill = underWay ? 0u : _tuning.ghostDashCount;
    planeMarks.push_back(footprint);

    for (std::uint32_t index = 0; index < ghost.preview.markCount; ++index)
    {
      OverlayMark station;
      station.anchorPlane =
          XMFLOAT2{ghost.preview.markXMetres[index] + offset.x, ghost.preview.markYMetres[index] + offset.y};
      station.radiusMetres = stationRadiusMetres;
      station.colourRgba = colour;
      station.kind = static_cast<std::uint16_t>(OverlayKind::OrderStation);
      planeMarks.push_back(station);
    }
  }

  _outMarks.marks.insert(_outMarks.marks.begin() + _outMarks.ringCount, planeMarks.begin(), planeMarks.end());
  _outMarks.ringCount += static_cast<std::uint32_t>(planeMarks.size());
}

} // namespace Neuron
