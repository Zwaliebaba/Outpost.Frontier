#pragma once

#include "UiLayout.h"

#include <cstdint>
#include <span>

/*
 * Where everything on the settings surface goes (`settings.png` §1, ADR-020 §8,
 * N3).
 *
 * `StationScreen`'s shape applied to the one screen in the corpus whose whole
 * subject is controls: **laid out and hit-tested in one file**, so the thing you
 * press is the thing you see. That rule pays hardest here, because a settings
 * body is a run of a dozen heterogeneous rows and every one of them is a rect a
 * draw and an input handler could disagree about independently.
 *
 * **Numbers read off the print, authored at 1440x900** (ADR-020 §7). The zones
 * are exact: a 46 px header bar, a 232 px nav rail, a 380 px right column, and
 * the section body takes what is left. Everything below is those numbers times
 * the UI scale, which is R9's entire permitted flexibility.
 *
 * **A flat run of rows, not a widget tree.** ADR-020's fence is explicit -- no
 * parent/child graph, no invalidation, no layout solver -- and this stays behind
 * it the way `BuildStationTabRow` does: a section is an *array* of items, laid
 * out top to bottom in one downward pass, rebuilt every frame, with no row
 * knowing about any other. What makes that sufficient is that no control here
 * contains another one.
 *
 * **The screen lays out; it does not know what a setting means.** An item
 * carries its kind, its words and how many options it has; the *value* is
 * passed in for the draw and the *press* is handed back as a row and an option.
 * `ClientApp` owns the one switch that turns a row index into a config field,
 * which is the same split `RosterRow::groupId` already makes -- an opaque
 * handle out, the meaning kept where the state is.
 *
 * **Overflow follows §7's declared rule for this surface: the section body
 * scrolls and the live preview letterboxes.** The preview is a proof about
 * proportion and colour, so distorting it breaks the proof -- which is why it
 * is the one rect here that holds its aspect rather than filling what it is
 * given.
 */

namespace Neuron
{

/*
 * The five sections, in nav order (`settings.png` §2).
 *
 * The order is the print's and it is an argument rather than a list: the two
 * the print marks REQUIRED come first, the two that are views onto machinery
 * that already exists follow, and the one blocked on a system nobody has built
 * is last. A player scanning down the rail meets them in decreasing order of
 * whether they can do anything.
 */
enum class SettingsSection : std::uint8_t
{
  Accessibility = 0,
  Input = 1,
  Display = 2,
  Audio = 3,
  Account = 4
};

inline constexpr std::uint32_t SETTINGS_SECTION_COUNT = 5;

/// The rail's word for one. Never null, for the same reason `OrderReasonText`
/// is not.
[[nodiscard]] const char* SettingsSectionName(SettingsSection _section) noexcept;

/// And the heading over the body, which is the same word -- spelled once, so a
/// rail and a heading cannot disagree about which section you are in.
[[nodiscard]] const char* SettingsSectionNote(SettingsSection _section) noexcept;

/*
 * Can this section be used yet?
 *
 * Two are false today and the reasons are different in kind, which is why this
 * is a function with a companion sentence rather than a bool on the enum:
 * ACCOUNT waits on a system nobody has built, and AUDIO waits on a *client* API
 * -- `AudioDevice` takes its gains once, at creation, and has no path to set
 * them on a live mixer.
 *
 * Both are drawn, both are refused, and both say why. The print's instruction
 * for ACCOUNT generalises to any section in this state: *"the section exists so
 * the shape is known, not so it can be built"*. A rail that hid them would grow
 * entries later for no visible reason, and a player who went looking for volume
 * would conclude there wasn't any.
 */
[[nodiscard]] bool SettingsSectionAvailable(SettingsSection _section) noexcept;

/// What a blocked section is waiting for, drawn where its controls would be.
/// Empty for an available one.
[[nodiscard]] const char* SettingsSectionBlockedReason(SettingsSection _section) noexcept;

/*
 * What shape a row is.
 *
 * Five kinds, and the set is closed on purpose: every control the print draws is
 * one of these, and the three it explicitly refuses -- a quality menu, a gesture
 * rebinder, a colour picker -- would each have needed a sixth. A closed set is
 * what stops "a settings screen is where design debt goes to hide".
 */
enum class SettingsControlKind : std::uint8_t
{
  /// A row of cards, one selected. The palette strip and the handedness pair.
  Choice,

