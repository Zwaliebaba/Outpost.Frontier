#include "pch.h"

#include "ClearColour.h"

namespace Neuron
{

ClearColour SpaceClearColour() noexcept
{
  ClearColour colour;
  colour.red = 0.002f;
  colour.green = 0.004f;
  colour.blue = 0.006f;
  colour.alpha = 1.0f;
  return colour;
}

} // namespace Neuron
