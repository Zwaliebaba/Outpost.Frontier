#pragma once

#include "InputMap.h"

#include <windows.h>

#include <cstdint>
#include <string>

/*
 * The Win32 window (fixed constraint: raw Win32, no frameworks).
 *
 * Message pumping and rendering share this thread (ADR-007 §1). A modal
 * drag or resize therefore stalls presentation while the server keeps
 * ticking -- accepted MVP behaviour, and it exercises the client's
 * catch-up path for free.
 *
 * It is also where input becomes logical: the virtual-key table lives here,
 * because this is the only file that should ever have to know what VK_OEM_PLUS
 * is. Everything downstream reads an InputFrame (InputMap.h) and speaks in
 * actions.
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

  /// This frame's input, and a reset of everything that is per-frame: the
  /// edges, the cursor delta and the wheel. Levels (buttons and keys held)
  /// survive, because they are state rather than events.
  [[nodiscard]] InputFrame ConsumeInput() noexcept;

  /*
   * Borderless fullscreen, at runtime (S1's open item).
   *
   * The mode used to be read once at `Create` and never again, while Alt+Enter
   * was swallowed and did nothing -- so the one gesture every player tries had
   * a handler whose whole job was to stop DXGI reacting to it. It toggles now,
   * and `Create` routes its own `borderlessFullscreen` through this same
   * function so the two paths cannot drift into two different fullscreens.
   *
   * Borderless means the monitor the window is *on*, found at the moment of the
   * toggle: a window dragged to the second screen goes fullscreen there. Coming
   * back restores the saved placement rather than a remembered width and
   * height, so a maximised window returns maximised.
   *
   * The swapchain is not touched here. The style change makes Windows send
   * `WM_SIZE`, which is the resize path the frame loop already owns -- one
   * place that resizes back buffers, not two.
   */
  void SetBorderlessFullscreen(bool _fullscreen) noexcept;
  [[nodiscard]] bool BorderlessFullscreen() const noexcept { return m_borderlessFullscreen; }

  [[nodiscard]] HWND Handle() const noexcept { return m_handle; }
  [[nodiscard]] std::uint32_t Width() const noexcept { return m_width; }
  [[nodiscard]] std::uint32_t Height() const noexcept { return m_height; }
  [[nodiscard]] bool Minimised() const noexcept { return m_minimised; }
  [[nodiscard]] bool CloseRequested() const noexcept { return m_closeRequested; }

private:
  static LRESULT CALLBACK WindowProc(HWND _window, UINT _message, WPARAM _wParam, LPARAM _lParam) noexcept;
  LRESULT HandleMessage(HWND _window, UINT _message, WPARAM _wParam, LPARAM _lParam) noexcept;

  void SetButton(InputButton _button, bool _down) noexcept;
  void SetKey(WPARAM _virtualKey, bool _down) noexcept;

  HWND m_handle = nullptr;
  HINSTANCE m_instance = nullptr;
  std::uint32_t m_width = 0;
  std::uint32_t m_height = 0;

  InputFrame m_input;
  std::int32_t m_lastCursorX = 0;
  std::int32_t m_lastCursorY = 0;
  bool m_haveCursor = false;   // False until the first move, so the first frame
                               // reports no delta rather than a jump from 0,0.
  std::uint32_t m_captureCount = 0;

  bool m_closeRequested = false;
  bool m_resized = false;
  bool m_borderlessFullscreen = false;

  /// Where the window was before it went fullscreen. A placement rather than a
  /// rect because it carries the maximised/restored state as well as the box,
  /// and losing that is how a maximised window comes back merely large.
  WINDOWPLACEMENT m_windowedPlacement{};
  bool m_minimised = false;
  bool m_classRegistered = false;
};

} // namespace Neuron
