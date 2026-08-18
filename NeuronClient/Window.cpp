#include "pch.h"

#include "Window.h"

#include "Log.h"

namespace Neuron
{
namespace
{

constexpr const wchar_t* WindowClassName = L"OutpostFrontierWindow";

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
  windowClass.lpszClassName = WindowClassName;
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

  m_handle = CreateWindowExW(0, WindowClassName, ToWide(_desc.title).c_str(), style, CW_USEDEFAULT, CW_USEDEFAULT, windowWidth,
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
    UnregisterClassW(WindowClassName, m_instance);
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

    case WM_SYSKEYDOWN:
      // Alt+Enter belongs to the client, not to DXGI's own fullscreen handling,
      // which the flip-model swapchain disables anyway.
      if (_wParam == VK_RETURN)
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
      break;

    default:
      break;
  }
  return DefWindowProcW(_window, _message, _wParam, _lParam);
}

} // namespace Neuron
