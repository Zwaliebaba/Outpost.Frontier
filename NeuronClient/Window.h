#pragma once

#include <cstdint>
#include <string>

/*
 * The Win32 window (fixed constraint: raw Win32, no frameworks).
 *
 * Message pumping and rendering share this thread (ADR-007 §1). A modal
 * drag or resize therefore stalls presentation while the server keeps
 * ticking -- accepted MVP behaviour, and it exercises the client's
 * catch-up path for free.
 */

namespace Neuron
{

struct WindowDesc
{
  std::uint32_t width = 1600;
  std::uint32_t height = 900;
  std::string title = "Outpost: Frontier";
  bool borderlessFullscreen = false;
};

class Window
{
public:
  Window() = default;
  ~Window();

  Window(const Window&) = delete;
  Window& operator=(const Window&) = delete;

  [[nodiscard]] bool Create(const WindowDesc& _desc);
  void Destroy();

  /// Drains the queue. Returns false once the window wants to close.
  [[nodiscard]] bool PumpMessages();

  /// True once, after the client area changed size. The caller resizes its
  /// swapchain; a minimised window reports 0x0 and is not a resize.
  [[nodiscard]] bool ConsumeResize(std::uint32_t& _outWidth, std::uint32_t& _outHeight);

  [[nodiscard]] HWND Handle() const noexcept { return m_handle; }
  [[nodiscard]] std::uint32_t Width() const noexcept { return m_width; }
  [[nodiscard]] std::uint32_t Height() const noexcept { return m_height; }
  [[nodiscard]] bool Minimised() const noexcept { return m_minimised; }
  [[nodiscard]] bool CloseRequested() const noexcept { return m_closeRequested; }

private:
  static LRESULT CALLBACK WindowProc(HWND _window, UINT _message, WPARAM _wParam, LPARAM _lParam) noexcept;
  LRESULT HandleMessage(HWND _window, UINT _message, WPARAM _wParam, LPARAM _lParam) noexcept;

  HWND m_handle = nullptr;
  HINSTANCE m_instance = nullptr;
  std::uint32_t m_width = 0;
  std::uint32_t m_height = 0;
  bool m_closeRequested = false;
  bool m_resized = false;
  bool m_minimised = false;
  bool m_classRegistered = false;
};

} // namespace Neuron
