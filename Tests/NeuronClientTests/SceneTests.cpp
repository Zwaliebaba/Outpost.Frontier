#include "pch.h"
#include "CppUnitTest.h"

#include "RenderWorld.h"

#include <DirectXMath.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace Neuron;
using namespace DirectX;

/*
 * What Extract hands the renderer (Build Order S5).
 *
 * The parked fleet is a placeholder S7 deletes, but the shape it produces is
 * not: the opaque pass draws one class at a time and reads a contiguous run of
 * instances per class, so "sorted by class, ranges partition the array" is a
 * contract the real Extract will have to keep too.
 */

namespace NeuronClientTests
{

namespace
{

/// Hull radii roughly matching the shipped meshes, in the classId order
/// Outpost.json declares. Written out rather than loaded so this file tests the
/// layout and not the loader.
const std::vector<float> CLASS_RADII = {3.8f, 10.8f, 15.2f, 24.6f, 36.7f, 18.8f, 118.0f, 45.1f, 252.9f};

/// The grid extent, repeated rather than reached for: this file tests the scene
/// layout, and that is the only camera fact it needs (ADR-001 §3).
constexpr float PLAY_AREA_HALF_EXTENT_METRES = 20000.0f;

} // namespace

TEST_CLASS(RenderSceneTests)
{
public:
  TEST_METHOD(InstanceRecordIsTheTwentyByteVertexStream)
  {
    // It is the per-instance vertex stream, so its size is the stride the input
    // layout declares and the shader reads. A field added here is a change in
    // three places, and this is the one that notices.
    Assert::AreEqual<std::size_t>(20, sizeof(InstanceRecord));
    Assert::AreEqual<std::size_t>(0, offsetof(InstanceRecord, posWorld));
    Assert::AreEqual<std::size_t>(12, offsetof(InstanceRecord, heading));
    Assert::AreEqual<std::size_t>(16, offsetof(InstanceRecord, teamColorId));
    Assert::AreEqual<std::size_t>(17, offsetof(InstanceRecord, selectionAndLodBias));
    Assert::AreEqual<std::size_t>(18, offsetof(InstanceRecord, classId));
  }

  TEST_METHOD(SortingByClassPartitionsTheInstanceArray)
  {
    RenderScene scene;
    for (std::uint16_t classId : {std::uint16_t{2}, std::uint16_t{0}, std::uint16_t{2}, std::uint16_t{1}, std::uint16_t{0}})
    {
      scene.instances.push_back(InstanceRecord{XMFLOAT3{0.0f, 0.0f, 0.0f}, 0.0f, 0, 0, classId});
    }
    scene.SortByClass(3);

    Assert::AreEqual<std::size_t>(3, scene.classRanges.size());
    Assert::AreEqual<std::uint32_t>(0, scene.classRanges[0].firstInstance);
    Assert::AreEqual<std::uint32_t>(2, scene.classRanges[0].instanceCount);
    Assert::AreEqual<std::uint32_t>(2, scene.classRanges[1].firstInstance);
    Assert::AreEqual<std::uint32_t>(1, scene.classRanges[1].instanceCount);
    Assert::AreEqual<std::uint32_t>(3, scene.classRanges[2].firstInstance);
    Assert::AreEqual<std::uint32_t>(2, scene.classRanges[2].instanceCount);

    for (std::uint32_t classId = 0; classId < scene.classRanges.size(); ++classId)
    {
      const InstanceRange& range = scene.classRanges[classId];
      for (std::uint32_t i = 0; i < range.instanceCount; ++i)
      {
        Assert::AreEqual<std::uint32_t>(classId, scene.instances[range.firstInstance + i].classId);
      }
    }
  }

  TEST_METHOD(AnInstanceOfAClassWithNoMeshIsSimplyNotDrawn)
  {
    RenderScene scene;
    scene.instances.push_back(InstanceRecord{XMFLOAT3{0.0f, 0.0f, 0.0f}, 0.0f, 0, 0, 99});
    scene.instances.push_back(InstanceRecord{XMFLOAT3{0.0f, 0.0f, 0.0f}, 0.0f, 0, 0, 1});
    scene.SortByClass(3);

    Assert::AreEqual<std::size_t>(3, scene.classRanges.size(), L"the range table is sized by the class count, not by the data");

    std::uint32_t drawn = 0;
    for (const InstanceRange& range : scene.classRanges)
    {
      drawn += range.instanceCount;
    }
    Assert::AreEqual<std::uint32_t>(1, drawn, L"an out-of-range class must be dropped, never indexed");
  }

  TEST_METHOD(ClearKeepsCapacitySoAFrameDoesNotAllocate)
  {
    RenderScene scene;
    scene.instances.resize(64);
    scene.SortByClass(4);
    const std::size_t capacity = scene.instances.capacity();

    scene.Clear();
    Assert::IsTrue(scene.instances.empty());
    Assert::AreEqual(capacity, scene.instances.capacity(), L"Clear runs every frame; releasing storage would allocate every frame");
  }
};

TEST_CLASS(ParkedFleetTests)
{
public:
  TEST_METHOD(TheFleetIsFortyShipsAndNoStation)
  {
    // From S5b the station is authored content and arrives through AddScenery,
    // so the invented half of the scene is ships only. 40 + the station is the
    // 41 the slice's frame-time budget is stated against.
    ParkedFleetDesc desc;
    RenderScene scene;
    BuildParkedFleet(desc, CLASS_RADII, scene);

    Assert::AreEqual<std::size_t>(40, scene.instances.size());
    Assert::AreEqual<std::size_t>(9, scene.classRanges.size());
    for (std::uint32_t classId = 0; classId < 8; ++classId)
    {
      Assert::AreEqual<std::uint32_t>(5, scene.classRanges[classId].instanceCount, L"one wing of five per ship class");
    }
    Assert::AreEqual<std::uint32_t>(0, scene.classRanges[8].instanceCount, L"the structure is not invented here any more");
  }

  TEST_METHOD(SceneryLandsWhereTheContentPutIt)
  {
    // The S5b acceptance in miniature: a placement read from the universe file
    // renders where the file says, and the plane coordinates map to render
    // space as ADR-001 §3 defines -- x east, y the cosmetic height, z north.
    const ScenePlacement placements[] = {
        ScenePlacement{0.0f, 0.0f, 0.0f, 8},
        ScenePlacement{-1200.5f, 640.25f, 1.5f, 8},
    };

    RenderScene scene;
    AddScenery(placements, scene);
    scene.SortByClass(9);

    Assert::AreEqual<std::size_t>(2, scene.instances.size());
    Assert::AreEqual<std::uint32_t>(2, scene.classRanges[8].instanceCount);

    const InstanceRecord& placed = scene.instances[1];
    Assert::AreEqual(-1200.5f, placed.posWorld.x, 1e-4f, L"local x is render x");
    Assert::AreEqual(0.0f, placed.posWorld.y, 1e-6f, L"authored scenery sits on the plane");
    Assert::AreEqual(640.25f, placed.posWorld.z, 1e-4f, L"local y is render z");
    Assert::AreEqual(1.5f, placed.heading, 1e-6f);
    Assert::AreEqual<std::uint16_t>(8, placed.classId);
  }

  TEST_METHOD(SceneryAndShipsShareOneScene)
  {
    // Both halves of the frame end up in the same sorted instance array, which
    // is what lets the opaque pass draw a class in one run.
    ParkedFleetDesc desc;
    RenderScene scene;
    BuildParkedFleet(desc, CLASS_RADII, scene);

    const ScenePlacement station{0.0f, 0.0f, 0.0f, 8};
    AddScenery(std::span<const ScenePlacement>{&station, 1}, scene);
    scene.SortByClass(9);

    Assert::AreEqual<std::size_t>(41, scene.instances.size(), L"40 ships plus the authored station");
    Assert::AreEqual<std::uint32_t>(1, scene.classRanges[8].instanceCount);
    for (std::size_t i = 1; i < scene.instances.size(); ++i)
    {
      Assert::IsTrue(scene.instances[i - 1].classId <= scene.instances[i].classId, L"the merged scene is still sorted by class");
    }
  }

  TEST_METHOD(EveryShipIsOnThePlaneInsideTheGridAndFacingTheStation)
  {
    ParkedFleetDesc desc;
    RenderScene scene;
    BuildParkedFleet(desc, CLASS_RADII, scene);

    for (const InstanceRecord& instance : scene.instances)
    {
      Assert::IsTrue(std::fabs(instance.posWorld.x) <= PLAY_AREA_HALF_EXTENT_METRES,
                     L"an instance outside the grid cannot be panned to");
      Assert::IsTrue(std::fabs(instance.posWorld.z) <= PLAY_AREA_HALF_EXTENT_METRES);
      Assert::AreEqual(0.0f, instance.posWorld.y, 1e-6f, L"there is no simulated altitude (ADR-001 §1)");

      // Heading is radians CCW from +x in sim space, so the facing direction is
      // (cos h, sin h) and a parked ship should be pointed back at the station.
      const float toStationX = -instance.posWorld.x;
      const float toStationZ = -instance.posWorld.z;
      const float length = std::sqrt(toStationX * toStationX + toStationZ * toStationZ);
      const float alignment = (toStationX * std::cos(instance.heading) + toStationZ * std::sin(instance.heading)) / length;
      Assert::IsTrue(alignment > 0.55f, L"a parked wing faces the station it is parked at");
    }
  }

  TEST_METHOD(WingSpacingFollowsTheHullBeingParked)
  {
    // A Carrier wing parked at Interceptor spacing would intersect itself, and
    // the spacing is the reason the loader's radius is plumbed through here.
    ParkedFleetDesc desc;
    RenderScene scene;
    BuildParkedFleet(desc, CLASS_RADII, scene);

    const auto wingSpread = [&scene](std::uint32_t _classId)
    {
      const InstanceRange& range = scene.classRanges[_classId];
      const InstanceRecord& first = scene.instances[range.firstInstance];
      const InstanceRecord& last = scene.instances[range.firstInstance + range.instanceCount - 1];
      const float dx = last.posWorld.x - first.posWorld.x;
      const float dz = last.posWorld.z - first.posWorld.z;
      return std::sqrt(dx * dx + dz * dz);
    };

    Assert::IsTrue(wingSpread(6) > wingSpread(0), L"the Carrier wing is spread wider than the Interceptor wing");
  }

  TEST_METHOD(NoMeshesMeansNoInstances)
  {
    ParkedFleetDesc desc;
    RenderScene scene;
    scene.instances.push_back(InstanceRecord{});
    BuildParkedFleet(desc, {}, scene);
    Assert::IsTrue(scene.instances.empty(), L"a client with no meshes draws nothing rather than indexing an empty table");
  }

  TEST_METHOD(FewerMeshesThanTheDescriptionAsksForIsNotAnOverrun)
  {
    // The mesh list is content (Outpost.json), so it can be shorter than the
    // default description expects. Clamping is the whole safety here.
    ParkedFleetDesc desc;
    const std::vector<float> threeClasses = {10.0f, 20.0f, 30.0f};
    RenderScene scene;
    BuildParkedFleet(desc, threeClasses, scene);

    Assert::AreEqual<std::size_t>(3, scene.classRanges.size());
    for (const InstanceRecord& instance : scene.instances)
    {
      Assert::IsTrue(instance.classId < threeClasses.size());
    }
  }
};

} // namespace NeuronClientTests
