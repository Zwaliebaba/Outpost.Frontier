#include "pch.h"

#include "SettingsScreen.h"

#include <algorithm>

namespace Neuron
{
namespace
{

[[nodiscard]] float SafeScale(float _scale) noexcept
{
  return _scale > 0.0f ? _scale : 1.0f;
}

/*
 * The tables, in print order.
 *
 * Words and notes verbatim from `settings.png` §1 where it draws them, because
 * a paraphrase is a second author: *"Overrides the selection + damaged rule"* is
 * the only place a player learns what that toggle does, and shortening it to
 * "hull bars" would take that away.
 */

/// ACCESSIBILITY -- the print's first REQUIRED section, and the one three other
/// documents depend on (§3: the palette contract, the UI-scale range, the wheel).
constexpr SettingsItem ACCESSIBILITY_ITEMS[] = {
    {SettingsControlKind::Choice, "Colour vision palette",
     "Flag rings and hull glyphs stay distinguishable by shape in every palette -- colour is never the only carrier.",
     3, 0.0f, 0.0f},
    {SettingsControlKind::Slider, "UI scale",
     "Touch targets never drop below 48 px at any scale -- the floor is enforced, not scaled.", 0, 0.8f, 1.6f},
    {SettingsControlKind::Toggle, "High contrast chrome", "Opaque panels, heavier borders.", 0, 0.0f, 0.0f},
    {SettingsControlKind::Toggle, "Reduce motion", "Stops pulses and sweeps; ghosts still bounce.", 0, 0.0f, 0.0f},
    {SettingsControlKind::Toggle, "Always show hull bars", "Overrides the selection + damaged rule.", 0, 0.0f, 0.0f},
};

/*
 * INPUT -- the second REQUIRED section, and the one the command wheel is
 * blocked on.
 *
 * The print is unusually firm about why handedness is a toggle rather than
 * something the client works out: *"Never inferred -- a wheel that silently
 * re-orders itself is worse than one that is merely mirrored."*
 *
 * The dwell is here because 350 ms is a compromise, which `GestureTuning`
 * already says in as many words -- it is lifted from `puck-and-wheel.png`'s own
 * step 1 -- and a player with a tremor needs it longer. The range is the
 * arbiter's: below 200 ms a press cannot be told from a tap, and past 800 ms
 * the surface feels broken rather than deliberate.
 */
constexpr SettingsItem INPUT_ITEMS[] = {
    {SettingsControlKind::Choice, "Handedness",
     "Rotates the command wheel so HOLD sits under your thumb. Never inferred.", 2, 0.0f, 0.0f},
    {SettingsControlKind::Slider, "Long-press dwell",
     "How long a press holds before it becomes an order surface. 350 ms is a compromise.", 0, 0.200f, 0.800f},
};

/*
 * DISPLAY -- "thin by design", and the print says exactly why: ADR-003 §7 makes
 * every visual enhancement a `GpuCaps` decision, so a quality menu here would be
 * a second, worse scheduler fighting the automatic degradation ladder.
 *
 * What is left is the two things that are genuinely the player's -- whether to
 * wait for the display and whether to cap the rate -- plus readouts for the
 * three the client *decided*, which are on the screen because they are true and
 * a player would otherwise have to guess.
 */
constexpr SettingsItem DISPLAY_ITEMS[] = {
    {SettingsControlKind::Toggle, "Wait for vertical blank", "Off uncaps the rate and tears; on is the default.", 0,
     0.0f, 0.0f},
    {SettingsControlKind::Slider, "Frame cap", "Zero is uncapped. A cap below the display's rate is a power decision.",
     0, 0.0f, 240.0f},
    {SettingsControlKind::Readout, "Resolution", "The client draws at the window's size; there is no render scale.", 0,
     0.0f, 0.0f},
    {SettingsControlKind::Readout, "Multisampling", "Chosen from what the device reports, not from a menu.", 0, 0.0f,
     0.0f},
};

/*
 * AUDIO -- "free", because the mix architecture exists: five submixes and a
 * master, already defined and already wired. This section is a view onto them
 * rather than new machinery, which is the whole of what earns it a place.
 */
constexpr SettingsItem AUDIO_ITEMS[] = {
    {SettingsControlKind::Slider, "Master", "Everything, after its own submix.", 0, 0.0f, 1.0f},
    {SettingsControlKind::Slider, "World", "Hulls, weapons, the grid around you.", 0, 0.0f, 1.0f},
    {SettingsControlKind::Slider, "Interface", "Presses, confirmations, refusals.", 0, 0.0f, 1.0f},
    {SettingsControlKind::Slider, "Music", "", 0, 0.0f, 1.0f},
    {SettingsControlKind::Slider, "Alerts", "Combat and arrival. Ducks the others while it plays.", 0, 0.0f, 1.0f},
    {SettingsControlKind::Slider, "Ambience", "", 0, 0.0f, 1.0f},
};

/*
 * ACCOUNT -- the print's own instruction, followed literally: *"Every row here
 * waits on 06 leaving placeholder -- the section exists so the shape is known,
 * not so it can be built."*
 *
 * So the rows exist, are drawn disabled, and say what they are waiting for. The
 * alternative -- an empty section, or no rail entry -- is the arrangement where
 * the screen grows an entry later for no visible reason, and a player who went
 * looking for sign-out concluded there wasn't one.
 */
constexpr SettingsItem ACCOUNT_ITEMS[] = {
    {SettingsControlKind::Placeholder, "Sign out", "Waiting on the account system.", 0, 0.0f, 0.0f},
    {SettingsControlKind::Placeholder, "Active sessions", "Waiting on the account system.", 0, 0.0f, 0.0f},
    {SettingsControlKind::Placeholder, "Erasure request", "Waiting on the account system.", 0, 0.0f, 0.0f},
};

/// How tall one row is, by kind. The three heights are the print's, and the
/// floor applies to all of them because all of them are pressed -- including the
/// readouts, which are not pressed but must not be shorter than their neighbours
/// or the run would read as two lists.
[[nodiscard]] float RowHeight(SettingsControlKind _kind, float _scale, const SettingsScreenTuning& _tuning) noexcept
{
  switch (_kind)
  {
  case SettingsControlKind::Choice:
    return SettingsRowHeightPixels(_tuning.choiceRowHeight, _scale);
  case SettingsControlKind::Slider:
    return SettingsRowHeightPixels(_tuning.sliderRowHeight, _scale);
  case SettingsControlKind::Toggle:
  case SettingsControlKind::Readout:
  case SettingsControlKind::Placeholder:
  default:
    return SettingsRowHeightPixels(_tuning.rowHeight, _scale);
  }
}

} // namespace

const char* SettingsSectionName(SettingsSection _section) noexcept
{
  switch (_section)
  {
  case SettingsSection::Input:
    return "INPUT";
  case SettingsSection::Display:
    return "DISPLAY";
  case SettingsSection::Audio:
    return "AUDIO";
  case SettingsSection::Account:
    return "ACCOUNT";
  case SettingsSection::Accessibility:
  default:
    return "ACCESSIBILITY";
  }
}

const char* SettingsSectionNote(SettingsSection _section) noexcept
{
  switch (_section)
  {
  case SettingsSection::Input:
    return "The wheel, the dwell, and which hand holds the device.";
  case SettingsSection::Display:
    return "Short by design: every visual enhancement is decided from what the device reports.";
  case SettingsSection::Audio:
    return "Category volumes over the five submixes the mix already defines.";
  case SettingsSection::Account:
    return "Not yet built. The rows are here so the shape is known.";
  case SettingsSection::Accessibility:
  default:
    return "Every option here re-lays out the interface rather than scaling a bitmap, so nothing blurs.";
  }
}

bool SettingsSectionAvailable(SettingsSection _section) noexcept
{
  switch (_section)
  {
  // `AudioDevice` takes its gains at creation and has no path to set them on a
  // live mixer, so a volume slider here would only take effect at next start --
  // which the header's own promise, CHANGES APPLY IMMEDIATELY, forbids. The
  // section is a view onto machinery that exists; what is missing is one setter.
  case SettingsSection::Audio:
  // And the account system does not exist at all.
  case SettingsSection::Account:
    return false;
  case SettingsSection::Accessibility:
  case SettingsSection::Input:
  case SettingsSection::Display:
  default:
    return true;
  }
}

const char* SettingsSectionBlockedReason(SettingsSection _section) noexcept
{
  switch (_section)
  {
  case SettingsSection::Audio:
    return "The mix has five submixes and a master, and no way to set a gain on a running mixer. "
           "Sliders here would only take effect at next start, and this screen applies changes immediately.";
  case SettingsSection::Account:
    return "Waiting on the account system. Sign out, active sessions and erasure live here when it lands.";
  default:
    return "";
  }
}

std::span<const SettingsItem> SettingsItemsFor(SettingsSection _section) noexcept
{
  switch (_section)
  {
  case SettingsSection::Input:
    return INPUT_ITEMS;
  case SettingsSection::Display:
    return DISPLAY_ITEMS;
  case SettingsSection::Audio:
    return AUDIO_ITEMS;
  case SettingsSection::Account:
    return ACCOUNT_ITEMS;
  case SettingsSection::Accessibility:
  default:
    return ACCESSIBILITY_ITEMS;
  }
}

const char* SettingsChoiceLabel(SettingsSection _section, std::uint32_t _item, std::uint32_t _option) noexcept
{
  if (_section == SettingsSection::Accessibility && _item == 0)
  {
    // The three tables `ResolveHudPalette` knows, in the order it resolves them
    // and the order the cards are drawn in.
    switch (_option)
    {
    case 1:
      return "DEUTERANOPIA";
    case 2:
      return "TRITANOPIA";
    case 0:
    default:
      return "DEFAULT";
    }
  }
  if (_section == SettingsSection::Input && _item == 0)
  {
    // LEFT first, which is the print's order and the reason `Handedness`
    // numbers `Right` zero rather than reading its enum order off the screen.
    return _option == 0 ? "LEFT" : "RIGHT";
  }
  return "";
}

SettingsScreenLayout ResolveSettingsScreen(std::uint32_t _viewportWidth, std::uint32_t _viewportHeight, float _scale,
                                           const SettingsScreenTuning& _tuning) noexcept
{
  const float scale = SafeScale(_scale);
  const auto width = static_cast<float>(_viewportWidth);
  const auto height = static_cast<float>(_viewportHeight);

  SettingsScreenLayout layout;
  layout.scale = scale;
  layout.viewport = UiRect{0.0f, 0.0f, width, height};

  /*
   * The header meets the target floor, because it carries a control.
   *
   * At 0.8x its design height is 36.8 px, and a bar that scaled freely would
   * put the only way off this screen below the floor the screen exists to
   * enforce -- on the one control a player reaches for when they have already
   * changed something they did not mean to. So the *bar* is floored rather than
   * just the word in it: the alternative is a target that overhangs its own bar
   * and overlaps the rail beneath.
   *
   * A caught defect rather than a considered one. The first draft floored the
   * word's width and took the height from the bar, which read correctly and was
   * 36.8 px tall.
   */
  const float headerHeight = std::min(SettingsRowHeightPixels(_tuning.headerHeight, scale), height);
  layout.header = UiRect{0.0f, 0.0f, width, headerHeight};

  // BACK is a word at the left of the header, sized to the floor rather than to
  // its own text.
  const float backWidth = std::min(SettingsRowHeightPixels(_tuning.headerHeight, scale) * 2.0f, width);
  layout.back = UiRect{0.0f, 0.0f, backWidth, headerHeight};

  const float contentTop = headerHeight;
  const float contentHeight = std::max(0.0f, height - contentTop);

  /*
   * Three columns: rail, body, right. The rail and the right column take their
   * widths and the body takes what is left -- clamped so a narrow window eats
   * into them rather than giving the body a negative width, which is
   * `ResolveUiLayout`'s posture.
   */
  const float navWidth = std::min(_tuning.navWidth * scale, width);
  const float rightWidth = std::min(_tuning.rightWidth * scale, std::max(0.0f, width - navWidth));

  const float footerHeight = std::min(_tuning.footerHeight * scale, contentHeight);
  layout.nav = UiRect{0.0f, contentTop, navWidth, std::max(0.0f, contentHeight - footerHeight)};
  layout.footer = UiRect{0.0f, contentTop + layout.nav.height, navWidth, footerHeight};

  const float bodyX = navWidth;
  const float bodyWidth = std::max(0.0f, width - navWidth - rightWidth);
  const float headingHeight = std::min(_tuning.bodyHeadingHeight * scale + _tuning.bodyPaddingY * 2.0f * scale,
                                       contentHeight);
  layout.bodyHeading = UiRect{bodyX, contentTop, bodyWidth, headingHeight};
  layout.body = UiRect{bodyX, contentTop + headingHeight, bodyWidth, std::max(0.0f, contentHeight - headingHeight)};

  /*
   * The right column, top to bottom: the preview, the audit under it, the
   * handedness panel under that, and the two resets across the foot.
   *
   * Laid out from the top for the preview and the audit and from the *bottom*
   * for the resets, so the handedness panel absorbs the slack. That is
   * `StationScreen`'s parking-diagram rule applied here, and for the same
   * reason: every other block has a height its contents need, and the one that
   * can be any size is the one carrying a diagram.
   */
  const float rightX = bodyX + bodyWidth;
  const float pad = _tuning.bodyPaddingX * scale;
  const float innerX = rightX + pad;
  const float innerWidth = std::max(0.0f, rightWidth - 2.0f * pad);

  // Letterboxed, not filled (§7): the preview is a proof about proportion, so
  // it holds its aspect and gives back whatever height that leaves.
  const float previewAspect = _tuning.previewAspect > 0.0f ? _tuning.previewAspect : 1.0f;
  const float previewHeight = innerWidth / previewAspect;
  float pen = contentTop + pad;
  layout.preview = UiRect{innerX, pen, innerWidth, previewHeight};
  pen += previewHeight + pad;

  const float auditHeight = _tuning.auditHeadingHeight * scale + _tuning.auditRowHeight * scale * 4.0f;
  layout.audit = UiRect{innerX, pen, innerWidth, auditHeight};
  pen += auditHeight + pad;

  const float resetHeight = SettingsRowHeightPixels(_tuning.resetHeight, scale);
  const float resetGap = _tuning.resetGap * scale;
  const float resetTop = contentTop + contentHeight - pad - resetHeight;
  const float resetWidth = std::max(0.0f, (innerWidth - resetGap) * 0.5f);
  layout.resetSection = UiRect{innerX, resetTop, resetWidth, resetHeight};
  layout.resetAll = UiRect{innerX + resetWidth + resetGap, resetTop, resetWidth, resetHeight};

  layout.handedness = UiRect{innerX, pen, innerWidth, std::max(0.0f, resetTop - pad - pen)};

  return layout;
}

std::uint32_t BuildSettingsNav(const UiRect& _nav, float _scale, const SettingsScreenTuning& _tuning,
                               std::span<SettingsNavRow> _outRows) noexcept
{
  const float scale = SafeScale(_scale);
  const float padX = _tuning.navPaddingX * scale;
  const float padY = _tuning.navPaddingY * scale;
  const float rowHeight = SettingsRowHeightPixels(_tuning.navRowHeight, scale);
  const float gap = _tuning.navRowGap * scale;
  const float rowWidth = std::max(0.0f, _nav.width - 2.0f * padX);

  std::uint32_t written = 0;
  float pen = _nav.y + padY;
  for (std::uint32_t index = 0; index < SETTINGS_SECTION_COUNT; ++index)
  {
    if (written >= _outRows.size())
    {
      break;
    }
    // Dropped rather than shrunk, like every other run on this screen. A rail
    // that reflowed at some window height would be a layout engine (R9).
    if (pen + rowHeight > _nav.Bottom())
    {
      break;
    }

    const auto section = static_cast<SettingsSection>(index);
    SettingsNavRow row;
    row.rect = UiRect{_nav.x + padX, pen, rowWidth, rowHeight};
    row.section = section;
    // Asked rather than decided here, so the rail and the body agree about
    // which sections are live and the reason sits beside the answer.
    row.enabled = SettingsSectionAvailable(section);
    _outRows[written] = row;
    ++written;

    pen += rowHeight + gap;
  }
  return written;
}

const SettingsNavRow* HitSettingsNav(std::span<const SettingsNavRow> _rows, float _x, float _y) noexcept
{
  for (const SettingsNavRow& row : _rows)
  {
    if (row.enabled && row.rect.Contains(_x, _y))
    {
      return &row;
    }
  }
  return nullptr;
}

std::uint32_t BuildSettingsRows(std::span<const SettingsItem> _items, const UiRect& _body, float _scale,
                                const SettingsScreenTuning& _tuning, std::uint32_t _firstRow,
                                std::span<SettingsRowRect> _outRows) noexcept
{
  const float scale = SafeScale(_scale);
  const float padX = _tuning.bodyPaddingX * scale;
  const float padY = _tuning.bodyPaddingY * scale;
  const float gap = _tuning.rowGap * scale;
  const float rowWidth = std::max(0.0f, _body.width - 2.0f * padX);

  std::uint32_t written = 0;
  float pen = _body.y + padY;

  for (std::uint32_t index = 0; index < _items.size(); ++index)
  {
    const SettingsItem& item = _items[index];
    const float height = RowHeight(item.kind, scale, _tuning);

    /*
     * Scrolled past: stepped over without advancing the pen, so the first row
     * shown sits at the top of the body rather than wherever its own height put
     * it in the full run.
     *
     * **Whole rows only**, which is ADR-020's fence -- *"No clip rectangle on
     * the Ui instance and no virtualised list beyond §4's whole-row cull"*. An
     * earlier draft took a pixel offset instead, which lands mid-row on a
     * screen whose rows are three different heights, and there is no clip
     * rectangle to hide the half hanging into the heading above.
     */
    if (index < _firstRow)
    {
      continue;
    }
    if (pen + height > _body.Bottom() || written >= _outRows.size())
    {
      break;
    }

    SettingsRowRect row;
    row.rect = UiRect{_body.x + padX, pen, rowWidth, height};
    row.item = index;
    row.kind = item.kind;
    row.optionCount = item.optionCount;
    // A readout is not pressed and a placeholder is waiting on a system nobody
    // has built. Both are drawn; neither is a target.
    row.enabled = item.kind != SettingsControlKind::Readout && item.kind != SettingsControlKind::Placeholder;

    switch (item.kind)
    {
    case SettingsControlKind::Toggle:
    {
      // The switch, at the right of the row and centred on it.
      const float switchWidth = _tuning.toggleWidth * scale;
      const float switchHeight = _tuning.toggleHeight * scale;
      // Sized to the floor as a *target* even though it is drawn at the print's
      // size: the rect that is pressed is not the rect that is painted, which
      // is the whole reason `control` is resolved separately from the artwork.
      const float targetHeight = std::max(switchHeight, std::min(TARGET_FLOOR_PIXELS, height));
      const float targetWidth = std::max(switchWidth, TARGET_FLOOR_PIXELS);
      row.control = UiRect{row.rect.Right() - targetWidth, row.rect.y + (height - targetHeight) * 0.5f, targetWidth,
                           targetHeight};
      break;
    }
    case SettingsControlKind::Slider:
    {
      // The track runs under the label, inset by the handle's radius at both
      // ends so the handle's own extent stays inside the row.
      const float radius = _tuning.sliderHandleRadius * scale;
      const float trackTop = row.rect.y + _tuning.rowNoteOffset * scale * 2.0f;
      const float trackHeight = std::max(_tuning.sliderTrackHeight * scale, TARGET_FLOOR_PIXELS * 0.5f);
      row.control = UiRect{row.rect.x + radius, trackTop, std::max(0.0f, rowWidth - 2.0f * radius), trackHeight};
      break;
    }
    case SettingsControlKind::Choice:
    {
      // The cards fill the row under its label and note.
      const float cardsTop = row.rect.y + _tuning.rowNoteOffset * scale * 1.6f;
      row.control = UiRect{row.rect.x, cardsTop, rowWidth, std::max(0.0f, row.rect.Bottom() - cardsTop)};
      break;
    }
    case SettingsControlKind::Readout:
    case SettingsControlKind::Placeholder:
    default:
      // Nothing to press. A collapsed control rect rather than the row's,
      // because `HitSettingsRow` tests `control` and a full-width one here
      // would make a disabled row swallow presses meant for nothing.
      row.control = UiRect{row.rect.x, row.rect.y, 0.0f, 0.0f};
      break;
    }

    _outRows[written] = row;
    ++written;
    pen += height + gap;
  }
  return written;
}

float SettingsRowsHeight(std::span<const SettingsItem> _items, float _scale,
                         const SettingsScreenTuning& _tuning) noexcept
{
  const float scale = SafeScale(_scale);
  const float gap = _tuning.rowGap * scale;

  float total = 0.0f;
  for (std::uint32_t index = 0; index < _items.size(); ++index)
  {
    total += RowHeight(_items[index].kind, scale, _tuning);
    if (index + 1 < _items.size())
    {
      total += gap;
    }
  }
  return total;
}

const SettingsRowRect* HitSettingsRow(std::span<const SettingsRowRect> _rows, float _x, float _y) noexcept
{
  for (const SettingsRowRect& row : _rows)
  {
    if (row.enabled && row.control.Contains(_x, _y))
    {
      return &row;
    }
  }
  return nullptr;
}

UiRect SettingsChoiceCard(const SettingsRowRect& _row, std::uint32_t _option, float _scale,
                          const SettingsScreenTuning& _tuning) noexcept
{
  if (_row.kind != SettingsControlKind::Choice || _option >= _row.optionCount || _row.optionCount == 0)
  {
    return UiRect{_row.control.x, _row.control.y, 0.0f, 0.0f};
  }

  const float scale = SafeScale(_scale);
  const float gap = _tuning.choiceCardGap * scale;
  const auto count = static_cast<float>(_row.optionCount);
  const float cardWidth = std::max(0.0f, (_row.control.width - gap * (count - 1.0f)) / count);
  return UiRect{_row.control.x + (cardWidth + gap) * static_cast<float>(_option), _row.control.y, cardWidth,
                _row.control.height};
}

std::uint32_t HitSettingsChoice(const SettingsRowRect& _row, float _scale, const SettingsScreenTuning& _tuning,
                                float _x, float _y) noexcept
{
  for (std::uint32_t option = 0; option < _row.optionCount; ++option)
  {
    if (SettingsChoiceCard(_row, option, _scale, _tuning).Contains(_x, _y))
    {
      return option;
    }
  }
  return _row.optionCount;
}

UiRect SettingsSliderHandle(const SettingsRowRect& _row, float _normalised, float _scale,
                            const SettingsScreenTuning& _tuning) noexcept
{
  const float scale = SafeScale(_scale);
  const float radius = _tuning.sliderHandleRadius * scale;
  const float clamped = std::clamp(_normalised, 0.0f, 1.0f);
  const float centreX = _row.control.x + _row.control.width * clamped;
  const float centreY = _row.control.y + _row.control.height * 0.5f;
  return UiRect{centreX - radius, centreY - radius, radius * 2.0f, radius * 2.0f};
}

float SettingsSliderValueAt(const SettingsRowRect& _row, float _x) noexcept
{
  if (_row.control.width <= 0.0f)
  {
    // A collapsed track has no position to report. Zero rather than a divide,
    // and the caller sees a value it can act on rather than a NaN it cannot.
    return 0.0f;
  }
  return std::clamp((_x - _row.control.x) / _row.control.width, 0.0f, 1.0f);
}

} // namespace Neuron
