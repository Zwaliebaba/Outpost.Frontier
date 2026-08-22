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
  static constexpr std::uint16_t DEFAULT_KIND = 5;
  static constexpr std::uint16_t DEFAULT_PARAMETER = 9;

  /*
   * Takes a frame the engine assembled (ADR-022).
   *
   * **The tick comes in now rather than out.** It used to be read out of the
   * first four bytes of an opaque payload, because the game was the only side
   * that could read a snapshot header; the header is the engine's since U3d-b,
   * so the number arrives with the frame and there is nothing to guess.
   *
   * The tail is refused when it is too short to be one, which stands in for the
   * real view's "a malformed tail leaves the picture untouched".
   */
  [[nodiscard]] bool ApplyFrame(const ReplicatedFrame& _frame) override
  {
    ++m_snapshotCount;
    if (!_frame.tail.empty() && _frame.tail.size() < sizeof(std::uint32_t))
    {
      return false; // Rejected: something arrived that is not a tail.
    }
    m_lastTick = _frame.tick;
    m_lastPayload.assign(_frame.tail.begin(), _frame.tail.end());
    m_lastEntityCount = static_cast<std::uint32_t>(_frame.entities.size());
    m_lastCulledCount = _frame.culledCount;
    return true;
  }

  [[nodiscard]] std::uint32_t LastEntityCount() const noexcept { return m_lastEntityCount; }
  [[nodiscard]] std::uint16_t LastCulledCount() const noexcept { return m_lastCulledCount; }

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

  /// Numbers this stub made up, which is the point: the client copies them into
  /// an intent and never learns what a 5 or a 9 is.
  [[nodiscard]] OrderDefaults DefaultOrder() const override { return OrderDefaults{DEFAULT_KIND, DEFAULT_PARAMETER}; }

  /*
   * Two options for its own kind and none for anything else, with parameters
   * that are neither contiguous nor starting at zero.
   *
   * That is deliberate. A client that stepped `parameter` from 0 upward, or
   * that assumed the default is the first entry, would pass against a game
   * whose formations happen to be 0, 1, 2 and fail against this one.
   */
  [[nodiscard]] std::uint32_t OrderOptions(std::uint16_t _kind, std::span<OrderOption> _outOptions) const override
  {
    if (_kind != DEFAULT_KIND || _outOptions.size() < 2)
    {
      return 0;
    }
    _outOptions[0] = OrderOption{4, "loose"};
    _outOptions[1] = OrderOption{DEFAULT_PARAMETER, "tight"};
    return 2;
  }

  /// One order running, invented the same way. A real view reads these out of
  /// the newest snapshot; the engine cannot tell the difference, and that is
  /// what the seam is for.
  void PollOrderFeedback(OrderFeedback& _outFeedback) override
  {
    ++m_feedbackCount;
    _outFeedback.Clear();
    _outFeedback.lastOrderSeqProcessed = m_lastOrderSeqProcessed;

    OrderProgress progress;
    progress.serverOrderId = 900;
    progress.clientOrderSeq = m_lastOrderSeqProcessed;
    progress.state = 1;
    progress.legIndex = 2;
    progress.legCount = 4;
    progress.memberCount = 6;
    (void)_outFeedback.Add(progress);
  }

  /*
   * Two groups, and the stub aggregates them itself -- which is the shape of
   * the claim. A game with no groups returns zero and the panel draws empty;
   * this one has groups it named, counted and combined health for on its own
   * side of the seam.
   */
  [[nodiscard]] std::uint32_t BuildRoster(std::span<const EntityId> _selectedIds,
                                          std::span<RosterRow> _outRows) const override
  {
    if (_outRows.size() < 2)
    {
      return 0;
    }
    _outRows[0] = RosterRow{"ALPHA", 11, 4, 0, 200, 100};
    _outRows[1] = RosterRow{"BETA", 12, 2, 0, 255, 0};

    // Anything selected counts against the first row, which is enough for a
    // test that cares whether the number crosses rather than how it was found.
    _outRows[0].selectedCount = static_cast<std::uint16_t>(_selectedIds.size());
    return 2;
  }

  /*
   * Two commands, one of them with no content -- the shape the command row is
   * built against. A stub that reported only working commands would let a row
   * that quietly dropped the greyed ones pass.
   */
  [[nodiscard]] std::uint32_t OrderKinds(std::span<const EntityId>,
                                         std::span<OrderKindOption> _outKinds) const override
  {
    if (_outKinds.size() < 2)
    {
      return 0;
    }
    _outKinds[0] = OrderKindOption{7, "Shove", "Shape", true};
    _outKinds[1] = OrderKindOption{9, "Smite", nullptr, false};
    return 2;
  }

  [[nodiscard]] const char* ReasonText(std::uint16_t _reasonCode) const override
  {
    return _reasonCode == REFUSE_REASON ? "the stub said no" : "something else";
  }

  [[nodiscard]] std::uint64_t SchemaHash() const override { return 0xabcdefull; }
  [[nodiscard]] std::uint64_t ContentHash() const override { return 0x123456ull; }

  void SetLastOrderSeqProcessed(std::uint32_t _seq) noexcept { m_lastOrderSeqProcessed = _seq; }
  [[nodiscard]] std::uint32_t FeedbackCount() const noexcept { return m_feedbackCount; }

  [[nodiscard]] std::uint32_t SnapshotCount() const noexcept { return m_snapshotCount; }
  [[nodiscard]] std::uint32_t SceneCount() const noexcept { return m_sceneCount; }
  [[nodiscard]] std::uint32_t LastTick() const noexcept { return m_lastTick; }
  [[nodiscard]] double LastRenderTick() const noexcept { return m_lastRenderTick; }
  [[nodiscard]] const std::vector<std::uint8_t>& LastPayload() const noexcept { return m_lastPayload; }

