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
  // nudge reads as `0.0 KM`, which is a distance nobody ordered. Uppercase and
  // ·-separated because the label is HUD chrome and the print sets all of it
  // in the monospace caps the rest of the chrome speaks.
  char distance[24] = {};
  if (_distanceMetres >= 1000.0f)
  {
    std::snprintf(distance, sizeof(distance), "%.1f KM", static_cast<double>(_distanceMetres) / 1000.0);
  }
  else
  {
    std::snprintf(distance, sizeof(distance), "%.0f M", static_cast<double>(_distanceMetres));
  }

  char eta[24] = {};
  FormatEta(_etaSeconds, eta, sizeof(eta));
  if (eta[0] == '\0')
  {
    // The game declined to say -- a group of stations, or a class table with no
    // speed in it. The distance is still true, so it is still shown.
    std::snprintf(_out, _capacity, "%s", distance);
    return;
  }
  // U+00B7, as UTF-8 bytes for the reason every marker glyph in this codebase
  // is: these files carry no byte-order mark.
  std::snprintf(_out, _capacity, "%s \xC2\xB7 ETA %s", distance, eta);
}

void FormatEta(float _seconds, char* _out, std::size_t _capacity) noexcept
{
  if (_out == nullptr || _capacity == 0)
  {
    return;
  }
  _out[0] = '\0';
  if (_seconds < 0.0f)
  {
    return; // Nothing to say, and an empty string is how a caller is told so.
  }

  // Rounded up rather than truncated: an ETA that reads `0s` while the ships
  // are visibly still moving is worse than one that reads `1s` for the last
  // fraction of a second.
  const auto totalSeconds = static_cast<std::uint32_t>(std::ceil(static_cast<double>(_seconds)));
  const std::uint32_t minutes = totalSeconds / 60u;
  const std::uint32_t seconds = totalSeconds % 60u;
  if (minutes > 0u)
  {
    std::snprintf(_out, _capacity, "%uM %02uS", minutes, seconds);
  }
  else
  {
    std::snprintf(_out, _capacity, "%uS", seconds);
  }
}

float DashCrawlPhase(double _nowSeconds, double _periodSeconds) noexcept
{
  // Not `<= 0.0`: a NaN period must answer "no crawl" too, and only a positive
  // comparison rejects it.
  if (!(_periodSeconds > 0.0))
  {
    return 0.0f;
  }

  double wrapped = std::fmod(_nowSeconds, _periodSeconds);
  if (wrapped < 0.0)
  {
    wrapped += _periodSeconds; // fmod keeps the dividend's sign.
  }

  // The division is what makes this a phase rather than a time, and doing it
  // after the wrap is what keeps the result exact: both operands are now small,
  // however long the session has been running.
  return static_cast<float>(wrapped / _periodSeconds);
}

