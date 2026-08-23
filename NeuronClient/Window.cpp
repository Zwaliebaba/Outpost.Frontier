#include "pch.h"

#include "Window.h"

#include "Log.h"
#include "TextEditState.h"

namespace Neuron
{
namespace
{

constexpr const wchar_t* WINDOW_CLASS_NAME = L"OutpostFrontierWindow";

std::wstring ToWide(const std::string& _utf8)
{
  if (_utf8.empty())
  {
    return {};
  }
  const int count = MultiByteToWideChar(CP_UTF8, 0, _utf8.c_str(), static_cast<int>(_utf8.size()), nullptr, 0);
  std::wstring result(static_cast<std::size_t>(count), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, _utf8.c_str(), static_cast<int>(_utf8.size()), result.data(), count);
  return result;
}

} // namespace

Window::~Window()
{
  Destroy();
}

bool Window::Create(const WindowDesc& _desc)
{
  m_instance = GetModuleHandleW(nullptr);

  // Per-monitor v2 so the client area is real pixels on a scaled display
  // rather than a stretched bitmap. Failure is not fatal: an older system
  // simply gets the system-DPI behaviour.
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

  WNDCLASSEXW windowClass{};
  windowClass.cbSize = sizeof(windowClass);
  windowClass.style = CS_HREDRAW | CS_VREDRAW;
  windowClass.lpfnWndProc = &Window::WindowProc;
  windowClass.hInstance = m_instance;
  windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  windowClass.lpszClassName = WINDOW_CLASS_NAME;
  // No background brush: the swapchain owns every pixel, and letting GDI paint
  // one produces a white flash on the first frames.
  windowClass.hbrBackground = nullptr;

  if (RegisterClassExW(&windowClass) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
  {
    NEURON_LOG_ERROR("RegisterClassExW failed (%lu)", GetLastError());
    return false;
  }
  m_classRegistered = true;

  /*
   * Created windowed whatever the configuration says, and put into borderless
   * fullscreen below if it asked for it.
   *
   * Two reasons rather than one. The mode is a *runtime* mode now, so it has to
   * come from the one function that owns the transition or the configured path
   * and the Alt+Enter path would be two different fullscreens. And the windowed
   * box created here is what the toggle restores to, so a session that starts
   * fullscreen still has somewhere sensible to come back to.
   */
  constexpr DWORD style = WS_OVERLAPPEDWINDOW;

  // AdjustWindowRect so the requested size is the client area -- the part the
  // swapchain gets -- not the window including its chrome.
  RECT rect{0, 0, static_cast<LONG>(_desc.width), static_cast<LONG>(_desc.height)};
  AdjustWindowRect(&rect, style, FALSE);

  const int windowWidth = rect.right - rect.left;
  const int windowHeight = rect.bottom - rect.top;

  m_handle = CreateWindowExW(0, WINDOW_CLASS_NAME, ToWide(_desc.title).c_str(), style, CW_USEDEFAULT, CW_USEDEFAULT, windowWidth,
                             windowHeight, nullptr, nullptr, m_instance, this);
  if (m_handle == nullptr)
  {
    NEURON_LOG_ERROR("CreateWindowExW failed (%lu)", GetLastError());
    return false;
  }

  RECT client{};
  GetClientRect(m_handle, &client);
  m_width = static_cast<std::uint32_t>(client.right - client.left);
  m_height = static_cast<std::uint32_t>(client.bottom - client.top);

  ShowWindow(m_handle, SW_SHOW);
  if (_desc.borderlessFullscreen)
  {
    SetBorderlessFullscreen(true);
  }
  NEURON_LOG_INFO("window created: %ux%u client%s", m_width, m_height, m_borderlessFullscreen ? " (borderless fullscreen)" : "");
  return true;
}

void Window::SetBorderlessFullscreen(bool _fullscreen) noexcept
{
  if (m_handle == nullptr || _fullscreen == m_borderlessFullscreen)
  {
    return;
  }

  if (_fullscreen)
  {
    // Saved before anything changes, because this is the only moment the
    // windowed placement still exists to be read.
    m_windowedPlacement.length = sizeof(m_windowedPlacement);
    if (GetWindowPlacement(m_handle, &m_windowedPlacement) == 0)
    {
      NEURON_LOG_WARNING("could not read the window placement; staying windowed");
      return;
    }

    // The monitor the window is on *now*, not the one it started on: a window
    // dragged to the second screen goes fullscreen there, which is the only
    // behaviour that is not surprising.
    MONITORINFO monitor{};
    monitor.cbSize = sizeof(monitor);
    if (GetMonitorInfoW(MonitorFromWindow(m_handle, MONITOR_DEFAULTTONEAREST), &monitor) == 0)
    {
      NEURON_LOG_WARNING("could not read the monitor bounds; staying windowed");
      return;
    }

    SetWindowLongPtrW(m_handle, GWL_STYLE, static_cast<LONG_PTR>(WS_POPUP | WS_VISIBLE));
    SetWindowPos(m_handle, HWND_TOP, monitor.rcMonitor.left, monitor.rcMonitor.top, monitor.rcMonitor.right - monitor.rcMonitor.left,
                 monitor.rcMonitor.bottom - monitor.rcMonitor.top, SWP_FRAMECHANGED | SWP_NOOWNERZORDER);
  }
  else
  {
    SetWindowLongPtrW(m_handle, GWL_STYLE, static_cast<LONG_PTR>(WS_OVERLAPPEDWINDOW | WS_VISIBLE));
    SetWindowPlacement(m_handle, &m_windowedPlacement);
    // Placement moves and sizes it; this is what makes Windows recompute the
    // frame it was just given back.
    SetWindowPos(m_handle, nullptr, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
  }

  m_borderlessFullscreen = _fullscreen;
  // The swapchain is resized by whoever consumes `ConsumeResize`: the style
  // change above has already queued the `WM_SIZE` that sets it.
  NEURON_LOG_INFO("window mode: %s", _fullscreen ? "borderless fullscreen" : "windowed");
}

void Window::Destroy()
{
  if (m_handle != nullptr)
  {
    DestroyWindow(m_handle);
    m_handle = nullptr;
  }
  if (m_classRegistered)
  {
    UnregisterClassW(WINDOW_CLASS_NAME, m_instance);
    m_classRegistered = false;
  }
}

bool Window::PumpMessages()
{
  MSG message{};
  while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE) != 0)
  {
    if (message.message == WM_QUIT)
    {
      m_closeRequested = true;
    }
    TranslateMessage(&message);
    DispatchMessageW(&message);
  }
  return !m_closeRequested;
}

bool Window::ConsumeResize(std::uint32_t& _outWidth, std::uint32_t& _outHeight)
{
  if (!m_resized)
  {
    return false;
  }
  m_resized = false;
  _outWidth = m_width;
  _outHeight = m_height;
  return true;
}

void Window::UpdateMouseContact() noexcept
{
  const bool buttonDown = m_input.buttonDown[static_cast<std::uint32_t>(InputButton::Left)];
  const bool buttonPressed = m_input.buttonPressed[static_cast<std::uint32_t>(InputButton::Left)];

  PointerPhase phase = PointerPhase::None;
  if (!m_mouseContactDown && (buttonDown || buttonPressed))
  {
    // `buttonPressed` as well as `buttonDown`, which is the fast-click case: a
    // press and release inside one frame leaves the level already false, and
    // reporting nothing here would lose the click entirely.
    phase = PointerPhase::Down;
    m_mouseContactDown = true;
  }
  else if (m_mouseContactDown)
  {
    phase = buttonDown ? PointerPhase::Held : PointerPhase::Up;
    m_mouseContactDown = buttonDown;
  }

  m_input.contactCount = 0;
  if (phase == PointerPhase::None)
  {
    return;
  }
  m_input.contacts[0] = PointerContact{MOUSE_CONTACT_ID, m_input.cursorX, m_input.cursorY, phase};
  m_input.contactCount = 1;
}

InputFrame Window::ConsumeInput() noexcept
{
  m_input.viewportWidth = m_width;
  m_input.viewportHeight = m_height;
  m_input.windowFocused = GetForegroundWindow() == m_handle;

  // Last, so the contact is built from the button state this frame ended with.
  UpdateMouseContact();

  const InputFrame frame = m_input;

  // Everything that describes *this frame* resets; everything that describes a
  // held state does not. Getting that split wrong is how a key sticks down or a
  // wheel notch fires for ever.
  m_input.cursorDeltaX = 0;
  m_input.cursorDeltaY = 0;
  m_input.wheelSteps = 0.0f;
  for (bool& pressed : m_input.buttonPressed)
  {
    pressed = false;
  }
  for (bool& released : m_input.buttonReleased)
  {
    released = false;
  }
  for (bool& pressed : m_input.actionPressed)
  {
    pressed = false;
  }
  for (bool& pressed : m_input.editKeyPressed)
  {
    pressed = false;
  }
  m_input.characterCount = 0;

  return frame;
}

void Window::AppendCharacter(WPARAM _codeUnit) noexcept
{
  const auto unit = static_cast<std::uint32_t>(_codeUnit) & 0xFFFFu;

  /*
   * UTF-16 arrives one code unit per message, so an astral character is two of
   * them and has to be assembled before it is a codepoint. Pairing it here is
   * the only place the pairing is unambiguous -- downstream, two adjacent
   * halves are indistinguishable from one half followed by a new character.
   */
  if (unit >= 0xD800u && unit <= 0xDBFFu)
  {
    m_pendingHighSurrogate = unit; // A lead half; the character is not here yet.
    return;
  }

  char32_t codepoint = 0;
  if (unit >= 0xDC00u && unit <= 0xDFFFu)
  {
    if (m_pendingHighSurrogate == 0)
    {
      // A trail half with no lead. Nothing can be made of it, and passing it on
      // would put an unpaintable value in a name.
      return;
    }
    codepoint = static_cast<char32_t>(0x10000u + ((m_pendingHighSurrogate - 0xD800u) << 10) + (unit - 0xDC00u));
    m_pendingHighSurrogate = 0;
  }
  else
  {
    // A lead half followed by anything other than a trail half was never a
    // character; dropping it is what stops it joining the next one.
    m_pendingHighSurrogate = 0;
    codepoint = static_cast<char32_t>(unit);
  }

  // Control characters come through this door too -- `\b`, `\r` and `\t` among
  // them -- and belong to the editing keys rather than to any buffer
  // (ADR-020 §3). The same filter the edit state applies, applied before the
  // frame carries them, so nothing unpaintable is ever in flight.
  if (!IsStorableCodepoint(codepoint))
  {
    return;
  }

  if (m_input.characterCount >= MAX_FRAME_CHARACTERS)
  {
    return; // Keep the earliest: they are the ones the player typed first.
  }
  m_input.characters[m_input.characterCount] = codepoint;
  ++m_input.characterCount;
}

void Window::SetButton(InputButton _button, bool _down) noexcept
{
  const auto index = static_cast<std::uint32_t>(_button);
  if (_down && !m_input.buttonDown[index])
  {
    m_input.buttonPressed[index] = true;
  }
  if (!_down && m_input.buttonDown[index])
  {
    m_input.buttonReleased[index] = true;
  }
  m_input.buttonDown[index] = _down;

  // Capture is counted rather than set, so releasing one button mid-drag of
  // another does not drop the drag that is still in progress.
  if (_down)
  {
    if (m_captureCount++ == 0)
    {
      SetCapture(m_handle);
    }
  }
  else if (m_captureCount > 0 && --m_captureCount == 0)
  {
    ReleaseCapture();
  }
}

void Window::SetKey(WPARAM _virtualKey, bool _down) noexcept
{
  /*
   * The field's keys first, in their own channel (`TextEditKey`).
   *
   * Their own switch and not a third entry in the array below, because they are
   * a different question -- see `InputMap.h`. `Home` is the one key in both
   * tables: it is `ResetView` to a camera and "start of line" to a field, and
   * falling through rather than returning is what lets each read its own answer
   * where the routing already decides which of them is listening.
   */
  {
    TextEditKey key = TextEditKey::Backspace;
    bool isEditKey = true;
    switch (_virtualKey)
    {
    case VK_BACK:
      key = TextEditKey::Backspace;
      break;
    case VK_DELETE:
      key = TextEditKey::Delete;
      break;
    case VK_LEFT:
      key = TextEditKey::Left;
      break;
    case VK_RIGHT:
      key = TextEditKey::Right;
      break;
    case VK_HOME:
      key = TextEditKey::Home;
      break;
    case VK_END:
      key = TextEditKey::End;
      break;
    default:
      isEditKey = false;
      break;
    }

    // A level rather than an edge would be a caret that runs while a key is
    // held, at the frame rate rather than at the platform's repeat rate -- so
    // this takes every `_down`, auto-repeat included, and the repeat rate is
    // the one the player configured.
    if (isEditKey && _down)
    {
      m_input.editKeyPressed[static_cast<std::uint32_t>(key)] = true;
    }
  }

  // The whole virtual-key table, in the one file entitled to know about
  // virtual keys. Left and right buttons are deliberately not here: the left
  // one is S8's click and box-select and the right one is S9's order puck, so
  // the camera takes the middle button, the wheel, the edges and these keys
  // (InputMap.h).
  //
  // An array rather than a single action, because one key may carry more than
  // one meaning: Shift is both "adjust the selection" and "append to the
  // queue", which qualify different drags and so never collide.
  InputAction actions[2] = {};
  std::uint32_t count = 0;

  switch (_virtualKey)
  {
  case 'W':
  case VK_UP:
    actions[count++] = InputAction::PanForward;
    break;
  case 'S':
  case VK_DOWN:
    actions[count++] = InputAction::PanBack;
    break;
  case 'A':
  case VK_LEFT:
    actions[count++] = InputAction::PanLeft;
    break;
  case 'D':
  case VK_RIGHT:
    actions[count++] = InputAction::PanRight;
    break;
  case 'Q':
    actions[count++] = InputAction::YawLeft;
    break;
  case 'E':
    actions[count++] = InputAction::YawRight;
    break;
  case VK_PRIOR:
  case VK_OEM_PLUS:
  case VK_ADD:
    actions[count++] = InputAction::ZoomIn;
    break;
  case VK_NEXT:
  case VK_OEM_MINUS:
  case VK_SUBTRACT:
    actions[count++] = InputAction::ZoomOut;
    break;
  case VK_HOME:
    actions[count++] = InputAction::ResetView;
    break;
  case VK_MENU:
    actions[count++] = InputAction::Modifier;
    break;
  case VK_SHIFT:
    actions[count++] = InputAction::SelectAdd;
    actions[count++] = InputAction::QueueOrder;
    break;
  case 'F':
    actions[count++] = InputAction::CycleParameter;
    break;
  case VK_F1:
    actions[count++] = InputAction::ToggleDiagnostics;
    break;
  case VK_F10:
    actions[count++] = InputAction::ToggleFeedFreeze;
    break;
  case VK_ESCAPE:
    // Where this goes is the router's decision and not this file's (ADR-020
    // §2): a focused field cancels its edit with it, and only if none does
    // it reach the surface as "back".
    actions[count++] = InputAction::Back;
    break;
  case VK_TAB:
    // The fleet stepper (U6). A key and nothing else: every fleet it visits is
    // already a row or a block on the roster, so this is a shortcut over the
    // screen rather than a second way in.
    actions[count++] = InputAction::CycleFleet;
    break;
  case 'P':
    actions[count++] = InputAction::PinCamera;
    break;
  case VK_RETURN:
    // Its twin, and it goes nowhere but a field. `WM_CHAR` also delivers `\r`
    // for this key; the buffer refuses it as a control character, so binding
    // the edge here is what gives the key its one meaning rather than two.
    actions[count++] = InputAction::Confirm;
    break;
  default:
    return; // Not a camera, selection, order or navigation key.
  }

  for (std::uint32_t slot = 0; slot < count; ++slot)
  {
    const auto index = static_cast<std::uint32_t>(actions[slot]);
    if (_down && !m_input.actionDown[index])
    {
      m_input.actionPressed[index] = true; // Auto-repeat must not re-fire an edge.
    }
    m_input.actionDown[index] = _down;
  }
}

LRESULT CALLBACK Window::WindowProc(HWND _window, UINT _message, WPARAM _wParam, LPARAM _lParam) noexcept
{
  // WM_NCCREATE is the first message a window receives, so this is where the
  // instance pointer is attached; everything before it goes to DefWindowProc.
  if (_message == WM_NCCREATE)
  {
    const auto* create = reinterpret_cast<const CREATESTRUCTW*>(_lParam);
    SetWindowLongPtrW(_window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
  }

  auto* self = reinterpret_cast<Window*>(GetWindowLongPtrW(_window, GWLP_USERDATA));
  if (self != nullptr)
  {
    return self->HandleMessage(_window, _message, _wParam, _lParam);
  }
  return DefWindowProcW(_window, _message, _wParam, _lParam);
}

LRESULT Window::HandleMessage(HWND _window, UINT _message, WPARAM _wParam, LPARAM _lParam) noexcept
{
  switch (_message)
  {
  case WM_CLOSE:
    m_closeRequested = true;
    return 0;

  case WM_DESTROY:
    m_handle = nullptr;
    m_closeRequested = true;
    PostQuitMessage(0);
    return 0;

  case WM_SIZE:
  {
    const auto width = static_cast<std::uint32_t>(LOWORD(_lParam));
    const auto height = static_cast<std::uint32_t>(HIWORD(_lParam));
    m_minimised = _wParam == SIZE_MINIMIZED || width == 0 || height == 0;
    if (!m_minimised && (width != m_width || height != m_height))
    {
      m_width = width;
      m_height = height;
      m_resized = true; // Consumed by the frame loop, which owns the swapchain.
    }
    return 0;
  }

  case WM_GETMINMAXINFO:
  {
    // A zero-sized client area is a swapchain error, so refuse to go there.
    auto* info = reinterpret_cast<MINMAXINFO*>(_lParam);
    info->ptMinTrackSize.x = 320;
    info->ptMinTrackSize.y = 240;
    return 0;
  }

  case WM_MOUSEMOVE:
  {
    // GET_X_LPARAM, not LOWORD: with the mouse captured a drag reports
    // negative client coordinates, and LOWORD would read those as 65000.
    const auto x = static_cast<std::int32_t>(static_cast<std::int16_t>(LOWORD(_lParam)));
    const auto y = static_cast<std::int32_t>(static_cast<std::int16_t>(HIWORD(_lParam)));
    if (m_haveCursor)
    {
      m_input.cursorDeltaX += x - m_lastCursorX;
      m_input.cursorDeltaY += y - m_lastCursorY;
    }
    m_lastCursorX = x;
    m_lastCursorY = y;
    m_haveCursor = true;
    m_input.cursorX = x;
    m_input.cursorY = y;
    m_input.cursorInsideWindow =
        x >= 0 && y >= 0 && x < static_cast<std::int32_t>(m_width) && y < static_cast<std::int32_t>(m_height);

    if (!m_input.cursorInsideWindow)
    {
      break;
    }
    // Asking for WM_MOUSELEAVE every move is cheap and is the only way to be
    // told the cursor left: the window gets no move message once it has.
    TRACKMOUSEEVENT track{};
    track.cbSize = sizeof(track);
    track.dwFlags = TME_LEAVE;
    track.hwndTrack = _window;
    TrackMouseEvent(&track);
    return 0;
  }

  case WM_MOUSELEAVE:
    m_input.cursorInsideWindow = false;
    return 0;

  case WM_LBUTTONDOWN:
    SetButton(InputButton::Left, true);
    return 0;
  case WM_LBUTTONUP:
    SetButton(InputButton::Left, false);
    return 0;
  case WM_RBUTTONDOWN:
    SetButton(InputButton::Right, true);
    return 0;
  case WM_RBUTTONUP:
    SetButton(InputButton::Right, false);
    return 0;
  case WM_MBUTTONDOWN:
    SetButton(InputButton::Middle, true);
    return 0;
  case WM_MBUTTONUP:
    SetButton(InputButton::Middle, false);
    return 0;

  case WM_MOUSEWHEEL:
    // Accumulated as a fraction of a notch. A high-resolution wheel sends
    // multiples smaller than WHEEL_DELTA, and rounding them here would make a
    // smooth wheel feel like a ratchet.
    m_input.wheelSteps += static_cast<float>(GET_WHEEL_DELTA_WPARAM(_wParam)) / static_cast<float>(WHEEL_DELTA);
    return 0;

  case WM_CHAR:
    // What the player *typed*, which is the layout's answer rather than the
    // key's -- so it arrives here and not through `SetKey`, and a dead key or a
    // composition path produces one with no key edge of its own. No `WM_IME_*`
    // message is handled and the window makes no IME arrangement: ADR-020 §3
    // defers that explicitly, and anything reaching this door by such a path
    // meets the same filter as everything else.
    AppendCharacter(_wParam);
    return 0;

  case WM_KILLFOCUS:
    // Every held state is a lie the moment focus goes, and a key released over
    // another window never sends its WM_KEYUP here. The cursor reference goes
    // too, so coming back at a different point is not a drag of the difference.
    m_input = InputFrame{};
    m_haveCursor = false;
    // A lead surrogate waiting for its trail half is held state like any other:
    // the trail half is going to another window now, and joining this one to
    // whatever is typed on return would invent a character nobody pressed.
    m_pendingHighSurrogate = 0;
    if (m_captureCount != 0)
    {
      m_captureCount = 0;
      ReleaseCapture();
    }
    return 0;

  case WM_SYSKEYDOWN:
    // Alt+Enter belongs to the client, not to DXGI's own fullscreen handling,
    // which the flip-model swapchain disables anyway.
    if (_wParam == VK_RETURN)
    {
      // Bit 30 is the previous key state. Without it a held Alt+Enter toggles
      // once per auto-repeat, which reads as the window flickering rather than
      // as a mode change.
      if ((_lParam & (LPARAM{1} << 30)) == 0)
      {
        SetBorderlessFullscreen(!m_borderlessFullscreen);
      }
      return 0;
    }
    SetKey(_wParam, true); // Alt arrives as a system key, not a plain one.
    // F10 is a system key like Alt, and DefWindowProc answers it by activating
    // the window menu -- which on a window with no menu still takes the
    // keyboard until something dismisses it. Handled, so it is not.
    if (_wParam == VK_F10)
    {
      return 0;
    }
    break;

  case WM_SYSKEYUP:
    SetKey(_wParam, false);
    if (_wParam == VK_F10)
    {
      return 0;
    }
    break;

  case WM_KEYDOWN:
    if (_wParam == VK_ESCAPE)
    {
      m_closeRequested = true;
      return 0;
    }
    SetKey(_wParam, true);
    return 0;

  case WM_KEYUP:
    SetKey(_wParam, false);
    return 0;

  default:
    break;
  }
  return DefWindowProcW(_window, _message, _wParam, _lParam);
}

} // namespace Neuron
