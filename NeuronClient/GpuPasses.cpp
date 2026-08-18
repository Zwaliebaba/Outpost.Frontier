#include "pch.h"

#include "GpuPasses.h"

#include "GlyphAtlas.h"
#include "GpuMeshes.h"
#include "GpuPipelines.h"
#include "GpuUploadRing.h"
#include "Log.h"
#include "OverlayMark.h"
#include "RenderWorld.h"
#include "Telemetry.h"

#include <algorithm>

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

void OverlayWorldPass::Record(const FrameContext& _context)
{
  NEURON_SPAN("OverlayWorld");

  m_ringCount = 0;
  m_barCount = 0;

  if (_context.overlayMarks == nullptr || _context.overlayMarks->marks.empty() || _context.pipelines == nullptr ||
      _context.pipelines->OverlayRings() == nullptr || _context.pipelines->OverlayBars() == nullptr)
  {
    return; // Nothing selected is the common case, and it costs one branch.
  }

  const OverlayMarkList& marks = *_context.overlayMarks;
  const auto markBytes = static_cast<std::uint32_t>(marks.marks.size() * sizeof(OverlayMark));

  GpuUploadRing::Allocation upload;
  if (!_context.uploadRing->Write(marks.marks.data(), markBytes, static_cast<std::uint32_t>(alignof(OverlayMark)), upload))
  {
    NEURON_COUNTER("OverlayMarkDrops", static_cast<std::int64_t>(marks.marks.size()));
    return; // Logged by the ring. A frame short of memory drops the overlay
            // rather than drawing half a selection.
  }

  D3D12_VERTEX_BUFFER_VIEW markView{};
  markView.BufferLocation = upload.gpu;
  markView.SizeInBytes = markBytes;
  markView.StrideInBytes = static_cast<UINT>(sizeof(OverlayMark));

  ID3D12GraphicsCommandList* commandList = _context.commandList;
  commandList->SetGraphicsRootSignature(_context.pipelines->RootSignature());
  commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
  commandList->IASetVertexBuffers(0, 1, &markView);

  // FrameConstants for the view-projection, PassConstants for the viewport size
  // the bars offset in. No textures: the marks are procedural.
  commandList->SetGraphicsRootConstantBufferView(static_cast<UINT>(RootSlot::FrameConstants), _context.frameConstants);
  commandList->SetGraphicsRootConstantBufferView(static_cast<UINT>(RootSlot::PassConstants), _context.passConstants);

  // Four vertices as a strip, instanced per mark. The plane-lying marks first
  // with depth, then the screen-facing ones without -- two pipelines over two
  // contiguous ranges of one upload, which is what `ringCount` exists to make
  // possible. It is called `ringCount` because selection rings were the first
  // thing in that half; from S9 it also holds order footprints and station
  // ticks, which lie on the same plane and want the same depth state.
  if (marks.ringCount > 0)
  {
    commandList->SetPipelineState(_context.pipelines->OverlayRings());
    commandList->DrawInstanced(4, marks.ringCount, 0, 0);
    m_ringCount = marks.ringCount;
  }

  const std::uint32_t barCount = marks.BarCount();
  if (barCount > 0)
  {
    commandList->SetPipelineState(_context.pipelines->OverlayBars());
    commandList->DrawInstanced(4, barCount, 0, marks.ringCount);
    m_barCount = barCount;
  }

  NEURON_COUNTER("OverlayMarks", static_cast<std::int64_t>(marks.marks.size()));
}

namespace
{

/*
 * Text runs into glyph quads.
 *
 * The pen walks the run in the baked size's own metrics: a bearing places the
 * bitmap against the pen and the advance moves it on. The face is monospace, so
 * the advance is the same for every glyph and the layout upstream could measure
 * a string without asking -- but *placing* a glyph still needs its bitmap's
 * offset, because a comma and a capital sit differently inside the same cell.
 *
 * `y` on a run is the top of the line rather than the baseline, so the ascent is
 * added once here. Every layout number on the prints is a box, and converting at
 * one place beats converting at every call site.
 *
 * A codepoint the bake has no glyph for is skipped and counted rather than
 * substituted: the atlas's alphabet is a boot-time decision, and a run of boxes
 * is a bake to fix rather than a layout to nudge.
 */
void ExpandTextRuns(const UiDrawList& _ui, const GlyphAtlas& _atlas, std::vector<UiInstance>& _outInstances,
                    std::uint64_t& _outMissing)
{
  const float atlasWidth = static_cast<float>(_atlas.TextureWidth());
  const float atlasHeight = static_cast<float>(_atlas.TextureHeight());
  if (atlasWidth <= 0.0f || atlasHeight <= 0.0f || _atlas.SizeCount() == 0)
  {
    return;
  }

  for (const UiTextRun& run : _ui.Runs())
  {
    const std::uint32_t sizeIndex = std::min<std::uint32_t>(run.sizeIndex, _atlas.SizeCount() - 1);
    const GlyphAtlasSize& size = _atlas.Size(sizeIndex);
    const float baseline = run.y + size.ascentPixels;

    float pen = run.x;
    const std::string_view text = _ui.Text(run);
    std::size_t byteIndex = 0;
    while (byteIndex < text.size())
    {
      // UTF-8, decoded here and nowhere earlier: the bake covers ASCII plus
      // the icon sheet's marker glyphs (ADR-006 §9), and those markers are
      // multi-byte. No shaping and no combining marks -- one codepoint is one
      // cell, which is what keeps layout a multiplication upstream.
      const char32_t codepoint = DecodeUtf8(text, byteIndex);
      const GlyphMetrics* glyph = _atlas.Find(sizeIndex, codepoint);
      if (glyph == nullptr)
      {
        ++_outMissing;
        pen += size.cellWidthPixels; // Keep the column, so one gap does not
                                     // shift the rest of the line.
        continue;
      }
      if (glyph->width == 0 || glyph->height == 0)
      {
        pen += glyph->advancePixels; // A space. It advances and draws nothing.
        continue;
      }

      UiInstance instance;
      instance.rect[0] = pen + static_cast<float>(glyph->bearingX);
      instance.rect[1] = baseline - static_cast<float>(glyph->bearingY);
      instance.rect[2] = static_cast<float>(glyph->width);
      instance.rect[3] = static_cast<float>(glyph->height);
      instance.uv[0] = static_cast<float>(glyph->atlasX) / atlasWidth;
      instance.uv[1] = static_cast<float>(glyph->atlasY) / atlasHeight;
      instance.uv[2] = static_cast<float>(glyph->width) / atlasWidth;
      instance.uv[3] = static_cast<float>(glyph->height) / atlasHeight;
      instance.colourRgba = run.colourRgba;
      instance.flags = UI_FLAG_GLYPH;
      _outInstances.push_back(instance);

      pen += glyph->advancePixels;
    }
  }
}

} // namespace

