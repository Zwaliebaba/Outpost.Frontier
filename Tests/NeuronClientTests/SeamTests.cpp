#include "pch.h"
#include "CppUnitTest.h"

#include "ByteWriter.h"
#include "OrderIntent.h"
#include "RenderWorld.h"
#include "WorldView.h"

#include <array>
#include <cstdint>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace Neuron;

/*
 * The client half of the engine/game seam (ADR-014 §2), driven with no game.
 *
 * This file is the slice's actual proof, and its most important property is
 * what it does *not* include: there is no GameLogic header here, and there is
 * no GameLogic reference in this test project's `.vcxproj`. A world view is
 * written from nothing but engine types and driven through every method on the
 * interface. If that ever stops compiling, the seam has grown a game shape.
 *
 * What it cannot prove is that `ClientApp` calls the seam, because `ClientApp`
 * needs a device and a window and CI has neither. The build's grep rule covers
 * the other half of the claim -- that no engine project references GameLogic --
 * and the frame itself remains a manual checkpoint (Build Order S1).
 */

namespace NeuronClientTests
{
namespace
{

/*
 * A world view that records what it was asked and answers plausibly.
 *
 * Written the way a second game would have to write one: engine types in,
 * engine types out, and no idea what any of the numbers mean. The `kind` it
 * refuses on is its own invention, which is exactly the point -- reason codes
 * belong to whoever implements this, and the engine only carries them.
 */
class StubWorldView final : public WorldView
{
public:
  static constexpr std::uint16_t REFUSE_KIND = 7;
  static constexpr std::uint16_t REFUSE_REASON = 42;

  /// Reads a tick out of the first four bytes, the way a real view reads it out
  /// of the snapshot header -- the engine cannot know it, so the game reports it.
  [[nodiscard]] std::uint32_t ApplySnapshot(std::span<const std::uint8_t> _payload) override
  {
    ++m_snapshotCount;
    if (_payload.size() < sizeof(std::uint32_t))
    {
      return 0; // Rejected: too short to carry a tick.
    }
    m_lastTick = static_cast<std::uint32_t>(_payload[0]) | (static_cast<std::uint32_t>(_payload[1]) << 8) |
                 (static_cast<std::uint32_t>(_payload[2]) << 16) | (static_cast<std::uint32_t>(_payload[3]) << 24);
    m_lastPayload.assign(_payload.begin(), _payload.end());
    return m_lastTick;
  }

  void BuildScene(double _renderTick, RenderScene& _outScene) override
  {
    ++m_sceneCount;
    m_lastRenderTick = _renderTick;

    _outScene.Clear();
    for (std::uint16_t classId = 0; classId < 3; ++classId)
    {
      InstanceRecord instance;
      instance.posWorld = DirectX::XMFLOAT3{static_cast<float>(classId) * 100.0f, 0.0f, static_cast<float>(_renderTick)};
      instance.classId = classId;
      _outScene.instances.push_back(instance);
    }
    _outScene.SortByClass(3);
  }

  [[nodiscard]] OrderVerdict PreCheck(const OrderIntent& _intent) override
  {
    OrderVerdict verdict;
    verdict.accepted = _intent.kind != REFUSE_KIND && _intent.entityCount > 0;
    verdict.reasonCode = verdict.accepted ? 0 : REFUSE_REASON;
    return verdict;
  }

  void SolvePreview(const OrderIntent& _intent, OrderPreview& _outPreview) override
  {
    _outPreview.Clear();
    for (std::uint32_t i = 0; i < _intent.entityCount; ++i)
    {
      if (!_outPreview.AddMark(_intent.targetXMetres + static_cast<float>(i) * 50.0f, _intent.targetYMetres))
      {
        break;
      }
    }
    _outPreview.extentMetres = static_cast<float>(_intent.entityCount) * 25.0f;
  }

  [[nodiscard]] bool EncodeOrder(const OrderIntent& _intent, ByteWriter& _writer) override
  {
    _writer.WriteUInt16(_intent.kind);
    _writer.WriteUInt16(_intent.parameter);
    _writer.WriteUInt16(static_cast<std::uint16_t>(_intent.entityCount));
    return _writer.Ok();
  }

  [[nodiscard]] std::uint64_t SchemaHash() const override { return 0xabcdefull; }
  [[nodiscard]] std::uint64_t ContentHash() const override { return 0x123456ull; }

