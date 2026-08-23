#include "pch.h"

#include "RenderWorld.h"

#include <algorithm>
#include <cmath>

namespace Neuron
{
namespace
{

using namespace DirectX;

} // namespace

void RenderScene::Clear() noexcept
{
  // Capacity is kept on purpose: this runs once a frame, and the instance count
  // is stable, so releasing the storage would be an allocation every frame for
  // no gain.
  instances.clear();
  classRanges.clear();
  entities.clear();

  // And the count with them. It is a statement about the frame being built, so
  // a scene that was cleared and not refilled must not keep the last frame's
  // answer -- which would leave a chip on screen claiming ships are hidden on a
  // grid nobody is watching any more.
  culledCount = 0;
}

void RenderScene::SortByClass(std::uint32_t _classCount)
{
  std::stable_sort(instances.begin(), instances.end(),
                   [](const InstanceRecord& _a, const InstanceRecord& _b) noexcept { return _a.classId < _b.classId; });

  classRanges.assign(_classCount, InstanceRange{});
  for (std::uint32_t i = 0; i < instances.size(); ++i)
  {
    const std::uint32_t classId = instances[i].classId;
    if (classId >= _classCount)
    {
      continue; // An instance of a class with no mesh simply is not drawn.
    }
    if (classRanges[classId].instanceCount == 0)
    {
      classRanges[classId].firstInstance = i;
    }
    ++classRanges[classId].instanceCount;
  }
}

} // namespace Neuron
