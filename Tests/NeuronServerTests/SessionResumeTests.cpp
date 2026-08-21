#include "pch.h"

#include "CppUnitTest.h"

#include "SessionResume.h"

#include <cstdint>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace Neuron;

/*
 * U3c-b: the grace window (ADR-018 D5, build order U3c-b).
 *
 * Every claim here is decidable without a socket, which is why `ResumeTable` is
 * a file of its own rather than three members of `ServerHost` -- the same
 * argument that put `DurableStore` where it is. What the twin-client `selfTest`
 * proves is that the host *calls* this; what these prove is that the answers
 * are right, including the ones a live run would take four minutes to reach.
 */

namespace NeuronServerTests
{

namespace
{
constexpr PlayerId ONE = 1;
constexpr PlayerId TWO = 2;
constexpr std::uint32_t TICK_RATE = 20;
} // namespace

TEST_CLASS(SessionResumeTests)
{
public:
  TEST_METHOD(ADroppedSessionComesBackInsideTheWindow)
  {
    ResumeTable table;
    const std::uint32_t deadline = ResumeDeadlineTick(100, TICK_RATE);
    table.Lapse(ONE, 0xABCDEF0123456789ull, 77, deadline);

    LapsedSession resumed;
    Assert::IsTrue(table.TryResume(ONE, 0xABCDEF0123456789ull, 200, resumed), L"a prompt return was refused");
    Assert::AreEqual<std::uint32_t>(ONE, resumed.playerId);
    Assert::AreEqual<std::uint16_t>(77, resumed.grid, L"a resumed session came back looking somewhere else");
  }

  TEST_METHOD(TheGraceWindowIsTheOneTheDesignNames)
  {
    // 120 seconds at 20 Hz is 2,400 ticks. Asserted against the constant rather
    // than the literal so that changing the window changes one number.
    Assert::AreEqual<std::uint32_t>(SESSION_GRACE_SECONDS * TICK_RATE, ResumeDeadlineTick(0, TICK_RATE));
  }

  TEST_METHOD(ALateReturnIsANewCommander)
  {
    ResumeTable table;
    const std::uint32_t deadline = ResumeDeadlineTick(0, TICK_RATE);
    table.Lapse(ONE, 42, 5, deadline);

    LapsedSession resumed;
    Assert::IsFalse(table.TryResume(ONE, 42, deadline + 1, resumed), L"a session resumed past its deadline");
    Assert::AreEqual<std::size_t>(0, table.Size(), L"the expired row was left behind");
  }

  TEST_METHOD(ArrivingExactlyOnTheDeadlineStillCounts)
  {
    // The boundary is inclusive, and it is asserted because "expired" and
    // "expiring" differ by one tick and a person writing this from the
    // description alone could pick either.
    ResumeTable table;
    const std::uint32_t deadline = ResumeDeadlineTick(0, TICK_RATE);
    table.Lapse(ONE, 42, 5, deadline);

    LapsedSession resumed;
    Assert::IsTrue(table.TryResume(ONE, 42, deadline, resumed), L"the last tick of the window was refused");
  }

  TEST_METHOD(TheTokenIsRequiredAndNotJustTheId)
  {
    /*
     * A `PlayerId` is a small integer anybody can count to. If the id alone
     * resumed a session, taking over the commander who just dropped would be a
     * matter of guessing "1" -- so this is the assertion that the token is
     * load-bearing rather than decorative.
     */
    ResumeTable table;
    table.Lapse(ONE, 0x5EAF00Dull, 5, ResumeDeadlineTick(0, TICK_RATE));

    LapsedSession resumed;
    Assert::IsFalse(table.TryResume(ONE, 1, 10, resumed), L"a guessed token resumed somebody's session");
    Assert::IsFalse(table.TryResume(ONE, 0, 10, resumed), L"a zero token resumed a session");
    Assert::AreEqual<std::size_t>(1, table.Size(), L"a refused attempt consumed the row");

    // And the real one still works afterwards: a failed attempt must not lock
    // the rightful holder out.
    Assert::IsTrue(table.TryResume(ONE, 0x5EAF00Dull, 10, resumed));
  }

  TEST_METHOD(OneCommandersTokenDoesNotOpenAnothersSession)
  {
    ResumeTable table;
    table.Lapse(ONE, 111, 5, ResumeDeadlineTick(0, TICK_RATE));
    table.Lapse(TWO, 222, 9, ResumeDeadlineTick(0, TICK_RATE));

    LapsedSession resumed;
    Assert::IsFalse(table.TryResume(ONE, 222, 10, resumed), L"one commander resumed with another's token");
    Assert::IsTrue(table.TryResume(TWO, 222, 10, resumed));
    Assert::AreEqual<std::uint16_t>(9, resumed.grid, L"the wrong session was handed over");
  }

  TEST_METHOD(AResumedSessionCannotBeResumedTwice)
  {
    // The row is consumed on the way out, so a token observed on the wire is
    // worth one use rather than every use until the window closes.
    ResumeTable table;
    table.Lapse(ONE, 777, 5, ResumeDeadlineTick(0, TICK_RATE));

    LapsedSession resumed;
    Assert::IsTrue(table.TryResume(ONE, 777, 10, resumed));
    Assert::IsFalse(table.TryResume(ONE, 777, 10, resumed), L"a token worked twice");
  }

  TEST_METHOD(ExpirySweepsWhatNobodyCameBackFor)
  {
    ResumeTable table;
    table.Lapse(ONE, 111, 5, 100);
    table.Lapse(TWO, 222, 9, 300);

    table.Expire(150);
    Assert::AreEqual<std::size_t>(1, table.Size(), L"the sweep took the wrong row");
    Assert::AreEqual<std::uint32_t>(TWO, table.Rows()[0].playerId);

    table.Expire(301);
    Assert::AreEqual<std::size_t>(0, table.Size());
  }

  TEST_METHOD(LapsingTwiceKeepsTheNewerRow)
  {
    // A commander has one session. Two rows for one player would mean an older
    // token still worked after a newer one was issued.
    ResumeTable table;
    table.Lapse(ONE, 111, 5, 100);
    table.Lapse(ONE, 222, 9, 300);
    Assert::AreEqual<std::size_t>(1, table.Size(), L"one commander held two lapsed sessions");

    LapsedSession resumed;
    Assert::IsFalse(table.TryResume(ONE, 111, 50, resumed), L"a superseded token still resumed");
    Assert::IsTrue(table.TryResume(ONE, 222, 50, resumed));
  }

  TEST_METHOD(NobodyCannotLapseAndNobodyCannotResume)
  {
    /*
     * `INVALID_PLAYER_ID` is what a zeroed handshake carries. If it could hold
     * a lapsed session, the one identity nobody has to prove would be
     * resumable -- which is the same skeleton-key shape U3c-a refused in the
     * registry's filters.
     */
    ResumeTable table;
    table.Lapse(INVALID_PLAYER_ID, 999, 5, 1000);
    Assert::AreEqual<std::size_t>(0, table.Size(), L"nobody's session was kept");

    LapsedSession resumed;
    Assert::IsFalse(table.TryResume(INVALID_PLAYER_ID, 999, 10, resumed));
  }


  TEST_METHOD(TheDeadlineSaturatesRatherThanWrapping)
  {
    /*
     * The tick is a u32 and rolls over roughly every 2.4 days at 20 Hz, which
     * ADR-018 D5 names. A deadline computed near the top that wrapped past zero
     * would read as already expired -- ending every session on the shard at the
     * instant the counter turned over, which is a failure that would happen
     * once and be very hard to explain afterwards.
     */
    const std::uint32_t nearTheTop = 0xFFFFFFFFu - 10;
    Assert::AreEqual<std::uint32_t>(0xFFFFFFFFu, ResumeDeadlineTick(nearTheTop, TICK_RATE));

    ResumeTable table;
    table.Lapse(ONE, 111, 5, ResumeDeadlineTick(nearTheTop, TICK_RATE));
    LapsedSession resumed;
    Assert::IsTrue(table.TryResume(ONE, 111, nearTheTop + 5, resumed), L"a session near the rollover expired early");
  }
};

} // namespace NeuronServerTests
