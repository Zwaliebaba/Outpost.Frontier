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

/*
 * The parked-fleet suite lived here and went with S7.
 *
 * It covered `BuildParkedFleet` and `AddScenery` -- the fleet the client
 * invented before there was one to replicate, and the station the composition
 * root converted out of the universe file. Both are gone: the scene arrives
 * through `WorldView::BuildScene` from real snapshots, and the station is a
 * `Structure` the server spawns like any other ship. Deleting the tests with
 * the code they covered is the point; keeping them would have meant keeping the
 * placeholder alive to be tested.
 *
 * What replaced them: `SeamTests` drives a world view through the interface,
 * and `GameLogicTests` covers the snapshot round trip and the interpolation
 * those scenes are built from.
 */

} // namespace NeuronClientTests