  [[nodiscard]] std::uint32_t SnapshotCount() const noexcept { return m_snapshotCount; }
  [[nodiscard]] std::uint32_t SceneCount() const noexcept { return m_sceneCount; }
  [[nodiscard]] std::uint32_t LastTick() const noexcept { return m_lastTick; }
  [[nodiscard]] double LastRenderTick() const noexcept { return m_lastRenderTick; }
  [[nodiscard]] const std::vector<std::uint8_t>& LastPayload() const noexcept { return m_lastPayload; }

private:
  std::uint32_t m_snapshotCount = 0;
  std::uint32_t m_sceneCount = 0;
  std::uint32_t m_lastTick = 0;
  double m_lastRenderTick = 0.0;
  std::vector<std::uint8_t> m_lastPayload;
};

/// Drives a view through the interface only, the way ClientApp does. Taking the
/// base reference is the assertion: nothing below needs the concrete type.
void DriveOneFrame(WorldView& _view, double _renderTick, RenderScene& _outScene)
{
  _view.BuildScene(_renderTick, _outScene);
}

} // namespace

TEST_CLASS(WorldViewSeamTests)
{
public:
  TEST_METHOD(AWorldViewCanBeWrittenWithNoGameAtAll)
  {
    // The compile is most of this test. StubWorldView is built from ByteWriter,
    // OrderIntent, RenderScene and WorldView -- four engine headers and nothing
    // else -- which is what ADR-014 §1 promises a second game would need.
    StubWorldView view;
    WorldView& seam = view;

    Assert::AreEqual<std::uint64_t>(0xabcdefull, seam.SchemaHash());
    Assert::AreEqual<std::uint64_t>(0x123456ull, seam.ContentHash());
  }

  TEST_METHOD(TheSceneComesThroughTheSeamAndNowhereElse)
  {
    StubWorldView view;
    RenderScene scene;

    DriveOneFrame(view, 12.5, scene);

    Assert::AreEqual<std::uint32_t>(1, view.SceneCount());
    Assert::AreEqual(12.5, view.LastRenderTick(), 1e-9);
    Assert::AreEqual<std::size_t>(3, scene.instances.size());
    Assert::AreEqual<std::size_t>(3, scene.classRanges.size());
    for (std::uint16_t classId = 0; classId < 3; ++classId)
    {
      Assert::AreEqual<std::uint32_t>(1, scene.classRanges[classId].instanceCount);
    }
  }

  TEST_METHOD(TheSceneIsRebuiltNotAccumulated)
  {
    // ClientApp calls this every frame into the same scene. A view that
    // appended would grow the instance list without bound, and the symptom
    // would be a frame-time slope rather than anything that looks like a bug.
    StubWorldView view;
    RenderScene scene;

    DriveOneFrame(view, 1.0, scene);
    DriveOneFrame(view, 2.0, scene);
    DriveOneFrame(view, 3.0, scene);

    Assert::AreEqual<std::uint32_t>(3, view.SceneCount());
    Assert::AreEqual<std::size_t>(3, scene.instances.size(), L"three frames, three instances, not nine");
  }

  TEST_METHOD(ASnapshotCrossesAsOpaqueBytes)
  {
    // The engine frames and orders the payload; it does not look inside. What
    // this asserts is that nothing on the way in reinterprets it.
    StubWorldView view;
    // Little-endian 9001 followed by a byte the view must not touch.
    const std::array<std::uint8_t, 5> payload{0x29, 0x23, 0x00, 0x00, 0xef};

    Assert::AreEqual<std::uint32_t>(9001, view.ApplySnapshot(payload), L"the game reports the tick it read");

    Assert::AreEqual<std::uint32_t>(1, view.SnapshotCount());
    Assert::AreEqual<std::uint32_t>(9001, view.LastTick());
    Assert::AreEqual<std::size_t>(payload.size(), view.LastPayload().size());
    for (std::size_t i = 0; i < payload.size(); ++i)
    {
      Assert::AreEqual(payload[i], view.LastPayload()[i], L"a snapshot byte changed in transit");
    }
  }

  TEST_METHOD(AVerdictCarriesTheGamesReasonCodeUntouched)
  {
    // ADR-014 §3: a local bounce and a server refusal must say the same thing.
    // The engine cannot guarantee that by inspecting the code -- it guarantees
    // it by never inspecting the code.
    StubWorldView view;
    const std::uint16_t selection[] = {1, 2, 3};

    OrderIntent accepted;
    accepted.kind = 1;
    accepted.entityIds = selection;
    accepted.entityCount = 3;
    const OrderVerdict yes = view.PreCheck(accepted);
    Assert::IsTrue(yes.accepted);
    Assert::AreEqual<std::uint16_t>(0, yes.reasonCode);

    OrderIntent refused = accepted;
    refused.kind = StubWorldView::REFUSE_KIND;
    const OrderVerdict no = view.PreCheck(refused);
    Assert::IsFalse(no.accepted);
    Assert::AreEqual<std::uint16_t>(StubWorldView::REFUSE_REASON, no.reasonCode);

    OrderIntent empty;
    empty.kind = 1;
    const OrderVerdict nothingSelected = view.PreCheck(empty);
    Assert::IsFalse(nothingSelected.accepted, L"an order with no entities is not an order");
  }

  TEST_METHOD(APreviewComesBackAsPlaneMarks)
  {
    StubWorldView view;
    const std::uint16_t selection[] = {1, 2, 3, 4};

    OrderIntent intent;
    intent.entityIds = selection;
    intent.entityCount = 4;
    intent.targetXMetres = 1000.0f;
    intent.targetYMetres = -250.0f;

    OrderPreview preview;
    view.SolvePreview(intent, preview);

    Assert::AreEqual<std::uint32_t>(4, preview.markCount);
    Assert::AreEqual(1000.0f, preview.markXMetres[0], 1e-3f);
    Assert::AreEqual(1150.0f, preview.markXMetres[3], 1e-3f);
    Assert::AreEqual(-250.0f, preview.markYMetres[3], 1e-3f);
    Assert::AreEqual(100.0f, preview.extentMetres, 1e-3f);
  }

  TEST_METHOD(APreviewRefusesToOverflowRatherThanTruncateQuietly)
  {
    // A footprint that silently stopped at the cap would read as a complete
    // arrangement that happens to be smaller -- the wrong thing to show
    // someone about to commit a fleet to it.
    OrderPreview preview;
    for (std::uint32_t i = 0; i < MAX_ORDER_PREVIEW_MARKS; ++i)
    {
      Assert::IsTrue(preview.AddMark(static_cast<float>(i), 0.0f));
    }
    Assert::AreEqual<std::uint32_t>(MAX_ORDER_PREVIEW_MARKS, preview.markCount);
    Assert::IsFalse(preview.AddMark(0.0f, 0.0f), L"the cap is reported, not absorbed");

    preview.Clear();
    Assert::AreEqual<std::uint32_t>(0, preview.markCount);
    Assert::AreEqual(0.0f, preview.extentMetres, 1e-6f);
  }

  TEST_METHOD(AnOrderIsEncodedByTheGameAndCarriedByTheEngine)
  {
    StubWorldView view;
    const std::uint16_t selection[] = {5, 6};

    OrderIntent intent;
    intent.kind = 3;
    intent.parameter = 11;
    intent.entityIds = selection;
    intent.entityCount = 2;

    std::array<std::uint8_t, 32> buffer{};
    ByteWriter writer{buffer};
    Assert::IsTrue(view.EncodeOrder(intent, writer));
    Assert::AreEqual<std::size_t>(6, writer.Written().size(), L"three u16s, and the engine chose none of them");
  }

  TEST_METHOD(EncodingRefusesRatherThanSendingHalfAnOrder)
  {
    // "Not sent" and "sent empty" are different outcomes, and only one of them
    // leaves the player's fleet doing what they expected.
    StubWorldView view;
    const std::uint16_t selection[] = {5};

    OrderIntent intent;
    intent.entityIds = selection;
    intent.entityCount = 1;

    std::array<std::uint8_t, 3> tooSmall{};
    ByteWriter writer{tooSmall};
    Assert::IsFalse(view.EncodeOrder(intent, writer));
  }
};

TEST_CLASS(NullWorldViewTests)
{
public:
  TEST_METHOD(ItBuildsAnEmptySceneRatherThanAPlaceholderOne)
  {
    // A client wired to nothing should look like a client wired to nothing.
    NullWorldView view;
    RenderScene scene;

    InstanceRecord stale;
    stale.classId = 2;
    scene.instances.push_back(stale);

    DriveOneFrame(view, 4.0, scene);
    Assert::AreEqual<std::size_t>(0, scene.instances.size(), L"last frame's scene does not survive");
  }

  TEST_METHOD(ItRefusesEverythingAndSaysSoConsistently)
  {
    NullWorldView view;

    const OrderVerdict verdict = view.PreCheck(OrderIntent{});
    Assert::IsFalse(verdict.accepted);
    Assert::AreEqual<std::uint32_t>(0, verdict.serverOrderId, L"nothing has been assigned an id");

    OrderPreview preview;
    Assert::IsTrue(preview.AddMark(1.0f, 1.0f));
    view.SolvePreview(OrderIntent{}, preview);
    Assert::AreEqual<std::uint32_t>(0, preview.markCount);

    std::array<std::uint8_t, 16> buffer{};
    ByteWriter writer{buffer};
    Assert::IsFalse(view.EncodeOrder(OrderIntent{}, writer));

    Assert::AreEqual<std::uint64_t>(0, view.SchemaHash());
    Assert::AreEqual<std::uint64_t>(0, view.ContentHash());
  }

  TEST_METHOD(ItRecordsTheSizeButClaimsNoTick)
  {
    // Not for the game's sake -- there is none -- but so a client running
    // against no world can still show that snapshots are arriving.
    NullWorldView view;
    const std::array<std::uint8_t, 4> payload{1, 2, 3, 4};

    // Zero, not 77: a view with no world cannot say when it is, and a made-up
    // tick would give the clock estimate something to chase that nothing emits.
    Assert::AreEqual<std::uint32_t>(0, view.ApplySnapshot(payload), L"a null view reports no tick");
    Assert::AreEqual<std::uint32_t>(4, view.LastPayloadBytes());
  }
};

} // namespace NeuronClientTests