void BuildGhostLanes(std::span<const OrderGhost> _ghosts, std::span<const SceneEntity> _entities,
                     const GhostLaneView& _view, const OverlayTuning& _colours, const GhostLaneTuning& _tuning,
                     double _nowSeconds, UiDrawList& _outLanes, UiDrawList& _outLabels)
{
  if (_ghosts.empty() || _view.viewportWidth == 0 || _view.viewportHeight == 0)
  {
    return;
  }

  const float scale = _view.scale > 0.0f ? _view.scale : 1.0f;
  const float thickness = std::max(1.0f, _tuning.thicknessPixels * scale);

  /*
   * The dash pattern, in screen pixels and the same size at every zoom.
   *
   * Scaled by the HUD's own `scale` and by nothing else -- no camera term, which
   * is the whole point: the lane is chrome, and a player who zooms out is not
   * asking for finer chrome. A period of zero leaves the lane solid, which is
   * the branch below rather than a division by nothing.
   */
  const float pitch = _tuning.dashPeriodPixels > 0.0f ? _tuning.dashPeriodPixels * scale : 0.0f;
  const float dash = pitch * std::clamp(_tuning.dashDutyCycle, 0.05f, 0.95f);

  /*
   * The crawl (`puck-and-wheel.png`'s `crawl`), as a fraction of one cycle.
   *
   * Computed once for the whole frame rather than per leg, so every dash on
   * every lane advances together -- two ghosts side by side marching out of
   * step would read as them meaning different things.
   */
  const float crawl = DashCrawlPhase(_nowSeconds, _tuning.crawlSecondsPerCycle);


  for (const OrderGhost& ghost : _ghosts)
  {
    const float bounce = OrderGhostList::BounceFraction(ghost, _nowSeconds);
    const bool underWay = ghost.state == GhostState::UnderWay;

    std::uint32_t colour = _colours.ghostPendingColourRgba;
    if (underWay)
    {
      colour = _colours.ghostUnderWayColourRgba;
    }
    else if (ghost.state == GhostState::Rejected)
    {
      colour = _colours.ghostRejectedColourRgba;
    }
    colour = FadeRgba(colour, 1.0f - bounce);

    /*
     * The chain, as points: the fleet, then a waypoint per leg
     * (`puck-and-wheel.png` §4). A ghost with one leg is this with two points,
     * which is the single lane S11c drew -- there is no separate case for it.
     */
    XMFLOAT2 plane[MAX_GHOST_LEGS + 1];

    /*
     * The tail follows the fleet.
     *
     * `originMetres` is where the group *was* when the order was given, which is
     * the right anchor for the bounce (a refusal comes back to where it was
     * asked from) and the wrong one to draw from: left alone it pins the line to
     * a patch of empty space the ships left seconds ago, and the lane grows a
     * tail instead of shortening as they close on the target.
     *
     * The first named ship is the one tracked, which is the same ship
     * `World::IngestOrders` joins an append to -- so the line starts where the
     * group's own anchor is rather than at an average that no hull sits on. A
     * ship that has despawned falls back to the origin, because a lane starting
     * nowhere is worse than one starting stale.
     */
    plane[0] = ghost.originMetres;
    for (const SceneEntity& entity : _entities)
    {
      if (entity.id == ghost.firstEntityId)
      {
        plane[0] = entity.planeMetres;
        break;
      }
    }
    const std::uint32_t legs = std::min<std::uint32_t>(ghost.queuedLegCount, MAX_GHOST_LEGS);
    for (std::uint32_t index = 0; index < legs; ++index)
    {
      plane[index + 1] = ghost.legs[index].targetMetres;
    }
    if (legs == 0)
    {
      continue; // A ghost with no plan is nothing to draw.
    }

    // Only the *last* waypoint retracts, and only toward the one before it:
    // a refused append takes back the leg that was refused, not the plan.
    const XMFLOAT2 home = ghost.RetractTowardMetres();
    plane[legs].x += (home.x - plane[legs].x) * bounce;
    plane[legs].y += (home.y - plane[legs].y) * bounce;

    XMFLOAT2 pixels[MAX_GHOST_LEGS + 1];
    bool projected = true;
    for (std::uint32_t index = 0; index <= legs && projected; ++index)
    {
      XMFLOAT2 ndc{};
      projected = PlaneToNdc(_view.mapping, plane[index], ndc);
      pixels[index] = NdcToPixels(ndc, _view.viewportWidth, _view.viewportHeight);
    }
    if (!projected)
    {
      continue; // A degenerate mapping, before the first resize.
    }

    for (std::uint32_t index = 0; index < legs; ++index)
    {
      const XMFLOAT2 from = pixels[index];
      const XMFLOAT2 to = pixels[index + 1];

      const float dx = to.x - from.x;
      const float dy = to.y - from.y;
      const float length = std::sqrt(dx * dx + dy * dy);

      XMFLOAT2 clippedStart = from;
      XMFLOAT2 clippedEnd = to;
      if (length > 0.0f && ClipSegmentToRect(_view.worldRect, clippedStart, clippedEnd))
      {
        if (pitch <= 0.0f)
        {
          // No scale to dash with -- a view that never said what a pixel is
          // worth. Solid, because a lane that vanished would be worse than one
          // that is merely not marching.
          _outLanes.AddSegment(clippedStart.x, clippedStart.y, clippedEnd.x, clippedEnd.y, thickness, colour);
        }
        else
        {
          /*
           * Dashed in every state, including under way.
           *
           * This used to go solid the moment the authority agreed, on the
           * argument that solid-versus-dashed was the difference between a
           * promise and a fact. It is not, and the owner's call (2026-08-19) is
           * that **the crawl is what says the ships are moving** -- so the state
           * a player watches for most of an order's life is the one that has to
           * show it. Promise and fact stay distinct through colour and alpha,
           * which were always carrying it too: a half-transparent cyan promise
           * against an opaque green fact.
           */
          const XMFLOAT2 direction{dx / length, dy / length};

          /*
           * Dashes are phased off this leg's **own** start, unclipped, plus the
           * frame's crawl.
           *
           * The clip point moves as the camera pans, so phasing off it would
           * make every dash slide along the lane while the player scrolls --
           * motion that reads as the order doing something. Per leg rather than
           * along the whole chain, so a leg's dashes do not shift when the leg
           * *before* it is retracted by a bounce.
           *
           * The crawl is the one motion that *is* wanted, and it comes from the
           * clock alone: adding it to the pattern's origin walks the dashes from
           * `from` toward `to`, which is the direction the order is going.
           * Subtracting it would march them back at the ship and read as a
           * retreat -- the sign is the whole feature.
           */
          const float clippedFrom = (clippedStart.x - from.x) * direction.x + (clippedStart.y - from.y) * direction.y;
          const float clippedTo = (clippedEnd.x - from.x) * direction.x + (clippedEnd.y - from.y) * direction.y;


          const auto firstIndex = static_cast<std::int64_t>(std::floor(clippedFrom / pitch - crawl));
          std::uint32_t drawn = 0;
          for (std::int64_t step = firstIndex; drawn < _tuning.maxDashesPerLane; ++step)
          {
            const float dashFrom = (static_cast<float>(step) + crawl) * pitch;
            if (dashFrom > clippedTo)
            {
              break;
            }
            const float segmentFrom = std::max(dashFrom, clippedFrom);
            const float segmentTo = std::min(dashFrom + dash, clippedTo);
            if (segmentTo <= segmentFrom)
            {
              continue; // This dash's whole body is outside the visible run.
            }
            _outLanes.AddSegment(from.x + direction.x * segmentFrom, from.y + direction.y * segmentFrom,
                                from.x + direction.x * segmentTo, from.y + direction.y * segmentTo, thickness, colour);
            ++drawn;
          }
        }
      }

      /*
       * A per-leg ETA at every waypoint but the last (S12).
       *
       * The last one's number is in the two-line label under the footprint, so
       * repeating it beside the ring would be the same seconds twice. A
       * single-leg ghost therefore gets no waypoint labels at all, which is
       * S11c's picture unchanged.
       */
      if (index + 1 >= legs || !_view.worldRect.Contains(to.x, to.y))
      {
        continue;
      }

      // The authority's number for the leg it is actually flying; the game's
      // prediction for the ones ahead of it, which the authority has not
      // started and has nothing to say about yet.
      const float legEta = (index == ghost.legIndex && ghost.authorityEtaSeconds >= 0.0f) ? ghost.authorityEtaSeconds
                                                                                         : ghost.legs[index].etaSeconds;
      char waypoint[24] = {};
      FormatEta(legEta, waypoint, sizeof(waypoint));
      if (waypoint[0] == '\0')
      {
        continue;
      }
      // Hangs below the waypoint, so containing the waypoint is not enough --
      // the same spill the footprint's own label had.
      const float waypointY = to.y + _tuning.labelGapPixels * scale;
      if (waypointY + _tuning.labelLineHeightPixels * scale > _view.worldRect.Bottom())
      {
        continue;
      }
      const auto width = static_cast<float>(std::strlen(waypoint)) * _view.cellPixels;
      _outLabels.AddText(to.x - width * 0.5f, waypointY, _tuning.detailSizeIndex, FadeRgba(colour, 0.75f), waypoint);
    }

    /*
     * The label, only when the footprint it belongs to is on screen.
     *
     * A label pinned to the viewport edge for an off-screen order is an
     * off-screen indicator, which `overlay-pass.png` lists as its own
     * mechanism-B class with its own rules. Drawing half of one here would be
     * inventing it.
     */
    const XMFLOAT2 end = pixels[legs];
    if (!_view.worldRect.Contains(end.x, end.y))
    {
      continue;
    }

    // The ring's radius is *not* scaled: `BuildGhostMarks` converts it straight
    // to plane metres, so the footprint is that many pixels across whatever the
    // HUD's scale is. Only the gap under it belongs to the HUD.
    const float top = end.y + _colours.puckRadiusPixels + _tuning.labelGapPixels * scale;
    const float lineHeight = _tuning.labelLineHeightPixels * scale;
    float line = top;

    /*
     * A label line is drawn only if it fits inside the world zone.
     *
     * The footprint being on screen is not enough: the lines hang *below* it,
     * so a puck near the bottom of the band put `2.1 KM - ETA 19S` across the
     * context bar -- world content over chrome, which ADR-006 forbids outright.
     * The labels are in the Ui layer and composite *after* the panels, so no
     * scissor upstream can catch them; the zone has to be honoured here.
     *
     * Per line rather than for the block, so a label that half fits loses only
     * the lines that do not -- the order's name survives and its numbers drop,
     * which is the right half to keep.
     */
    const auto lineFits = [&](float _y) { return _y >= _view.worldRect.y && _y + lineHeight <= _view.worldRect.Bottom(); };

    if (ghost.preview.label[0] != '\0' && lineFits(line))
    {
      // Cells, not bytes: the label carries a UTF-8 token separator, and
      // `strlen` would over-measure it by two columns.
      const auto width = static_cast<float>(TextCellCount(ghost.preview.label)) * _view.cellPixels;
      _outLabels.AddText(end.x - width * 0.5f, line, _tuning.labelSizeIndex, colour, ghost.preview.label);
      line += lineHeight;
    }

    /*
     * The distance is the **whole plan's**, walked leg by leg on the plane.
     *
     * Not on screen: the pixel length of a lane is a fact about the zoom, and a
     * label that changed when the player scrolled the wheel would be reporting
     * the camera. Against the ghost's own waypoints rather than the retracted
     * ones, because a refused order's distance is the distance that was
     * refused.
     */
    float journeyMetres = 0.0f;
    XMFLOAT2 walk = ghost.originMetres;
    for (std::uint32_t index = 0; index < legs; ++index)
    {
      const float legX = ghost.legs[index].targetMetres.x - walk.x;
      const float legY = ghost.legs[index].targetMetres.y - walk.y;
      journeyMetres += std::sqrt(legX * legX + legY * legY);
      walk = ghost.legs[index].targetMetres;
    }

    const float lastEta = (legs - 1 == ghost.legIndex && ghost.authorityEtaSeconds >= 0.0f)
                              ? ghost.authorityEtaSeconds
                              : ghost.legs[legs - 1].etaSeconds;

    char detail[48] = {};
    FormatLaneDetail(journeyMetres, lastEta, detail, sizeof(detail));
    if (detail[0] != '\0' && lineFits(line))
    {
      const auto width = static_cast<float>(TextCellCount(detail)) * _view.cellPixels;
      // Dimmer and a size smaller than the name above it: the print draws the
      // command in the ghost's own colour and its numbers a step back, so the
      // eye reads what the order *is* before how far it goes.
      _outLabels.AddText(end.x - width * 0.5f, line, _tuning.detailSizeIndex, FadeRgba(colour, 0.65f), detail);
      line += lineHeight;
    }

    /*
     * `3 LEGS` -- the print's own footer, and only when there is a queue.
     *
     * The authority's `legCount` when it has reported one, because that is the
     * number that is true; the client's own until then. They differ for exactly
     * one round trip, which is the window the chain is a guess in.
     */
    if ((legs > 1 || ghost.legCount > 1) && lineFits(line))
    {
      const std::uint32_t shown = ghost.legCount > 0 ? ghost.legCount : legs;
      char footer[24] = {};
      // One branch, everywhere a count is printed: `1 LEG`, never `1 LEGS`.
      std::snprintf(footer, sizeof(footer), "%u LEG%s", shown, shown == 1 ? "" : "S");
      const auto width = static_cast<float>(std::strlen(footer)) * _view.cellPixels;
      _outLabels.AddText(end.x - width * 0.5f, line, _tuning.detailSizeIndex, FadeRgba(colour, 0.5f), footer);
    }
  }
}

} // namespace Neuron