private:
  std::uint32_t m_snapshotCount = 0;
  std::uint32_t m_lastEntityCount = 0;
  std::uint16_t m_lastCulledCount = 0;
  std::uint32_t m_sceneCount = 0;
  std::uint32_t m_lastTick = 0;
  std::uint32_t m_feedbackCount = 0;
  std::uint32_t m_lastOrderSeqProcessed = 0;
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

  TEST_METHOD(TheGamesTailCrossesAsOpaqueBytes)
  {
    /*
     * **The envelope stopped being opaque and the tail did not** (ADR-022 §1).
     *
     * The engine now reads the header, holds the baseline and assembles the
     * tick -- all of it link semantics over a record type it owns. What it
     * still does not look inside is the game's own bytes for the tick, and this
     * asserts that nothing on the way in reinterprets them.
     */
    StubWorldView view;
    // Five bytes the view must carry and not touch.
    const std::array<std::uint8_t, 5> payload{0x29, 0x23, 0x00, 0x00, 0xef};

    ReplicatedFrame frame;
    frame.tick = 9001;
    frame.gridId = 42;
    frame.culledCount = 7;
    frame.tail = payload;
    Assert::IsTrue(view.ApplyFrame(frame), L"the game took the frame");

    Assert::AreEqual<std::uint32_t>(1, view.SnapshotCount());
    Assert::AreEqual<std::uint32_t>(9001, view.LastTick());
    Assert::AreEqual<std::uint16_t>(7, view.LastCulledCount(), L"the culled count crosses the seam unread");
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
    const EntityId selection[] = {1, 2, 3};

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
    const EntityId selection[] = {1, 2, 3, 4};

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
    const EntityId selection[] = {5, 6};

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

  TEST_METHOD(TheGameSaysWhatKindOfOrderThePuckMakes)
  {
    /*
     * The client turns a gesture into a place and a facing. *Which command*
     * that is belongs to a surface that does not exist yet, so until it does
     * the answer comes from the game -- and the numbers are the game's, which
     * this stub demonstrates by picking two nobody else uses. Leaving the
     * intent's fields zero would have worked today and only because
     * `OrderKind::Move` and `FormationId::Line` both happen to be zero.
     */
    StubWorldView view;
    WorldView& seam = view;

    const OrderDefaults defaults = seam.DefaultOrder();
    Assert::AreEqual<std::uint16_t>(StubWorldView::DEFAULT_KIND, defaults.kind, L"the kind is the game's");
    Assert::AreEqual<std::uint16_t>(StubWorldView::DEFAULT_PARAMETER, defaults.parameter, L"and so is the parameter");
    Assert::AreNotEqual<std::uint16_t>(0, defaults.kind, L"and neither is zero by luck");
  }

  TEST_METHOD(TheGameListsWhatItsParameterMayBe)
  {
    /*
     * What the formation control is driven from (S10), and what the command
     * wheel's sub-ring will be drawn from (S11). The engine gets numbers to
     * send and words to show; it learns neither how many formations exist nor
     * what they are called, which is the same bargain `ReasonText` strikes.
     *
     * The stub's parameters are 4 and 9 rather than 0 and 1, so a client that
     * counted from zero would fail here rather than in the game that ships.
     */
    StubWorldView view;
    WorldView& seam = view;

    OrderOption options[MAX_ORDER_OPTIONS] = {};
    const std::uint32_t count = seam.OrderOptions(seam.DefaultOrder().kind, options);

    Assert::AreEqual<std::uint32_t>(2, count, L"two options for this kind");
    Assert::AreEqual<std::uint16_t>(4, options[0].parameter, L"the numbers are the game's");
    Assert::AreEqual("loose", options[0].name, L"and so are the words");
    Assert::AreEqual("tight", options[1].name);

    // The default has to be findable in the list, or a client that starts on
    // the game's default cannot line the two up.
    Assert::AreEqual<std::uint16_t>(seam.DefaultOrder().parameter, options[1].parameter,
                                    L"the default is one of the options, and not necessarily the first");

    Assert::AreEqual<std::uint32_t>(0, seam.OrderOptions(9999, options), L"a kind it does not know offers nothing");
  }

  TEST_METHOD(AnOptionListSmallerThanTheGameWantsIsNotOverrun)
  {
    // The span is the client's buffer and the game writes at most its size.
    // `MAX_ORDER_OPTIONS` is the engine's cap; a game with more has to answer
    // with fewer rather than write past the end.
    StubWorldView view;
    WorldView& seam = view;

    OrderOption one[1] = {};
    Assert::AreEqual<std::uint32_t>(0, seam.OrderOptions(seam.DefaultOrder().kind, one),
                                    L"this stub would rather offer nothing than half a list");
  }

  TEST_METHOD(TheGameAggregatesTheRosterAndTheEngineOnlyDrawsIt)
  {
    /*
     * The engine has `EntityRecord::groupId` and could group by it in four
     * lines. It must not: doing so would decide that groups are worth showing,
     * that they are named, and how a group's health combines. Those are
     * questions about a particular game, and this call is where they are
     * answered on the side allowed to answer them.
     *
     * Nothing in the engine's half of this test knows the word "wing".
     */
    StubWorldView view;
    WorldView& seam = view;

    const EntityId selected[] = {1, 2, 3};
    RosterRow rows[MAX_ROSTER_ROWS] = {};
    const std::uint32_t count = seam.BuildRoster(selected, rows);

    Assert::AreEqual<std::uint32_t>(2, count, L"two rows");
    Assert::AreEqual("ALPHA", rows[0].name, L"named by the game");
    Assert::AreEqual<std::uint16_t>(11, rows[0].groupId, L"with an id the engine echoes and never reads");
    Assert::AreEqual<std::uint16_t>(4, rows[0].shipCount);
    Assert::AreEqual<std::uint16_t>(3, rows[0].selectedCount, L"and the selection counted on the game's side");
    Assert::AreEqual<std::uint8_t>(200, rows[0].hullGauge, L"gauges are 0-255, EntityRecord's own scale");
  }

  TEST_METHOD(ARosterBufferSmallerThanTheGameWantsIsNotOverrun)
  {
    StubWorldView view;
    WorldView& seam = view;

    RosterRow one[1] = {};
    Assert::AreEqual<std::uint32_t>(0, seam.BuildRoster(std::span<const EntityId>{}, one),
                                    L"this stub would rather offer nothing than half a roster");
  }

  TEST_METHOD(OrderProgressCrossesAsNumbersTheEngineDoesNotInterpret)
  {
    // The promotion path. The engine polls, copies six numbers, and compares
    // one of them for change -- it never learns that `state` has names.
    StubWorldView view;
    view.SetLastOrderSeqProcessed(31);
    WorldView& seam = view;

    OrderFeedback feedback;
    seam.PollOrderFeedback(feedback);

    Assert::AreEqual<std::uint32_t>(1, view.FeedbackCount(), L"polled, not pushed");
    Assert::AreEqual<std::uint32_t>(31, feedback.lastOrderSeqProcessed, L"the high-water mark comes through");
    Assert::AreEqual<std::uint32_t>(1, feedback.orderCount, L"one order running");
    Assert::AreEqual<std::uint32_t>(900, feedback.orders[0].serverOrderId, L"with the id the game assigned");
    Assert::AreEqual<std::uint8_t>(2, feedback.orders[0].legIndex, L"on leg two");
    Assert::AreEqual<std::uint8_t>(4, feedback.orders[0].legCount, L"of four");
  }

  TEST_METHOD(TheBounceStringComesFromTheSameSideTheReasonCodeDid)
  {
    /*
     * BounceParity's other half. The reason code is a number the engine cannot
     * read, so the words have to come from whoever assigned it -- and both a
     * local refusal and a server refusal reach this one function, which is what
     * makes them say the same thing rather than two tables agreeing today.
     */
    StubWorldView view;
    WorldView& seam = view;

    OrderIntent intent;
    intent.kind = StubWorldView::REFUSE_KIND;
    intent.entityCount = 1;
    const EntityId id = 1;
    intent.entityIds = &id;

    const OrderVerdict verdict = seam.PreCheck(intent);
    Assert::IsFalse(verdict.accepted, L"the stub refuses its own kind");
    Assert::AreEqual("the stub said no", seam.ReasonText(verdict.reasonCode), L"and the text is its own too");
    Assert::AreEqual("something else", seam.ReasonText(1234), L"a code it does not know still gets words");
  }

  TEST_METHOD(EncodingRefusesRatherThanSendingHalfAnOrder)
  {
    // "Not sent" and "sent empty" are different outcomes, and only one of them
    // leaves the player's fleet doing what they expected.
    StubWorldView view;
    const EntityId selection[] = {5};

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

    OrderFeedback feedback;
    feedback.lastOrderSeqProcessed = 5;
    Assert::IsTrue(feedback.Add(OrderProgress{}));
    view.PollOrderFeedback(feedback);
    Assert::AreEqual<std::uint32_t>(0, feedback.orderCount, L"a world with no orders reports none");
    Assert::AreEqual<std::uint32_t>(0, feedback.lastOrderSeqProcessed, L"and no high-water mark either");

    Assert::AreEqual<std::uint16_t>(0, view.DefaultOrder().kind, L"there is no command to give");
    OrderOption options[MAX_ORDER_OPTIONS] = {};
    Assert::AreEqual<std::uint32_t>(0, view.OrderOptions(0, options), L"and no parameters to give it");
    RosterRow rows[MAX_ROSTER_ROWS] = {};
    Assert::AreEqual<std::uint32_t>(0, view.BuildRoster(std::span<const EntityId>{}, rows),
                                    L"a world with no fleet has no roster");
    EntityId members[8] = {};
    Assert::AreEqual<std::uint32_t>(0, view.BuildGroupMembers(1, members),
                                    L"and no group in it to press, so no ships behind one");
    Assert::IsNotNull(view.ReasonText(0), L"and still never a null string to draw");

    Assert::AreEqual<std::uint64_t>(0, view.SchemaHash());
    Assert::AreEqual<std::uint64_t>(0, view.ContentHash());
  }

  TEST_METHOD(AViewWithNoGameOffersNoStationAndSendsNothingToIt)
  {
    /*
     * The station half of the same posture, and every clause of it is load
     * bearing on a real screen.
     *
     * No tabs and no roster means the hangar draws as empty rather than as a
     * row of words the engine invented. No action means UNDOCK has no verb, so
     * there is nothing to press. `PreCheckStation` refusing is the important
     * one -- a view that cannot judge must not wave a command through, which
     * is `ValidationView`'s optional-field posture applied to a whole seam.
     *
     * And a cap of **zero** rather than "no limit": read the other way, a
     * client with no game would build one command naming a whole hangar, which
     * is the payload `MAX_SHIPS_PER_ORDER` exists to prevent.
     */
    NullWorldView view;

    StationTab tabs[MAX_STATION_TABS] = {};
    Assert::AreEqual<std::uint32_t>(0, view.BuildStationTabs(1, tabs), L"no station has any services");

    StationGroup groups[MAX_STATION_GROUPS] = {};
    StationChip chips[MAX_STATION_CHIPS] = {};
    const StationRosterCounts counts =
        view.BuildStationRoster(1, std::span<const std::uint32_t>{}, groups, chips);
    Assert::AreEqual<std::uint32_t>(0, counts.groups);
    Assert::AreEqual<std::uint32_t>(0, counts.chips);

    StationAction actions[MAX_STATION_ACTIONS] = {};
    Assert::AreEqual<std::uint32_t>(0, view.BuildStationActions(1, actions), L"and no action to take there");
    OrderOption formations[MAX_ORDER_OPTIONS] = {};
    Assert::AreEqual<std::uint32_t>(0, view.StationActionOptions(0, formations), L"nor values for its parameter");

    Assert::IsFalse(view.PreCheckStation(StationIntent{}).accepted, L"and nothing is waved through");

    std::array<std::uint8_t, 16> buffer{};
    ByteWriter writer{buffer};
    Assert::IsFalse(view.EncodeStationCommand(StationIntent{}, writer));

    Assert::AreEqual<std::uint32_t>(0, view.ShipsPerStationCommand(), L"zero means none, never means unlimited");
  }

  TEST_METHOD(ItRecordsTheSizeButClaimsNoTick)
  {
    // Not for the game's sake -- there is none -- but so a client running
    // against no world can still show that snapshots are arriving.
    NullWorldView view;
    const std::array<std::uint8_t, 4> payload{1, 2, 3, 4};

    /*
     * False, not true: a view with no world has nowhere to put a frame, and
     * claiming to have taken one would give the clock estimate something to
     * chase that nothing emits.
     */
    ReplicatedFrame frame;
    frame.tick = 77;
    frame.tail = payload;
    Assert::IsFalse(view.ApplyFrame(frame), L"a null view takes no frame");
    Assert::AreEqual<std::uint32_t>(4, view.LastPayloadBytes());
  }
};

} // namespace NeuronClientTests
