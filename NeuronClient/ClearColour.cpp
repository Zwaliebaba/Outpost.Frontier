#include "pch.h"

#include "ClearColour.h"

#include <cmath>

namespace Neuron
{

ClearColour AnimatedClearColour(double _seconds) noexcept
{
  // A 6-second cycle, staying in the dark blues the art direction lives in.
  const auto phase = static_cast<float>(std::sin(_seconds * (2.0 * 3.14159265358979323846 / 6.0)));
  const float wave = 0.5f * (phase + 1.0f); // sin is [-1,1]; colours are not.

  ClearColour colour;
  colour.red = 0.010f + 0.010f * wave;
  colour.green = 0.020f + 0.020f * wave;
  colour.blue = 0.040f + 0.030f * wave;
  colour.alpha = 1.0f;
  return colour;
}

} // namespace Neuron