  /// A track with a handle, and tick marks under it. UI scale, volumes.
  Slider,

  /// A switch. The readability rules.
  Toggle,

  /// A value the player cannot change here, shown because it is true and they
  /// would otherwise have to guess. The display section is mostly these.
  Readout,

  /// A row that exists so the shape is known, drawn disabled with its reason.
  /// The print's own instruction for ACCOUNT: *"the section exists so the shape
  /// is known, not so it can be built"*.
  Placeholder
};

/*
 * One setting, as the screen knows it.
 *
 * The words are here rather than in `ClientApp` for `HudRoster`'s reason: a
 * label and its note are laid out against each other, so the file that measures
 * them is the file that should hold them. They are interface words rather than
 * game words, which is what makes that legal (ADR-014 §2c).
 */
struct SettingsItem
{
  SettingsControlKind kind = SettingsControlKind::Toggle;

  /// The row's word, in the print's sentence case.
  const char* label = "";

  /// The line under it. The print gives one for nearly every control, and they
  /// are not decoration: *"Overrides the selection + damaged rule"* is the only
  /// place a player learns what the toggle actually does.
  const char* note = "";

  /// `Choice` only: how many cards. Their words come from the caller, because
  /// the palette's three are also the three the live preview draws.
  std::uint32_t optionCount = 0;

  /// `Slider` only: the ends, in the setting's own units, for the scale marks.
  float minimum = 0.0f;
  float maximum = 1.0f;
};

/*
 * The items in a section, in print order.
 *
 * A span into a static table rather than a fill-a-buffer call, because the
 * description never changes at runtime -- only the values do, and those are the
 * caller's.
 */
[[nodiscard]] std::span<const SettingsItem> SettingsItemsFor(SettingsSection _section) noexcept;

/*
 * The word on one card of a `Choice` row.
 *
 * Here rather than beside the values because these are *words*, and this file's
 * rule is that the words are laid out where they are measured. A caller that
 * supplied them would be a caller that has to keep its order in step with an
 * option index it does not otherwise use.
 *
 * Never null: an option nobody drew is an empty string rather than a crash, on
 * `OrderReasonText`'s terms.
 */
[[nodiscard]] const char* SettingsChoiceLabel(SettingsSection _section, std::uint32_t _item,
                                              std::uint32_t _option) noexcept;

/// Sizes at scale 1.0, off the print.
struct SettingsScreenTuning
{
  /// The header bar: `BACK | SETTINGS` on the left, the apply notice right.
  float headerHeight = 46.0f;

  /// The nav rail down the left, and a row in it.
  float navWidth = 232.0f;
  float navRowHeight = 46.0f;
  float navRowGap = 2.0f;
  float navPaddingX = 16.0f;
  float navPaddingY = 18.0f;

  /// The right column: live preview, contrast audit, handedness, the resets.
  float rightWidth = 380.0f;

  /*
   * The preview's aspect, held rather than filled (§7).
   *
   * 16:9, because what it draws is a scrap of the tactical view and a preview
   * at a different aspect than the screen it previews is a preview of something
   * else.
   */
  float previewAspect = 16.0f / 9.0f;

  /// The audit panel: a row per pair, and the print draws four.
  float auditRowHeight = 30.0f;
  float auditHeadingHeight = 26.0f;

  /// The reset buttons across the foot of the right column.
  float resetHeight = 44.0f;
  float resetGap = 12.0f;

  /// The body's own margins, and the run of rows down it.
  float bodyPaddingX = 26.0f;
  float bodyPaddingY = 22.0f;
  float bodyHeadingHeight = 34.0f;

