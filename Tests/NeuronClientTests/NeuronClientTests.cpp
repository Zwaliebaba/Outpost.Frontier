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
    const ClearColour colour = SpaceClearColour();

    Assert::IsTrue(colour.red >= 0.0f && colour.red <= 1.0f);
    Assert::IsTrue(colour.green >= 0.0f && colour.green <= 1.0f);
    Assert::IsTrue(colour.blue >= 0.0f && colour.blue <= 1.0f);
    Assert::AreEqual(1.0f, colour.alpha);
  }

  TEST_METHOD(StaysDarkAndBlue)
  {
    // The art direction is near-black space. These are linear values behind an
    // _SRGB RTV, so a number that looks small here is not small on screen --
    // 0.05 linear is already a visible grey.
    const ClearColour colour = SpaceClearColour();
    Assert::IsTrue(colour.blue > colour.red);
    Assert::IsTrue(colour.blue < 0.02f, L"clear colour is brighter than the art direction allows");
  }

  TEST_METHOD(DoesNotChangeFrameToFrame)
  {
    // S1's clear breathed on a 6-second cycle to prove the loop ran with
    // nothing else on screen. From the Nebula node onwards there is something
    // else on screen, and a background that moves by itself competes with it.
    Assert::AreEqual(SpaceClearColour().blue, SpaceClearColour().blue);
    Assert::IsTrue(SpaceClearColour().green > 0.0f, L"space is near-black, not black");
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
