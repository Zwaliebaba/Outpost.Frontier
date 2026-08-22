#include "pch.h"

#include "ShaderTable.h"

// Generated into Outpost/CompiledShaders by fxc, one header per stage, each
// defining `g_p<name>` (see Outpost.vcxproj). They are build outputs and are
// not in source control -- the .hlsl files beside them are the source.
//
// This is the only translation unit that includes them, which is deliberate:
// the arrays have internal linkage, so a second includer would be a second copy
// of every shader in the binary.
#include "LampPS.h"
#include "LampVS.h"
#include "NebulaPS.h"
#include "NebulaVS.h"
#include "OpaquePS.h"
#include "OpaqueVS.h"
#include "OverlayPS.h"
#include "OverlayVS.h"
#include "UiPS.h"
#include "UiVS.h"

#include <cstdint>
#include <span>

namespace Outpost
{
namespace
{

/// `g_p*` are declared `const BYTE[]` by fxc. The element type is spelled as a
/// template parameter rather than as `unsigned char` so that this does not
/// depend on how `BYTE` happens to be typedef'd; the assert is what actually
/// holds the assumption.
template <typename T, std::size_t N>
[[nodiscard]] std::span<const std::uint8_t> Bytes(const T (&_array)[N]) noexcept
{
  static_assert(sizeof(T) == 1, "a compiled shader is a byte array");
  return std::span<const std::uint8_t>{reinterpret_cast<const std::uint8_t*>(_array), N};
}

} // namespace

Neuron::PipelineShaders ShaderTable() noexcept
{
  Neuron::PipelineShaders shaders;
  shaders.opaqueVertex = Bytes(g_pOpaqueVS);
  shaders.opaquePixel = Bytes(g_pOpaquePS);
  shaders.nebulaVertex = Bytes(g_pNebulaVS);
  shaders.nebulaPixel = Bytes(g_pNebulaPS);
  shaders.overlayVertex = Bytes(g_pOverlayVS);
  shaders.overlayPixel = Bytes(g_pOverlayPS);
  shaders.uiVertex = Bytes(g_pUiVS);
  shaders.uiPixel = Bytes(g_pUiPS);
  shaders.lampVertex = Bytes(g_pLampVS);
  shaders.lampPixel = Bytes(g_pLampPS);
  return shaders;
}

} // namespace Outpost