  /*
   * A row, and it is the 48 px target floor plus air rather than a number
   * chosen for looks (ADR-020 §8, `settings.png` §1).
   *
   * The floor is *enforced and not scaled*, which is the one part of D15.4 the
   * touch reversal kept -- so this is the number that has to survive the
   * smallest UI scale the slider offers. `TargetHeightPixels` (`UiLayout.h`)
   * is where that promise is actually kept.
   */
  float rowHeight = 56.0f;
  float rowGap = 10.0f;

  /// A note sits under its label inside the same row, so the row is tall enough
  /// for two lines and the switch is centred against both.
  float rowNoteOffset = 26.0f;

  /// The pressable part of each kind, at the right of its row.
  float toggleWidth = 52.0f;
  float toggleHeight = 26.0f;
  float sliderTrackHeight = 6.0f;
  float sliderHandleRadius = 11.0f;
  float choiceCardGap = 12.0f;

  /// A `Choice` row is taller: its cards carry swatches and a word under them.
  float choiceRowHeight = 86.0f;

  /// And a `Slider` row is taller still, because the scale marks sit below the
  /// track with their numbers under them.
  float sliderRowHeight = 96.0f;

  /// The footer under the rail: build and schema, which is what a player is
  /// asked to read out when something has gone wrong.
  float footerHeight = 44.0f;
};

/*
 * The 48 px target floor moved out on 2026-08-23, to `UiLayout.h`.
 *
 * It was written here, for `settings.png`, and the strategic map became its
 * second caller -- which is the moment a rule stops being one screen's. It is
 * `TARGET_FLOOR_PIXELS` and `TargetHeightPixels` there, unchanged in value and
 * in behaviour, and the print's promise is still what it keeps: *"Touch targets
 * never drop below 48 px at any scale -- the floor is enforced, not scaled"*.
 * Every pressable row on this screen is still sized through it.
 */

/// The screen's zones, resolved for one viewport at one scale.
struct SettingsScreenLayout
{
  UiRect viewport;
  UiRect header;

  /// `BACK`, which is the only control in the header.
  UiRect back;

  /// The rail, and the footer under it.
  UiRect nav;
  UiRect footer;

  /// The selected section's heading, and the run of rows under it. `body` is
  /// what scrolls (§7).
  UiRect bodyHeading;
  UiRect body;

  /// The right column, top to bottom.
  UiRect preview;
  UiRect audit;
  UiRect handedness;
  UiRect resetSection;
  UiRect resetAll;

  float scale = 1.0f;
};

/*
 * Resolves the zones.
 *
 * A viewport smaller than the chrome is not an error -- the zones clamp and the
 * panels overlap rather than inverting, which is `ResolveUiLayout`'s posture and
 * for the same reason: dragging a window small must not put a quad with a
 * flipped winding on screen.
 */
[[nodiscard]] SettingsScreenLayout ResolveSettingsScreen(std::uint32_t _viewportWidth, std::uint32_t _viewportHeight,
                                                         float _scale, const SettingsScreenTuning& _tuning) noexcept;

/// One laid-out rail row.
struct SettingsNavRow
{
  UiRect rect;
  SettingsSection section = SettingsSection::Accessibility;

  /// Drawn dimmed and refused by the hit test, exactly as a greyed command is.
  /// `Account` is the one that is false today, and the print says why.
  bool enabled = true;
};

/*
 * Lays the rail out, top to bottom, one row per section.
 *
 * Every section always gets a row, including the disabled one: the print is
 * explicit that ACCOUNT exists *so the shape is known*, and a rail that hid it
 * would be a rail that grows an entry later for no visible reason.
 */
[[nodiscard]] std::uint32_t BuildSettingsNav(const UiRect& _nav, float _scale, const SettingsScreenTuning& _tuning,
                                             std::span<SettingsNavRow> _outRows) noexcept;

/// Which section a press landed on, or null. A disabled row returns null rather
/// than itself, so a caller cannot forget to check.
[[nodiscard]] const SettingsNavRow* HitSettingsNav(std::span<const SettingsNavRow> _rows, float _x, float _y) noexcept;

/*
 * One laid-out control row.
 *
 * `rect` is the whole row -- what a hover highlights -- and `control` is the
 * part that is *pressed*. They differ because a toggle's switch is 52 px at the
 * right of a row that runs the width of the body, and treating the whole row as
 * the target would fire on a press against the explanatory note.
 */
struct SettingsRowRect
{
  UiRect rect;
  UiRect control;

