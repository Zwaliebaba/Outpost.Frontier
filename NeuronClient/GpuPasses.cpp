#include "pch.h"

#include "GpuPasses.h"

#include "GpuMeshes.h"
#include "GpuPipelines.h"
#include "GpuUploadRing.h"
#include "Log.h"
#include "RenderWorld.h"
#include "Telemetry.h"

namespace Neuron
{

void ClearPass::Record(const FrameContext& _context) const
{
  D3D12_VIEWPORT viewport{};
  viewport.Width = static_cast<float>(_context.viewportWidth);
  viewport.Height = static_cast<float>(_context.viewportHeight);
  viewport.MinDepth = 0.0f;
  viewport.MaxDepth = 1.0f;

  D3D12_RECT scissor{};
  scissor.right = static_cast<LONG>(_context.viewportWidth);
  scissor.bottom = static_cast<LONG>(_context.viewportHeight);

  _context.commandList->RSSetViewports(1, &viewport);
  _context.commandList->RSSetScissorRects(1, &scissor);
  _context.commandList->OMSetRenderTargets(1, &_context.renderTargetView, FALSE, &_context.depthStencilView);

  _context.commandList->ClearRenderTargetView(_context.renderTargetView, _context.clearColour, 0, nullptr);
  // 1.0 because the projection is LESS-tested and near maps to 0 (ADR-006 §3a),
  // and because it is the value the depth buffer's fast-clear was created with.
  _context.commandList->ClearDepthStencilView(_context.depthStencilView, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
}

void OpaquePass::Record(const FrameContext& _context)
{
  NEURON_SPAN("Opaque");

  m_drawCount = 0;
  m_instanceCount = 0;

  if (_context.scene == nullptr || _context.meshes == nullptr || _context.pipelines == nullptr || _context.uploadRing == nullptr)
  {
    return;
  }

  const RenderScene& scene = *_context.scene;
  if (scene.instances.empty())
  {
    return;
  }

  // The whole frame's instances go up as one stream and every class draws a run
  // of it, which is why the scene is sorted by class: one upload, one vertex
  // buffer binding per class, and StartInstanceLocation does the rest.
  const auto instanceBytes = static_cast<std::uint32_t>(scene.instances.size() * sizeof(InstanceRecord));
  GpuUploadRing::Allocation instances;
  if (!_context.uploadRing->Write(scene.instances.data(), instanceBytes, static_cast<std::uint32_t>(alignof(InstanceRecord)),
                                  instances))
  {
    NEURON_COUNTER("OpaqueInstanceDrops", static_cast<std::int64_t>(scene.instances.size()));
    return; // Logged by the ring. A frame short of instance memory drops the
            // pass rather than drawing a scene that is partly last frame's.
  }

  D3D12_VERTEX_BUFFER_VIEW instanceView{};
  instanceView.BufferLocation = instances.gpu;
  instanceView.SizeInBytes = instanceBytes;
  instanceView.StrideInBytes = static_cast<UINT>(sizeof(InstanceRecord));

  ID3D12GraphicsCommandList* commandList = _context.commandList;
  commandList->SetGraphicsRootSignature(_context.pipelines->RootSignature());
  commandList->SetPipelineState(_context.pipelines->Opaque());
  commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

  commandList->SetGraphicsRootConstantBufferView(static_cast<UINT>(RootSlot::FrameConstants), _context.frameConstants);
  commandList->SetGraphicsRootConstantBufferView(static_cast<UINT>(RootSlot::PassConstants), _context.passConstants);
  if (_context.textureTable.ptr != 0)
  {
    commandList->SetGraphicsRootDescriptorTable(static_cast<UINT>(RootSlot::Textures), _context.textureTable);
  }

  const std::uint32_t classCount = std::min(_context.meshes->Count(), static_cast<std::uint32_t>(scene.classRanges.size()));
  for (std::uint32_t classId = 0; classId < classCount; ++classId)
  {
    const InstanceRange& range = scene.classRanges[classId];
    if (range.instanceCount == 0)
    {
      continue;
    }

    const GpuMesh& mesh = _context.meshes->Mesh(classId);
    if (!mesh.Valid())
    {
      continue;
    }

    const D3D12_VERTEX_BUFFER_VIEW streams[] = {mesh.vertexView, instanceView};
    commandList->IASetVertexBuffers(0, static_cast<UINT>(_countof(streams)), streams);
    commandList->IASetIndexBuffer(&mesh.indexView);

    for (const SubmeshRange& submesh : mesh.submeshes)
    {
      // One root constant, one draw. Sorting submeshes by material across
      // classes would save a few of these and cost a bind of the mesh per
      // material -- worse at nine meshes, and worth revisiting at ninety.
      commandList->SetGraphicsRoot32BitConstant(static_cast<UINT>(RootSlot::DrawConstants),
                                                static_cast<UINT>(submesh.material), 0);
      commandList->DrawIndexedInstanced(submesh.indexCount, range.instanceCount, submesh.firstIndex, 0, range.firstInstance);
      ++m_drawCount;
    }

    m_instanceCount += range.instanceCount;
  }

  NEURON_COUNTER("OpaqueDraws", m_drawCount);
}

void NebulaPass::Record(const FrameContext& _context)
{
  NEURON_SPAN("Nebula");

  m_drew = false;

  if (!_context.nebulaReady || _context.pipelines == nullptr || _context.pipelines->Nebula() == nullptr ||
      _context.textureTable.ptr == 0)
  {
    return;
  }

  ID3D12GraphicsCommandList* commandList = _context.commandList;
  commandList->SetGraphicsRootSignature(_context.pipelines->RootSignature());
  commandList->SetPipelineState(_context.pipelines->Nebula());
  commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

  // PassConstants and the texture table only. FrameConstants is not bound
  // because Nebula.hlsl does not declare it: setting the root signature
  // invalidates every root argument, so what a pass must bind is exactly what
  // its shaders read, and binding more would imply this one reads more.
  commandList->SetGraphicsRootConstantBufferView(static_cast<UINT>(RootSlot::PassConstants), _context.passConstants);
  commandList->SetGraphicsRootDescriptorTable(static_cast<UINT>(RootSlot::Textures), _context.textureTable);

  // No vertex or index buffer is set on purpose: the vertex shader builds the
  // triangle from SV_VertexID, so there is no geometry to keep in sync with it.
  // Whatever the Opaque pass left bound stays bound and is simply not read --
  // this PSO declares no input layout, and DrawInstanced consults no index
  // buffer. Clearing them would be two calls that change nothing.
  commandList->DrawInstanced(3, 1, 0, 0);

  m_drew = true;
}

void GpuPassList::Record(const FrameContext& _context)
{
  m_clear.Record(_context);
  m_opaque.Record(_context);
  m_nebula.Record(_context);
  // OverlayWorld (S8) and Ui (S11) are recorded here, in this order, when they
  // exist. Present is the swapchain's, not a recorded pass.
}

} // namespace Neuron