void UiPass::Record(const FrameContext& _context)
{
  NEURON_SPAN("Ui");

  m_panelCount = 0;
  m_glyphCount = 0;
  m_instances.clear();

  if (_context.ui == nullptr || _context.pipelines == nullptr || _context.pipelines->Ui() == nullptr)
  {
    return;
  }

  const UiDrawList& ui = *_context.ui;

  // Panels first and in order. The build order is the draw order because there
  // is one pipeline and no sort: a panel added after a run of text is meant to
  // be over it.
  for (const UiQuad& quad : ui.Quads())
  {
    UiInstance instance;
    instance.rect[0] = quad.rect.x;
    instance.rect[1] = quad.rect.y;
    instance.rect[2] = quad.rect.width;
    instance.rect[3] = quad.rect.height;
    instance.colourRgba = quad.colourRgba;
    if (quad.oriented)
    {
      // `rect` already carries centre and (length, thickness) -- `AddSegment`
      // wrote it that way, so nothing is converted here. The axis and the flag
      // are what tell the shader to read it that way.
      instance.flags = UI_FLAG_ORIENTED;
      instance.axis[0] = quad.axisX;
      instance.axis[1] = quad.axisY;
    }
    m_instances.push_back(instance);
  }
  m_panelCount = static_cast<std::uint32_t>(m_instances.size());

  // Then the glyphs. This is the only step that needs the atlas, which is why
  // everything upstream of it is device-free.
  if (_context.glyphAtlas != nullptr)
  {
    ExpandTextRuns(ui, *_context.glyphAtlas, m_instances, m_missingGlyphs);
  }
  m_glyphCount = static_cast<std::uint32_t>(m_instances.size()) - m_panelCount;

  if (m_instances.empty())
  {
    return;
  }

  const auto instanceBytes = static_cast<std::uint32_t>(m_instances.size() * sizeof(UiInstance));

  GpuUploadRing::Allocation upload;
  if (!_context.uploadRing->Write(m_instances.data(), instanceBytes, static_cast<std::uint32_t>(alignof(UiInstance)), upload))
  {
    NEURON_COUNTER("UiInstanceDrops", static_cast<std::int64_t>(m_instances.size()));
    m_panelCount = 0;
    m_glyphCount = 0;
    return; // A frame short of memory drops the HUD rather than drawing half of
            // it, which would read as the game having lost half its state.
  }

  D3D12_VERTEX_BUFFER_VIEW instanceView{};
  instanceView.BufferLocation = upload.gpu;
  instanceView.SizeInBytes = instanceBytes;
  instanceView.StrideInBytes = static_cast<UINT>(sizeof(UiInstance));

  ID3D12GraphicsCommandList* commandList = _context.commandList;
  commandList->SetGraphicsRootSignature(_context.pipelines->RootSignature());
  commandList->SetPipelineState(_context.pipelines->Ui());
  commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
  commandList->IASetVertexBuffers(0, 1, &instanceView);

  // PassConstants for the viewport size the pixel coordinates are converted
  // against; the texture table for the atlas at t0. No FrameConstants: the HUD
  // is in screen space and has no view to be projected through.
  commandList->SetGraphicsRootConstantBufferView(static_cast<UINT>(RootSlot::PassConstants), _context.passConstants);
  if (_context.textureTable.ptr != 0)
  {
    commandList->SetGraphicsRootDescriptorTable(static_cast<UINT>(RootSlot::Textures), _context.textureTable);
  }

  commandList->DrawInstanced(4, static_cast<UINT>(m_instances.size()), 0, 0);

  NEURON_COUNTER("UiInstances", static_cast<std::int64_t>(m_instances.size()));
}

void GpuPassList::Record(const FrameContext& _context)
{
  m_clear.Record(_context);
  m_opaque.Record(_context);
  m_nebula.Record(_context);
  m_overlayWorld.Record(_context);
  m_ui.Record(_context);
  // Present is the swapchain's, not a recorded pass.
}

} // namespace Neuron