  /// Which item this is, by index into the section's own span.
  std::uint32_t item = 0;

  SettingsControlKind kind = SettingsControlKind::Toggle;

  /// `Choice` only: the cards, laid out left to right inside `control`.
  std::uint32_t optionCount = 0;

  bool enabled = true;
};

/*
 * Lays a section's rows out down the body.
 *
 * A row that would run past the body's bottom edge is **dropped**, not shrunk
 * or wrapped -- `CommandRow`'s rule and the same argument. §7's declared
 * overflow answer for this surface is that the body *scrolls*, and `_firstRow`
 * is how: the run starts at that item, so a dropped row is one the player can
 * bring back rather than one the layout has decided against.
 *
 * **An item index rather than a pixel offset**, which is what makes the whole-
 * row cull the only virtualisation here (ADR-020's fence). Rows on this screen
 * are three different heights, so a pixel offset would land mid-row and there
 * is no clip rectangle to hide the half of it hanging into the heading above.
 * It is also the unit `UiScrollState` already counts in, so no caller has to
 * convert between the two and get the conversion wrong in one direction.
 *
 * The count returned is what fitted.
 */
[[nodiscard]] std::uint32_t BuildSettingsRows(std::span<const SettingsItem> _items, const UiRect& _body, float _scale,
                                              const SettingsScreenTuning& _tuning, std::uint32_t _firstRow,
                                              std::span<SettingsRowRect> _outRows) noexcept;

/// How tall the whole run is at this scale, fitted or not -- what a scrollbar
/// needs and what says whether there is anything to scroll.
[[nodiscard]] float SettingsRowsHeight(std::span<const SettingsItem> _items, float _scale,
                                       const SettingsScreenTuning& _tuning) noexcept;

/// Which row a press landed on, or null. Tests `control` rather than `rect`,
/// which is the whole reason both are resolved.
[[nodiscard]] const SettingsRowRect* HitSettingsRow(std::span<const SettingsRowRect> _rows, float _x,
                                                    float _y) noexcept;

/// One card of a `Choice` row, by index, or a collapsed rect for a row that is
/// not one. Resolved on demand rather than stored, because a row has at most a
/// handful and storing them would put a variable-length array in a fixed struct.
[[nodiscard]] UiRect SettingsChoiceCard(const SettingsRowRect& _row, std::uint32_t _option, float _scale,
                                        const SettingsScreenTuning& _tuning) noexcept;

/// Which card a press landed on, or `_row.optionCount` for none.
[[nodiscard]] std::uint32_t HitSettingsChoice(const SettingsRowRect& _row, float _scale,
                                              const SettingsScreenTuning& _tuning, float _x, float _y) noexcept;

/*
 * Where a slider's handle sits, and what a press on its track means.
 *
 * `_normalised` is 0..1 along the track rather than the setting's own units,
 * because the track does not know whether it is carrying a UI scale or a volume
 * -- and a caller that has to convert once on the way in and once on the way
 * out cannot get the two conversions out of step.
 */
[[nodiscard]] UiRect SettingsSliderHandle(const SettingsRowRect& _row, float _normalised, float _scale,
                                          const SettingsScreenTuning& _tuning) noexcept;

/// Where along its track a press landed, 0..1, clamped. A press left of the
/// track reads 0 and right of it reads 1, which is what a drag off the end of a
/// slider should do rather than jumping back.
[[nodiscard]] float SettingsSliderValueAt(const SettingsRowRect& _row, float _x) noexcept;

} // namespace Neuron
