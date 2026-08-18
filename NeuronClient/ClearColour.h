#pragma once

/*
 * The frame's clear colour.
 *
 * Deliberately free of Windows and D3D12 headers: it is presentation maths, so
 * it should be testable without a device, a window, or the C++/WinRT projection
 * the graphics headers pull in.
 */

namespace Neuron
{

struct ClearColour
{
  float red = 0.0f;
  float green = 0.0f;
  float blue = 0.0f;
  float alpha = 1.0f;
};

/// Slow breathing between two near-black blues -- proof the loop is running
/// without pretending to be art (the real look arrives with ADR-006's passes).
[[nodiscard]] ClearColour AnimatedClearColour(double _seconds) noexcept;

} // namespace Neuron
