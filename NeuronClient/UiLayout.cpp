#include "pch.h"

#include "UiLayout.h"

#include <algorithm>

namespace Neuron
{
namespace
{

/// The scale a control offers, clamped to what the settings sheet specifies.
/// Outside that range the text stops matching the panels it sits in, which is
/// worse than refusing the setting.
[[nodiscard]] float ClampScale(float _scale) noexcept
{
  return std::clamp(_scale, 0.8f, 1.6f);
}

} // namespace

UiLayout ResolveUiLayout(std::uint32_t _viewportWidth, std::uint32_t _viewportHeight, float _scale,
                         const UiTuning& _tuning) noexcept
{
  UiLayout layout;
  layout.scale = ClampScale(_scale);

  const float width = static_cast<float>(_viewportWidth);
  const float height = static_cast<float>(_viewportHeight);
  layout.viewport = UiRect{0.0f, 0.0f, width, height};

  const float topBar = _tuning.topBarHeight * layout.scale;
  const float roster = _tuning.rosterWidth * layout.scale;
  const float ability = _tuning.abilityWidth * layout.scale;
  const float context = _tuning.contextBarHeight * layout.scale;
  const float command = _tuning.commandRowHeight * layout.scale;

  layout.topBar = UiRect{0.0f, 0.0f, width, std::min(topBar, height)};

  // Bottom up: the command row sits on the floor and the context bar above it,
  // because the print draws the buttons at the very edge where a thumb reaches.
  layout.commandRow = UiRect{0.0f, height - command, width, command};
  layout.contextBar = UiRect{0.0f, layout.commandRow.y - context, width, context};

  const float bandTop = layout.topBar.Bottom();
  const float bandBottom = layout.contextBar.y;
  const float bandHeight = std::max(0.0f, bandBottom - bandTop);

  layout.roster = UiRect{0.0f, bandTop, std::min(roster, width), bandHeight};
  layout.abilityRack = UiRect{std::max(0.0f, width - ability), bandTop, std::min(ability, width), bandHeight};

  // What is left is the world, and it is the reason the chrome is a border
  // rather than an overlay: nothing here composites over the plane.
  const float worldLeft = layout.roster.Right();
  const float worldRight = layout.abilityRack.x;
  layout.world = UiRect{worldLeft, bandTop, std::max(0.0f, worldRight - worldLeft), bandHeight};

  layout.criticalToast = UiRect{(width - _tuning.criticalWidth * layout.scale) * 0.5f,
                                layout.topBar.Bottom() + _tuning.criticalTopMargin * layout.scale,
                                _tuning.criticalWidth * layout.scale, _tuning.criticalHeight * layout.scale};

  return layout;
}

UiRect UiLayout::ToastSlot(std::size_t _index, const UiTuning& _tuning) const noexcept
{
  const float toastWidth = _tuning.toastWidth * scale;
  const float toastHeight = _tuning.toastHeight * scale;
  const float gap = _tuning.toastGap * scale;
  const float padding = _tuning.padding * scale;

  /*
   * Stacked upward from just above the context bar.
   *
   * "No toast ever overlaps the context bar" is the sheet's rule and it is not
   * decoration: the bar is where orders are issued, and a toast covering
   * ATTACK mid-fight is a lost engagement. Anchoring to the bar's top rather
   * than to the viewport's bottom is what makes that true at every scale --
   * the bar grows with the scale and the stack moves with it.
   *
   * Also clear of the ability rack, so the two right-hand surfaces do not
   * fight: the rack is the wider commitment and the stack yields to it.
   */
  const float right = abilityRack.x - padding;
  const float bottom = contextBar.y - padding - static_cast<float>(_index) * (toastHeight + gap);

  return UiRect{right - toastWidth, bottom - toastHeight, toastWidth, toastHeight};
}

} // namespace Neuron
