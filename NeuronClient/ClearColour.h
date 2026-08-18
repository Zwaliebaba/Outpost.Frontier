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

/*
 * Near-black, faintly blue, and the same every frame.
 *
 * This replaced `AnimatedClearColour`, which breathed between two blues so that
 * slice S1 could prove the loop was running when nothing else was on screen.
 * There is a fleet and a nebula on screen now, and a background that changes on
 * its own is a background competing with them.
 *
 * The values are linear -- the RTV is `_SRGB`, so the hardware encodes them --
 * and they are deliberately not configurable. What the eye reads as the colour
 * of space here is the nebula, and that *is* configurable; this is the absence
 * of light behind it, which is not an art decision anyone needs to retune.
 */
[[nodiscard]] ClearColour SpaceClearColour() noexcept;

} // namespace Neuron
