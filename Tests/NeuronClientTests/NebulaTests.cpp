#include "pch.h"
#include "CppUnitTest.h"

#include "IsoCamera.h"
#include "NebulaField.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace Neuron;
using namespace DirectX;

/*
 * The Nebula node, tested without a GPU.
 *
 * This is the payoff for baking the field on the CPU instead of evaluating it
 * in a shader (NebulaField.h): everything that decides what the haze looks like
 * -- the noise, the shaping, the periodicity -- is ordinary C++ and can be
 * asserted here. What is left on the GPU side is an upload and one additive
 * draw, which is the smallest untestable surface the split allows.
 *
 * The plane-mapping suite is the other half. It is the same shape as
 * `EastIsOnTheRightOfTheScreen`, and for the same reason: ADR-006 §3a's
 * handedness defect was invisible to review and visible only to a round trip
 * through the real matrices, so a mapping that claims to invert the
 * view-projection is asserted against the view-projection rather than derived
 * twice and eyeballed.
 */

namespace NeuronClientTests
{
namespace
{

/// Projects a plane point through the real view-projection to NDC. Row-major
/// storage and the row-vector convention, matching IsoCamera and the shaders.
void ProjectPlanePointToNdc(const IsoCamera& _camera, float _planeX, float _planeY, float& _outNdcX, float& _outNdcY)
{
  const XMFLOAT4X4 viewProjection = _camera.ViewProjectionMatrix();
  const float world[4] = {_planeX, 0.0f, _planeY, 1.0f};

  float clip[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  for (int column = 0; column < 4; ++column)
  {
    for (int row = 0; row < 4; ++row)
    {
      clip[column] += world[row] * viewProjection.m[row][column];
    }
  }

  _outNdcX = clip[0] / clip[3];
  _outNdcY = clip[1] / clip[3];
}

} // namespace

TEST_CLASS(NebulaFieldTests)
{
public:
  TEST_METHOD(TheDefaultSettingsBakeAUsableField)
  {
    NebulaField field;
    Assert::IsTrue(BakeNebulaField(NebulaSettings{}, field));
    Assert::IsTrue(field.Valid());
    Assert::AreEqual<std::uint32_t>(256, field.resolution);
    Assert::AreEqual<std::size_t>(256u * 256u, field.intensity.size());
  }

  TEST_METHOD(SpaceIsSparse)
  {
    // The art direction is "sparse space" (ADR-006 context). An even grey fog
    // over the whole tile would pass every other test in this file and be
    // completely wrong on screen, so the distribution is asserted directly.
    NebulaField field;
    Assert::IsTrue(BakeNebulaField(NebulaSettings{}, field));

    std::size_t nearBlack = 0;
    std::uint8_t brightest = 0;
    for (const std::uint8_t value : field.intensity)
    {
      if (value <= 8)
      {
        ++nearBlack;
      }
      brightest = std::max(brightest, value);
    }

    const double darkFraction = static_cast<double>(nearBlack) / static_cast<double>(field.intensity.size());
    Assert::IsTrue(darkFraction > 0.30, L"most of the tile should be empty space");
    Assert::IsTrue(darkFraction < 0.95, L"but not all of it, or there is no nebula");
    Assert::IsTrue(brightest > 64, L"the field should reach somewhere worth looking at");
  }

  TEST_METHOD(TheSameSettingsBakeTheSameBytes)
  {
    // A field that differed between runs would make every screenshot
    // comparison and every "is this the build I think it is" check useless.
    NebulaField first;
    NebulaField second;
    Assert::IsTrue(BakeNebulaField(NebulaSettings{}, first));
    Assert::IsTrue(BakeNebulaField(NebulaSettings{}, second));
    Assert::IsTrue(first.intensity == second.intensity);
  }

  TEST_METHOD(TheSeedIsTheOnlyThingThatNeedsToChange)
  {
    NebulaSettings settings;
    NebulaField original;
    Assert::IsTrue(BakeNebulaField(settings, original));

    settings.seed = 2;
    NebulaField reseeded;
    Assert::IsTrue(BakeNebulaField(settings, reseeded));
    Assert::IsTrue(reseeded.intensity != original.intensity, L"a different seed is a different field");
    Assert::AreEqual(original.resolution, reseeded.resolution);
  }

  TEST_METHOD(TheTileWrapsWithoutASeam)
  {
    // The pass samples with a wrapping sampler over an unbounded plane, so the
    // tile's edges meet on screen. If the field were not periodic that meeting
    // would be a hard line across the world -- the exact artefact a tile sized
    // to the play area would have produced instead.
    NebulaField field;
    Assert::IsTrue(BakeNebulaField(NebulaSettings{}, field));

    const auto size = static_cast<std::int32_t>(field.resolution);
    int seamStep = 0;
    int interiorStep = 0;
    for (std::int32_t i = 0; i < size; ++i)
    {
      seamStep = std::max(seamStep, std::abs(field.At(0, i) - field.At(size - 1, i)));
      seamStep = std::max(seamStep, std::abs(field.At(i, 0) - field.At(i, size - 1)));
      for (std::int32_t x = 1; x < size; ++x)
      {
        interiorStep = std::max(interiorStep, std::abs(field.At(x, i) - field.At(x - 1, i)));
      }
    }

    Assert::IsTrue(seamStep <= interiorStep, L"the seam is no sharper than anywhere else in the tile");
    Assert::IsTrue(interiorStep < 24, L"no texel-to-texel cliff for the bilinear sampler to smear");
  }

  TEST_METHOD(TheContinuousFieldHasPeriodOne)
  {
    // The baked texture only wraps because the function behind it does.
    const NebulaSettings settings;
    for (int i = 0; i < 32; ++i)
    {
      const float u = static_cast<float>(i) * 0.031f;
      const float v = 1.0f - static_cast<float>(i) * 0.017f;
      Assert::AreEqual(SampleNebulaField(settings, u, v), SampleNebulaField(settings, u + 1.0f, v - 2.0f), 1e-5f);
    }
  }

  TEST_METHOD(CoverageMeansWhatItSays)
  {
    const auto meanOf = [](const NebulaField& _field) {
      long long sum = 0;
      for (const std::uint8_t value : _field.intensity)
      {
        sum += value;
      }
      return static_cast<double>(sum) / static_cast<double>(_field.intensity.size());
    };

    NebulaSettings settings;
    NebulaField normal;
    Assert::IsTrue(BakeNebulaField(settings, normal));

    settings.coverage = 0.80f;
    NebulaField emptier;
    Assert::IsTrue(BakeNebulaField(settings, emptier));

    Assert::IsTrue(meanOf(emptier) < meanOf(normal), L"raising coverage empties the field");
  }

  TEST_METHOD(SettingsThatDescribeNoFieldAreRefused)
  {
    // A bad `nebula` block costs the frame its haze, so these have to fail
    // rather than produce something -- the pass checks Ready() and skips.
    NebulaField field;

    NebulaSettings noTexels;
    noTexels.resolution = 0;
    Assert::IsFalse(BakeNebulaField(noTexels, field));

    NebulaSettings noOctaves;
    noOctaves.octaves = 0;
    Assert::IsFalse(BakeNebulaField(noOctaves, field));

    NebulaSettings noTile;
    noTile.tileMetres = 0.0f;
    Assert::IsFalse(BakeNebulaField(noTile, field));

    NebulaSettings absurd;
    absurd.resolution = 99999;
    Assert::IsFalse(BakeNebulaField(absurd, field));

    Assert::IsFalse(field.Valid(), L"a refused bake leaves nothing behind to draw");
  }

  TEST_METHOD(AWrappingFetchReachesOutsideTheTile)
  {
    NebulaField field;
    Assert::IsTrue(BakeNebulaField(NebulaSettings{}, field));

    const auto size = static_cast<std::int32_t>(field.resolution);
    Assert::AreEqual(field.At(0, 0), field.At(size, size));
    Assert::AreEqual(field.At(3, 7), field.At(3 - size, 7 + 2 * size));
    Assert::AreEqual(field.At(size - 1, 0), field.At(-1, 0));
  }
};

TEST_CLASS(PlaneMappingTests)
{
public:
  TEST_METHOD(ItInvertsTheViewProjectionOnThePlane)
  {
    // The nebula anchors its field with this mapping, so if it is wrong the
    // haze slides against the world when the camera moves -- a thing that looks
    // like a rendering bug and is arithmetic.
    const float yaws[] = {0.0f, 0.3f, XM_PIDIV4, XM_PIDIV2, 2.4f, 3.0f, -1.1f, -2.2f};
    const float zooms[] = {IsoCamera::MIN_ZOOM_METRES, 2000.0f, 8000.0f, IsoCamera::MAX_ZOOM_METRES};
    const XMFLOAT2 focuses[] = {{0.0f, 0.0f}, {1234.0f, -5678.0f}, {-19000.0f, 19000.0f}};

    float worstError = 0.0f;
    for (const float yaw : yaws)
    {
      for (const float zoom : zooms)
      {
        for (const XMFLOAT2& focus : focuses)
        {
          IsoCamera camera;
          camera.SetViewport(1600, 900);
          camera.SetYawRadians(yaw);
          camera.SetZoomMetres(zoom);
          camera.SetFocus(focus);

          const PlaneMapping mapping = camera.PlaneMappingForNdc();
          for (int i = -2; i <= 2; ++i)
          {
            for (int j = -2; j <= 2; ++j)
            {
              const float ndcX = static_cast<float>(i) * 0.5f;
              const float ndcY = static_cast<float>(j) * 0.5f;

              const float planeX = mapping.origin.x + mapping.rightPerNdc.x * ndcX + mapping.upPerNdc.x * ndcY;
              const float planeY = mapping.origin.y + mapping.rightPerNdc.y * ndcX + mapping.upPerNdc.y * ndcY;

              float roundTripX = 0.0f;
              float roundTripY = 0.0f;
              ProjectPlanePointToNdc(camera, planeX, planeY, roundTripX, roundTripY);

              worstError = std::max(worstError, std::fabs(roundTripX - ndcX));
              worstError = std::max(worstError, std::fabs(roundTripY - ndcY));
            }
          }
        }
      }
    }

    Assert::IsTrue(worstError < 1e-4f, L"the mapping is not the view-projection's inverse on the plane");
  }

  TEST_METHOD(TheCentreOfTheViewIsTheFocus)
  {
    IsoCamera camera;
    camera.SetViewport(1600, 900);
    camera.SetYawRadians(1.0f);
    camera.SetFocus(XMFLOAT2{700.0f, -300.0f});

    const PlaneMapping mapping = camera.PlaneMappingForNdc();
    Assert::AreEqual(700.0f, mapping.origin.x, 1e-3f);
    Assert::AreEqual(-300.0f, mapping.origin.y, 1e-3f);
  }

  TEST_METHOD(ScreenUpCoversMorePlaneThanScreenRight)
  {
    // 1/sin(30 degrees) = 2 against a 16:9 aspect, which is the same
    // foreshortening that makes a ground circle a 2:1 ellipse (ADR-006 §4).
    IsoCamera camera;
    camera.SetViewport(1600, 900);
    camera.SetYawRadians(0.0f);
    camera.SetZoomMetres(8000.0f);

    const PlaneMapping mapping = camera.PlaneMappingForNdc();
    Assert::IsTrue(mapping.rightPerNdc.x > 0.0f, L"at yaw 0, NDC +x is east");
    Assert::IsTrue(mapping.upPerNdc.y > 0.0f, L"at yaw 0, NDC +y is north");
    Assert::AreEqual(8000.0f * 16.0f / 9.0f, mapping.rightPerNdc.x, 1.0f);
    Assert::AreEqual(16000.0f, mapping.upPerNdc.y, 1.0f);
  }

  TEST_METHOD(TheFieldMovesWithTheWorldWhenTheCameraPans)
  {
    // The property the whole world-anchored design exists for: pan, and the
    // plane point under a given pixel changes by exactly the pan. Sampling in
    // screen space would leave it fixed -- a smudge on the monitor.
    IsoCamera camera;
    camera.SetViewport(1600, 900);
    camera.SetYawRadians(0.0f);
    camera.SetZoomMetres(8000.0f);
    camera.SetFocus(XMFLOAT2{0.0f, 0.0f});

    const PlaneMapping before = camera.PlaneMappingForNdc();
    camera.SetFocus(XMFLOAT2{1500.0f, -2500.0f});
    const PlaneMapping after = camera.PlaneMappingForNdc();

    Assert::AreEqual(1500.0f, after.origin.x - before.origin.x, 1e-3f);
    Assert::AreEqual(-2500.0f, after.origin.y - before.origin.y, 1e-3f);
    Assert::AreEqual(before.rightPerNdc.x, after.rightPerNdc.x, 1e-3f, L"panning does not rescale the field");
    Assert::AreEqual(before.upPerNdc.y, after.upPerNdc.y, 1e-3f);
  }
};

} // namespace NeuronClientTests
