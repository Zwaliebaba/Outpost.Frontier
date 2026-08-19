#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

/*
 * The sound bank (ADR-011 §11).
 *
 * `soundId -> { file, category, gain, pitchVariance, maxInstances, cooldownMs,
 * minDistance, maxDistance }`, read from `GameData/Audio/SoundBank.json` by the
 * ADR-012 parser. Design tuning is data: retuning a cue's falloff or its
 * instance cap is an edit to content, not a rebuild.
 *
 * Device-free on purpose, like every other half of this client that can be:
 * parsing and validating a bank needs no XAudio2, so `NeuronClientTests` covers
 * it headlessly and CI -- which has no audio device -- still runs it.
 */

namespace Neuron
{

/*
 * The five submixes of ADR-011 §2, and the order is the graph's order.
 *
 * `World3D` is the only spatialised one; the rest are 2D. `Count` is last so it
 * sizes the gain tables, which is why the categories are a contiguous enum
 * rather than strings past the parse.
 */
enum class SoundCategory : std::uint8_t
{
  World3D,
  Ambience,
  Ui,
  Music,
  Alerts,
  Count
};

inline constexpr std::uint32_t SOUND_CATEGORY_COUNT = static_cast<std::uint32_t>(SoundCategory::Count);

/// The authored name of a category, for diagnostics and for the parse. Index by
/// `SoundCategory`; anything past the end is the empty string rather than a read
/// off the end of the table.
[[nodiscard]] std::string_view SoundCategoryName(SoundCategory _category) noexcept;

/// The category a bank file names, or false if it names none of them. Case is
/// significant: the bank is authored content and a typo should be a diagnostic,
/// not a silent fallback to whichever category sorts first.
[[nodiscard]] bool TryParseSoundCategory(std::string_view _name, SoundCategory& _outCategory) noexcept;

/*
 * One cue.
 *
 * The distances are metres because the world is metres (ADR-009) and
 * `CurveDistanceScaler` is set to match, so a designer writing 4000 here means
 * four kilometres rather than four of something the mixer invented.
 */
struct SoundDef
{
  std::string id;
  std::string file;
  SoundCategory category = SoundCategory::Ui;

  /// Linear, applied on top of the category gain. Not decibels: the settings
  /// sheet's sliders are linear and two scales would be converted wrongly once.
  float gain = 1.0f;

  /// Fraction either side of unity pitch, so 0.05 is +/-5%. Zero is a cue that
  /// must sound identical every time, which is the right default for UI.
  float pitchVariance = 0.0f;

  /// How many of *this* cue may sound at once. Distinct from the voice pool's
  /// own cap: the pool protects the mixer, this protects the ear.
  std::uint32_t maxInstances = 8;

  /// The shortest gap between two starts of this cue. A click that retriggers
  /// every frame is a buzz, and the fix belongs in content.
  double cooldownMs = 0.0;

  /// Full volume inside `minDistance`, silent past `maxDistance`. Only read for
  /// `World3D`; a 2D cue has no position to be far from.
  float minDistanceMetres = 50.0f;
  float maxDistanceMetres = 5000.0f;
};

class SoundBank
{
public:
  /*
   * Parses a bank from JSON text.
   *
   * Returns false only when the document will not parse or its shape is wrong
   * -- a bank that is not an object, or has no `sounds`. A *single* bad entry
   * is skipped with a diagnostic and the rest of the bank still loads, because
   * one mistyped category should cost that cue and not the whole soundscape.
   * Every skip is named in `_outErrors`, so "the rest still loads" never means
   * "nobody was told".
   */
  [[nodiscard]] static bool Parse(std::string_view _text, SoundBank& _outBank, std::vector<std::string>& _outErrors);

  /// The cue with this id, or nullptr. Linear: a bank is a few dozen entries
  /// and the lookup happens on an event, not per voice per frame.
  [[nodiscard]] const SoundDef* Find(std::string_view _id) const noexcept;

  [[nodiscard]] std::span<const SoundDef> Sounds() const noexcept
  {
    return m_sounds;
  }

  [[nodiscard]] bool Empty() const noexcept
  {
    return m_sounds.empty();
  }

private:
  std::vector<SoundDef> m_sounds;
};

} // namespace Neuron
