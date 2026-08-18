#include "pch.h"
#include "CppUnitTest.h"

#include <cstdint>

#include "ClearColour.h"
#include "ClientConfig.h"

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace Neuron;

/*
 * No device and no window here: a unit test cannot assume a GPU, and CI has
 * none worth trusting. What is testable without one is the maths and the
 * defaults; the device path is a manual checkpoint (Build Order S1).
 */

namespace NeuronClientTests
{

TEST_CLASS(ClearColourTests)
{
public:
  TEST_METHOD(StaysInsideTheDisplayableRange)
  {
    // A clear colour outside [0,1] is not an artistic choice, it is a bug that
    // shows up as a flash on some drivers and not others.
    for (int i = 0; i <= 240; ++i)
    {
      const double seconds = i * 0.05;
      const ClearColour colour = AnimatedClearColour(seconds);

      Assert::IsTrue(colour.red >= 0.0f && colour.red <= 1.0f);
      Assert::IsTrue(colour.green >= 0.0f && colour.green <= 1.0f);
      Assert::IsTrue(colour.blue >= 0.0f && colour.blue <= 1.0f);
      Assert::AreEqual(1.0f, colour.alpha);
    }
  }

  TEST_METHOD(StaysDarkAndBlue)
  {
    // The art direction is near-black space; a clear colour that drifts bright
    // or green would be visible proof the loop is lying about the palette.
    for (int i = 0; i <= 60; ++i)
    {
      const ClearColour colour = AnimatedClearColour(i * 0.1);
      Assert::IsTrue(colour.blue > colour.red);
      Assert::IsTrue(colour.blue < 0.1f, L"clear colour is brighter than the art direction allows");
    }
  }

  TEST_METHOD(AnimatesAndRepeatsOnItsCycle)
  {
    const ClearColour start = AnimatedClearColour(0.0);
    const ClearColour quarter = AnimatedClearColour(1.5);
    const ClearColour full = AnimatedClearColour(6.0);

    Assert::AreNotEqual(start.blue, quarter.blue); // It moves...
    Assert::AreEqual(start.blue, full.blue, 1e-5f); // ...and comes back after 6 s.
  }
};

TEST_CLASS(ClientConfigTests)
{
public:
  TEST_METHOD(DefaultsAreUsableWithoutAConfigFile)
  {
    const ClientConfig config;

    Assert::IsTrue(config.windowWidth >= 640 && config.windowHeight >= 480);
    Assert::IsTrue(config.vsync);
    Assert::AreEqual<std::uint32_t>(0, config.frameCap);
    Assert::IsFalse(config.enableDebugLayer); // Off unless the host turns it on.
    Assert::IsTrue(config.serverHost == "127.0.0.1");
    Assert::AreEqual<std::uint16_t>(7777, config.serverPort);
  }
};

} // namespace NeuronClientTests
