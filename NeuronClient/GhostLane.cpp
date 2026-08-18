#include "pch.h"

#include "GhostLane.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

using namespace DirectX;

namespace Neuron
{
namespace
{

/// The same fade the footprint uses, so a bouncing ghost's lane and its ring
/// dim together. A copy of `OverlayMark.cpp`'s, and deliberately a small one:
/// exporting four bytes of shifting would put the Ui half's colours in the
/// overlay's header, and the two agree because they read the same `OverlayTuning`.
[[nodiscard]] std::uint32_t FadeRgba(std::uint32_t _colour, float _alphaScale) noexcept
{
  const float alpha = static_cast<float>((_colour >> 24) & 0xffu) * std::clamp(_alphaScale, 0.0f, 1.0f);
  return (_colour & 0x00ffffffu) | (static_cast<std::uint32_t>(alpha + 0.5f) << 24);
}

/// One side of the Liang-Barsky test. `_denominator` is the segment's component
/// against that edge's outward normal and `_numerator` how far inside the start
/// already is; the pair is the whole algorithm and writing it four times is how
/// one of the four ends up with the comparison the wrong way round.
[[nodiscard]] bool ClipAgainstEdge(float _denominator, float _numerator, float& _t0, float& _t1) noexcept
{
  if (_denominator == 0.0f)
  {
    // Parallel to this edge: inside if it started inside, and no `t` to clamp.
    return _numerator >= 0.0f;
  }

  const float t = _numerator / _denominator;
  if (_denominator < 0.0f)
  {
    if (t > _t1)
    {
      return false;
    }
    _t0 = std::max(_t0, t);
  }
  else
  {
    if (t < _t0)
    {
      return false;
    }
    _t1 = std::min(_t1, t);
  }
  return true;
}

} // namespace

bool ClipSegmentToRect(const UiRect& _rect, XMFLOAT2& _start, XMFLOAT2& _end) noexcept
{
  const float dx = _end.x - _start.x;
  const float dy = _end.y - _start.y;

  float t0 = 0.0f;
  float t1 = 1.0f;
  if (!ClipAgainstEdge(-dx, _start.x - _rect.x, t0, t1) || !ClipAgainstEdge(dx, _rect.Right() - _start.x, t0, t1) ||
      !ClipAgainstEdge(-dy, _start.y - _rect.y, t0, t1) || !ClipAgainstEdge(dy, _rect.Bottom() - _start.y, t0, t1))
  {
    return false;
  }

  // Written into locals first: `_start` is read by the expression that computes
  // `_end`, so assigning it in place would clip the second point against the
  // already-clipped first.
  const XMFLOAT2 clippedStart{_start.x + dx * t0, _start.y + dy * t0};
  const XMFLOAT2 clippedEnd{_start.x + dx * t1, _start.y + dy * t1};
  _start = clippedStart;
  _end = clippedEnd;
  return true;
}

void FormatLaneDetail(float _distanceMetres, float _etaSeconds, char* _out, std::size_t _capacity) noexcept
{
  if (_out == nullptr || _capacity == 0)
  {
    return;
  }
  _out[0] = '\0';

  // Kilometres past a kilometre, metres below it. The print's own switch, and
  // the reason it is a switch rather than always-km is that a station-keeping
  // nudge reads as `0.0 km`, which is a distance nobody ordered.
  char distance[24] = {};
  if (_distanceMetres >= 1000.0f)
  {
    std::snprintf(distance, sizeof(distance), "%.1f km", static_cast<double>(_distanceMetres) / 1000.0);
  }
  else
  {
    std::snprintf(distance, sizeof(distance), "%.0f m", static_cast<double>(_distanceMetres));
  }

  if (_etaSeconds < 0.0f)
  {
    // The game declined to say -- a group of stations, or a class table with no
    // speed in it. The distance is still true, so it is still shown.
    std::snprintf(_out, _capacity, "%s", distance);
    return;
  }

  // Rounded up rather than truncated: an ETA that reads `0s` while the ships
  // are visibly still moving is worse than one that reads `1s` for the last
  // fraction of a second.
  const auto totalSeconds = static_cast<std::uint32_t>(std::ceil(static_cast<double>(_etaSeconds)));
  const std::uint32_t minutes = totalSeconds / 60u;
  const std::uint32_t seconds = totalSeconds % 60u;
  if (minutes > 0u)
  {
    std::snprintf(_out, _capacity, "%s - ETA %um %02us", distance, minutes, seconds);
  }
  else
  {
    std::snprintf(_out, _capacity, "%s - ETA %us", distance, seconds);
  }
}

void BuildGhostLanes(std::span<const OrderGhost> _ghosts, const GhostLaneView& _view, const OverlayTuning& _colours,
                     const GhostLaneTuning& _tuning, double _nowSeconds, UiDrawList& _outList)
{
  if (_ghosts.empty() || _view.viewportWidth == 0 || _view.viewportHeight == 0)
  {
    return;
  }

  const float scale = _view.scale > 0.0f ? _view.scale : 1.0f;
  const float dash = _tuning.dashPixels * scale;
  const float gap = _tuning.gapPixels * scale;
  const float thickness = std::max(1.0f, _tuning.thicknessPixels * scale);
  const float pitch = dash + gap;

  for (const OrderGhost& ghost : _ghosts)
  {
    const float bounce = OrderGhostList::BounceFraction(ghost, _nowSeconds);

    // Retracted toward the fleet by the same fraction the footprint is, so the
    // lane's far end stays on the ring it points at all the way home.
    const XMFLOAT2 target{ghost.targetMetres.x + (ghost.originMetres.x - ghost.targetMetres.x) * bounce,
                          ghost.targetMetres.y + (ghost.originMetres.y - ghost.targetMetres.y) * bounce};

    XMFLOAT2 startNdc{};
    XMFLOAT2 endNdc{};
    if (!PlaneToNdc(_view.mapping, ghost.originMetres, startNdc) || !PlaneToNdc(_view.mapping, target, endNdc))
    {
      continue; // A degenerate mapping, before the first resize.
    }

    const XMFLOAT2 start = NdcToPixels(startNdc, _view.viewportWidth, _view.viewportHeight);
    const XMFLOAT2 end = NdcToPixels(endNdc, _view.viewportWidth, _view.viewportHeight);

    std::uint32_t colour = _colours.ghostPendingColourRgba;
    const bool underWay = ghost.state == GhostState::UnderWay;
    if (underWay)
    {
      colour = _colours.ghostUnderWayColourRgba;
    }
    else if (ghost.state == GhostState::Rejected)
    {
      colour = _colours.ghostRejectedColourRgba;
    }
    colour = FadeRgba(colour, 1.0f - bounce);

    const float dx = end.x - start.x;
    const float dy = end.y - start.y;
    const float length = std::sqrt(dx * dx + dy * dy);

    XMFLOAT2 clippedStart = start;
    XMFLOAT2 clippedEnd = end;
    if (length > 0.0f && ClipSegmentToRect(_view.worldRect, clippedStart, clippedEnd))
    {
      if (underWay)
      {
        // Solid the moment the authority agrees. That is the whole visual
        // difference between a promise and a fact (`puck-and-wheel.png` §4),
        // and it arrives on the same frame the ships start moving.
        _outList.AddSegment(clippedStart.x, clippedStart.y, clippedEnd.x, clippedEnd.y, thickness, colour);
      }
      else if (pitch > 0.0f)
      {
        const XMFLOAT2 direction{dx / length, dy / length};

        /*
         * Dashes are phased off the **unclipped** start.
         *
         * The clip point moves as the camera pans, so phasing off it would make
         * every dash crawl along the lane while the player scrolls -- motion
         * that reads as the order doing something. Measured from the lane's own
         * origin, a dash stays where it was put.
         */
        const float clippedFrom = (clippedStart.x - start.x) * direction.x + (clippedStart.y - start.y) * direction.y;
        const float clippedTo = (clippedEnd.x - start.x) * direction.x + (clippedEnd.y - start.y) * direction.y;

        const auto firstIndex = static_cast<std::int64_t>(std::floor(clippedFrom / pitch));
        std::uint32_t drawn = 0;
        for (std::int64_t index = firstIndex; drawn < _tuning.maxDashesPerLane; ++index)
        {
          const float dashFrom = static_cast<float>(index) * pitch;
          if (dashFrom > clippedTo)
          {
            break;
          }
          const float from = std::max(dashFrom, clippedFrom);
          const float to = std::min(dashFrom + dash, clippedTo);
          if (to <= from)
          {
            continue; // This dash's whole body is outside the visible run.
          }
          _outList.AddSegment(start.x + direction.x * from, start.y + direction.y * from, start.x + direction.x * to,
                              start.y + direction.y * to, thickness, colour);
          ++drawn;
        }
      }
    }

    /*
     * The label, only when the footprint it belongs to is on screen.
     *
     * A label pinned to the viewport edge for an off-screen order is an
     * off-screen indicator, which `overlay-pass.png` lists as its own mechanism-B
     * class with its own rules. Drawing half of one here would be inventing it.
     */
    if (!_view.worldRect.Contains(end.x, end.y))
    {
      continue;
    }

    // The ring's radius is *not* scaled: `BuildGhostMarks` converts it straight
    // to plane metres, so the footprint is that many pixels across whatever the
    // HUD's scale is. Only the gap under it belongs to the HUD.
    const float top = end.y + _colours.puckRadiusPixels + _tuning.labelGapPixels * scale;
    const float lineHeight = _tuning.labelLineHeightPixels * scale;

    if (ghost.preview.label[0] != '\0')
    {
      const auto width = static_cast<float>(std::strlen(ghost.preview.label)) * _view.cellPixels;
      _outList.AddText(end.x - width * 0.5f, top, _tuning.labelSizeIndex, colour, ghost.preview.label);
    }

    // Measured on the plane, not on screen: the label says how far the ships
    // travel, and the pixel length of the lane is a fact about the zoom.
    // Against the ghost's own target rather than the retracted one, because a
    // refused order's distance is the distance that was refused.
    const float journeyX = ghost.targetMetres.x - ghost.originMetres.x;
    const float journeyY = ghost.targetMetres.y - ghost.originMetres.y;

    /*
     * The authority's ETA if there is one, the game's prediction if there is
     * not (S12).
     *
     * A ghost is PENDING for one round trip and the prediction is all there is;
     * from promotion onward the authority is measuring the same thing from
     * where the ships have actually got to, and preferring the prediction after
     * that would be showing a stale guess beside a live fact.
     */
    const float eta = ghost.authorityEtaSeconds >= 0.0f ? ghost.authorityEtaSeconds : ghost.preview.etaSeconds;

    char detail[48] = {};
    FormatLaneDetail(std::sqrt(journeyX * journeyX + journeyY * journeyY), eta, detail, sizeof(detail));
    if (detail[0] != '\0')
    {
      const auto width = static_cast<float>(std::strlen(detail)) * _view.cellPixels;
      // Dimmer than the name above it: the print draws the command in the
      // ghost's own colour and its numbers a step back, so the eye reads what
      // the order *is* before how far it goes.
      _outList.AddText(end.x - width * 0.5f, top + lineHeight, _tuning.labelSizeIndex, FadeRgba(colour, 0.65f), detail);
    }
  }
}

} // namespace Neuron
