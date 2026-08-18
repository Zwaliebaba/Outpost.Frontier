#include "pch.h"

#include "Window.h"

#include "Log.h"

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

  const DWORD style = _desc.borderlessFullscreen ? WS_POPUP : WS_OVERLAPPEDWINDOW;

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
  NEURON_LOG_INFO("window created: %ux%u client", m_width, m_height);
  return true;
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

InputFrame Window::ConsumeInput() noexcept
{
  m_input.viewportWidth = m_width;
  m_input.viewportHeight = m_height;
  m_input.windowFocused = GetForegroundWindow() == m_handle;

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

  return frame;
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
  default:
    return; // Not a camera, selection or order key.
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

  case WM_KILLFOCUS:
    // Every held state is a lie the moment focus goes, and a key released over
    // another window never sends its WM_KEYUP here. The cursor reference goes
    // too, so coming back at a different point is not a drag of the difference.
    m_input = InputFrame{};
    m_haveCursor = false;
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
      return 0;
    }
    SetKey(_wParam, true); // Alt arrives as a system key, not a plain one.
    break;

  case WM_SYSKEYUP:
    SetKey(_wParam, false);
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
