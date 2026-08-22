#pragma once

#include "SurfaceStack.h"

#include <cstdint>

/*
 * Who owns the keyboard (ADR-020 §3).
 *
 * **At most one focus owner exists**, and that is the whole of the design. A
 * client with two would have to decide which of them a keystroke belongs to on
 * every key, which is the arrangement where "W" both types and pans -- the
 * defect §2's keyboard order exists to make impossible. One owner, named by the
 * surface it is on and the widget it is, makes the routing question a
 * comparison rather than a policy.
 *
 * **The surface is part of the name, not decoration.** A widget id is unique
 * only within the screen that laid it out -- widget 3 on the hangar and widget
 * 3 on the settings sheet are different controls -- so focus that named only the
 * widget would survive a navigation and land on whatever happened to share its
 * number. Naming the surface is what lets the exit rule below be exact instead
 * of "clear everything and hope".
 */

namespace Neuron
{

/*
 * A widget's name within its surface, assigned by the screen that lays it out.
 *
 * Opaque here, and never zero for a real widget: zero is the absence of one,
 * which is what makes a default-constructed `UiFocus` unfocused without a
 * second flag to keep in step with the ids.
 */
using WidgetId = std::uint16_t;
inline constexpr WidgetId NO_WIDGET = 0;

/*
 * What kind of thing holds focus, because the two kinds claim different
 * amounts of the keyboard (ADR-020 §3, §8).
 *
 * An editable field claims the **text** channel: printable keys go into it and
 * the editing keys act on it, while F1 and Escape still route past. A
 * binding-capture control -- the settings sheet arming a key for rebinding --
 * claims the **whole** keyboard channel, F-keys included, because the next key
 * pressed is the answer it is waiting for and a diagnostics toggle that fired
 * instead would be a binding the player could not set.
 */
enum class FocusKind : std::uint8_t
{
  None = 0,
  EditableField,
  BindingCapture
};

/*
 * The one owner.
 *
 * Cleared by three events and no others (ADR-020 §3): the surface it is on
 * exits, a pointer claim lands outside it, and the window is deactivated. All
 * three are the router's to notice -- this type holds the answer and does not
 * go looking for it, which is what keeps it device-free.
 */
class UiFocus
{
public:
  void Claim(SurfaceId _surface, WidgetId _widget, FocusKind _kind) noexcept
  {
    if (_widget == NO_WIDGET || _kind == FocusKind::None)
    {
      // A claim that names nothing is a clear. Refusing it instead would leave
      // the caller holding a half-answer to check for.
      Clear();
      return;
    }
    m_surface = _surface;
    m_widget = _widget;
    m_kind = _kind;
  }

  void Clear() noexcept
  {
    m_widget = NO_WIDGET;
    m_kind = FocusKind::None;
  }

  /// The surface-exit rule. Exact rather than unconditional: a client that
  /// cleared focus on *any* exit would drop the hangar's rename box because the
  /// settings sheet closed over it and went away again.
  void ClearIfOn(SurfaceId _surface) noexcept
  {
    if (Held() && m_surface == _surface)
    {
      Clear();
    }
  }

  [[nodiscard]] bool Held() const noexcept { return m_widget != NO_WIDGET; }
  [[nodiscard]] bool HeldBy(SurfaceId _surface, WidgetId _widget) const noexcept
  {
    return Held() && m_surface == _surface && m_widget == _widget;
  }

  [[nodiscard]] SurfaceId Surface() const noexcept { return m_surface; }
  [[nodiscard]] WidgetId Widget() const noexcept { return m_widget; }
  [[nodiscard]] FocusKind Kind() const noexcept { return m_kind; }

  /// Whether the owner takes every key, F-keys included. The one question §2's
  /// keyboard order asks of this type.
  [[nodiscard]] bool ClaimsWholeKeyboard() const noexcept { return m_kind == FocusKind::BindingCapture; }

private:
  SurfaceId m_surface = SurfaceId::Tactical;
  WidgetId m_widget = NO_WIDGET;
  FocusKind m_kind = FocusKind::None;
};

} // namespace Neuron
