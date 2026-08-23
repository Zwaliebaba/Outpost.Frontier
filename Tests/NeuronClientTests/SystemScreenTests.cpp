#include "pch.h"
#include "CppUnitTest.h"

#include "SystemScreen.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <span>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace Neuron;

/*
 * The system view, device-free (U6a, ADR-016 §9/§9b, ADR-020 §5.1/§7).
 *
 * `MapScreenTests.cpp` one level up, and the same division of labour: what a
 * screenshot would catch is U6b's, and this suite is about what a screenshot
 * would *not*. The ring radii, the fan, the shared slot space, the pressable
 * floor over a 13 px reticle and the two verb gates are all arithmetic, and all
 * of them are places where a wrong answer is invisible in a picture and obvious
 * to a finger.
 *
 * **The system here is a fixture, not the bake.** This is an engine suite and
 * may not link GameLogic (CI enforces it), so the ring indices are written by
 * hand -- which is the seam working rather than a shortcut: if these tests
 * needed the universe in order to place a planet, the placement would not be
 * neutral. ADR-016 §9b.1's capacity and §9b.2's site ring are the *game's* to
 * compute, and `Outpost/SelfTest.cpp` proves them against the real bake.
 */

namespace NeuronClientTests
{
namespace
{

constexpr std::uint32_t VIEW_WIDTH = 1440;
constexpr std::uint32_t VIEW_HEIGHT = 900;

[[nodiscard]] SystemScreenLayout Layout(float _scale = 1.0f)
{
  return ResolveSystemScreen(VIEW_WIDTH, VIEW_HEIGHT, _scale, SystemScreenTuning{});
}

/*
 * A system on two rings: five on the inner one and three on the outer, which is
 * the shape the committed bake's smallest systems take once §9b.2 has moved
 * their fields outward.
 */
struct Fixture
{
  std::vector<SystemAnchor> anchors;
  std::vector<SystemBackdrop> backdrop;

  [[nodiscard]] SystemViewData View() const
  {
    SystemViewData data;
    data.label = "KIL-4";
    data.badge = "SEC 0.4";
    data.anchors = anchors;
    data.backdrop = backdrop;
    data.ringCount = 2;
    return data;
  }
};

[[nodiscard]] Fixture MakeFixture()
{
  Fixture fixture;
  for (std::uint16_t slot = 0; slot < 5; ++slot)
  {
    SystemAnchor anchor;
    anchor.id = static_cast<std::uint16_t>(100 + slot);
    anchor.ring = 0;
    anchor.slot = slot;
    anchor.label = "PLACE";
    fixture.anchors.push_back(anchor);
  }
  for (std::uint16_t slot = 0; slot < 3; ++slot)
  {
    SystemAnchor anchor;
    anchor.id = static_cast<std::uint16_t>(200 + slot);
    anchor.ring = 1;
    anchor.slot = slot;
    anchor.label = "FIELD";
    fixture.anchors.push_back(anchor);
  }
  return fixture;
}

[[nodiscard]] float Distance(float _x0, float _y0, float _x1, float _y1)
{
  return std::sqrt((_x1 - _x0) * (_x1 - _x0) + (_y1 - _y0) * (_y1 - _y0));
}

} // namespace

TEST_CLASS(SystemLayoutTests)
{
public:
  /*
   * The zones tile without overlapping, which is the property every screen in
   * this corpus is asserted for first -- a bar that overlapped the disc would
   * put a pressable chip on top of a pressable anchor, and the input router has
   * no way to tell which one the player meant.
   */
  TEST_METHOD(TheZonesTileTheViewport)
  {
    const SystemScreenLayout layout = Layout();

    Assert::IsTrue(layout.topBar.Bottom() <= layout.disc.y + 0.01f, L"the bar overlaps the disc");
    Assert::IsTrue(layout.disc.Right() <= layout.panel.x + 0.01f, L"the disc overlaps the panel");
    Assert::IsTrue(layout.disc.Bottom() <= layout.footer.y + 0.01f, L"the disc overlaps the footer");
    Assert::IsTrue(layout.backChip.Right() <= layout.title.x + 0.01f, L"the back chip overlaps the title");
    Assert::IsTrue(layout.warpHere.Bottom() <= layout.dockHere.y + 0.01f, L"the two verbs overlap");
    Assert::IsTrue(layout.panelBody.Bottom() <= layout.warpHere.y + 0.01f, L"the panel body runs into WARP");
  }

  /*
   * Every pressable thing clears ADR-020's floor at every scale in the range,
   * including the ones where the print's own numbers do not.
   *
   * The back chip's tuning is 46 px, which is *under* the floor at 1x -- so this
   * asserts the floor is enforced rather than that the tuning happens to be
   * large enough. That distinction is the whole reason `TargetHeightPixels`
   * exists.
   */
  TEST_METHOD(EveryTargetClearsTheTouchFloor)
  {
    for (float scale = 0.5f; scale <= 3.01f; scale += 0.25f)
    {
      const SystemScreenLayout layout = Layout(scale);
      Assert::IsTrue(layout.backChip.height >= 48.0f, L"the back chip is under the floor");
      Assert::IsTrue(layout.warpHere.height >= 48.0f, L"WARP HERE is under the floor");
      Assert::IsTrue(layout.dockHere.height >= 48.0f, L"DOCK HERE is under the floor");
    }
  }

  /*
   * The disc is round at any aspect.
   *
   * A system stretched to fill a 21:9 monitor would imply an extent it does not
   * have, which is exactly what ADR-016 §9's *"the layout cannot say distance"*
   * forbids -- so the radius comes from the shorter side, and a wide window
   * gives more margin rather than a wider system.
   */
  TEST_METHOD(TheDiscIsInscribedRatherThanStretched)
  {
    const SystemScreenLayout wide = ResolveSystemScreen(2560, 900, 1.0f, SystemScreenTuning{});
    Assert::IsTrue(wide.radius <= wide.disc.height * 0.5f + 0.01f, L"the disc is taller than its zone");
    Assert::IsTrue(wide.radius <= wide.disc.width * 0.5f + 0.01f, L"the disc is wider than its zone");
    Assert::AreEqual(wide.disc.height * 0.5f, wide.radius, 0.01f, L"a wide window did not use the short side");
  }

  /*
   * A tiny window degrades rather than inverting.
   *
   * `MapScreen`'s habit and the same reason: a zone that went negative at a
   * small client area would draw off-screen instead of failing, and the stated
   * minimum client area is the thing ADR-016 §9's scaling review records as
   * still undocumented.
   */
  TEST_METHOD(ATinyWindowClampsRatherThanInverts)
  {
    const SystemScreenLayout tiny = ResolveSystemScreen(320, 200, 1.0f, SystemScreenTuning{});
    Assert::IsTrue(tiny.disc.width >= 0.0f && tiny.disc.height >= 0.0f, L"the disc inverted");
    Assert::IsTrue(tiny.panel.width >= 0.0f && tiny.panelBody.height >= 0.0f, L"the panel inverted");
    Assert::IsTrue(tiny.radius >= 0.0f, L"the radius went negative");
    Assert::IsTrue(SystemRingRadius(0, 3, tiny, 1.0f, SystemScreenTuning{}) >= 0.0f, L"a ring radius went negative");
  }
};

TEST_CLASS(SystemRingTests)
{
public:
  /*
   * Adding a ring moves them all inward rather than pushing the outer one off
   * the screen, which is what makes ADR-016 §9b.1's overflow safe: a system
   * that grows a third ring gets tighter, never clipped.
   */
  TEST_METHOD(GrowingARingTightensRatherThanClips)
  {
    const SystemScreenLayout layout = Layout();
    const SystemScreenTuning tuning;
    const float outer = layout.radius * tuning.outerRingFraction;

    for (std::uint16_t count = 1; count <= 6; ++count)
    {
      float previous = -1.0f;
      for (std::uint16_t ring = 0; ring < count; ++ring)
      {
        const float radius = SystemRingRadius(ring, count, layout, 1.0f, tuning);
        Assert::IsTrue(radius > previous, L"a ring did not sit outside the one before it");
        Assert::IsTrue(radius <= outer + 0.01f, L"a ring escaped the disc");
        previous = radius;
      }
    }

    // And the tightening, stated as the comparison it actually is: the second
    // ring of a three-ring system sits inside the second ring of a two-ring one.
    const float ofTwo = SystemRingRadius(1, 2, layout, 1.0f, tuning);
    const float ofThree = SystemRingRadius(1, 3, layout, 1.0f, tuning);
    Assert::IsTrue(ofThree < ofTwo, L"a third ring did not tighten the second");
  }

  /*
   * A lone ring sits at the first radius rather than at the rim.
   *
   * The committed bake never produces one -- every system has fields, so §9b.2
   * guarantees a second ring -- but a bake without sites does, and a five-anchor
   * system drawn as a hoop around an empty middle is the picture this avoids.
   */
  TEST_METHOD(ALoneRingSitsInsideRatherThanAtTheRim)
  {
    const SystemScreenLayout layout = Layout();
    const SystemScreenTuning tuning;
    Assert::AreEqual(tuning.firstRingRadius, SystemRingRadius(0, 1, layout, 1.0f, tuning), 0.01f,
                     L"a lone ring was not at the first radius");
    Assert::IsTrue(SystemRingRadius(0, 1, layout, 1.0f, tuning) < layout.radius * tuning.outerRingFraction,
                   L"a lone ring reached the rim");
  }

  /*
   * The fan starts from a fixed bearing and runs the same way every time.
   *
   * §9b.1 orders the rings by bake order *precisely* so a player learns where
   * things are, and a fan whose first slot landed wherever the arithmetic
   * happened to put it would spend that. Slot zero is straight up.
   */
  TEST_METHOD(SlotZeroIsStraightUp)
  {
    const SystemScreenLayout layout = Layout();
    const Fixture fixture = MakeFixture();
    std::array<SystemAnchorRect, MAX_SYSTEM_ANCHORS> anchors{};
    std::array<SystemBackdropRect, MAX_SYSTEM_BACKDROP> backdrop{};
    const SystemPlacement placed =
      PlaceSystemView(fixture.View(), layout, 1.0f, SystemScreenTuning{}, anchors, backdrop);
    Assert::AreEqual(std::uint32_t{8}, placed.anchors, L"not everything was placed");

    Assert::AreEqual(layout.centreX, anchors[0].x, 0.01f, L"slot zero was not straight up");
    Assert::IsTrue(anchors[0].y < layout.centreY, L"slot zero was below the centre");

    // And the next slot is clockwise from it, which on a y-down plane is a
    // larger x.
    Assert::IsTrue(anchors[1].x > anchors[0].x, L"the fan did not run clockwise");
  }

  /*
   * Everything on a ring is the same distance from the centre, and the two rings
   * are different distances.
   *
   * This is *"rings are placed, never measured"* spent: the radius is a function
   * of the ring index and nothing else, so two systems with different real
   * extents look alike and neither implies a scale.
   */
  TEST_METHOD(ARingIsOneRadius)
  {
    const SystemScreenLayout layout = Layout();
    const Fixture fixture = MakeFixture();
    std::array<SystemAnchorRect, MAX_SYSTEM_ANCHORS> anchors{};
    std::array<SystemBackdropRect, MAX_SYSTEM_BACKDROP> backdrop{};
    (void)PlaceSystemView(fixture.View(), layout, 1.0f, SystemScreenTuning{}, anchors, backdrop);

    const float inner = Distance(layout.centreX, layout.centreY, anchors[0].x, anchors[0].y);
    for (std::size_t index = 1; index < 5; ++index)
    {
      Assert::AreEqual(inner, Distance(layout.centreX, layout.centreY, anchors[index].x, anchors[index].y), 0.05f,
                       L"an inner-ring anchor sat at a different radius");
    }
    const float outer = Distance(layout.centreX, layout.centreY, anchors[5].x, anchors[5].y);
    for (std::size_t index = 6; index < 8; ++index)
    {
      Assert::AreEqual(outer, Distance(layout.centreX, layout.centreY, anchors[index].x, anchors[index].y), 0.05f,
                       L"an outer-ring anchor sat at a different radius");
    }
    Assert::IsTrue(outer > inner, L"the outer ring was not outside the inner one");
  }

  /*
   * A ring is fanned by everything on it, so a backdrop body never lands on an
   * anchor.
   *
   * The two lists exist to separate what may be *pressed*; they do not separate
   * what takes up room on a ring. A fan that counted only the anchors would put
   * every moon exactly under a planet -- drawn, invisible, and unclickable for a
   * reason the player cannot see.
   */
  TEST_METHOD(BackdropSharesTheRingsSlotSpace)
  {
    const SystemScreenLayout layout = Layout();
    Fixture fixture = MakeFixture();
    fixture.backdrop.push_back(SystemBackdrop{0, 5, 128});
    fixture.backdrop.push_back(SystemBackdrop{0, 6, 200});

    std::array<SystemAnchorRect, MAX_SYSTEM_ANCHORS> anchors{};
    std::array<SystemBackdropRect, MAX_SYSTEM_BACKDROP> backdrop{};
    const SystemPlacement placed =
      PlaceSystemView(fixture.View(), layout, 1.0f, SystemScreenTuning{}, anchors, backdrop);
    Assert::AreEqual(std::uint32_t{2}, placed.backdrop, L"the scenery was not placed");

    for (std::uint32_t body = 0; body < placed.backdrop; ++body)
    {
      for (std::uint32_t anchor = 0; anchor < placed.anchors; ++anchor)
      {
        Assert::IsTrue(Distance(backdrop[body].x, backdrop[body].y, anchors[anchor].x, anchors[anchor].y) > 1.0f,
                       L"a backdrop body landed on an anchor");
      }
    }

    // Still on ring 0's radius, which is the other half of the claim: sharing
    // the slot space must not move a body to another ring.
    const float ring = Distance(layout.centreX, layout.centreY, anchors[0].x, anchors[0].y);
    Assert::AreEqual(ring, Distance(layout.centreX, layout.centreY, backdrop[0].x, backdrop[0].y), 0.05f,
                     L"a backdrop body left its ring");
  }

  /// A body draws at its ratio, which is the only thing the seam says about how
  /// big a moon is -- and it says it as a ratio because a radius would be a
  /// distance and this surface has none.
  TEST_METHOD(ABodyDrawsAtItsRatio)
  {
    const SystemScreenLayout layout = Layout();
    Fixture fixture = MakeFixture();
    fixture.backdrop.push_back(SystemBackdrop{0, 5, 128});
    fixture.backdrop.push_back(SystemBackdrop{0, 6, 255});

    std::array<SystemAnchorRect, MAX_SYSTEM_ANCHORS> anchors{};
    std::array<SystemBackdropRect, MAX_SYSTEM_BACKDROP> backdrop{};
    (void)PlaceSystemView(fixture.View(), layout, 1.0f, SystemScreenTuning{}, anchors, backdrop);
    Assert::IsTrue(backdrop[1].radius > backdrop[0].radius, L"a larger ratio did not draw larger");
  }

  /*
   * The count chip exists only where the player has ships.
   *
   * §9b.3 puts two numbers on one marker, and a marker with no ships is not a
   * marker with a zero on it -- a caller that forgot to check should draw
   * nothing rather than a chip at the origin.
   */
  TEST_METHOD(TheCountChipExistsOnlyWhereShipsAre)
  {
    const SystemScreenLayout layout = Layout();
    Fixture fixture = MakeFixture();
    fixture.anchors[2].shipCount = 4;
    fixture.anchors[6].dockedCount = 9;

    std::array<SystemAnchorRect, MAX_SYSTEM_ANCHORS> anchors{};
    std::array<SystemBackdropRect, MAX_SYSTEM_BACKDROP> backdrop{};
    const SystemPlacement placed =
      PlaceSystemView(fixture.View(), layout, 1.0f, SystemScreenTuning{}, anchors, backdrop);

    for (std::uint32_t index = 0; index < placed.anchors; ++index)
    {
      const bool wanted = index == 2 || index == 6;
      const bool drawn = anchors[index].countBadge.width > 0.0f;
      Assert::AreEqual(wanted, drawn, L"a count chip was drawn where it should not be, or missing where it should");
    }

    // Docked-only too: the split is two numbers, and either one alone is still
    // "the player has ships here".
    Assert::IsTrue(anchors[6].countBadge.width > 0.0f, L"a docked-only anchor drew no chip");
  }

  /// The second line exists only where the game wrote one -- §9b.4's far-side
  /// name for a gate, and nothing at all for a planet.
  TEST_METHOD(TheSecondLineExistsOnlyWhereTheGameWroteOne)
  {
    const SystemScreenLayout layout = Layout();
    Fixture fixture = MakeFixture();
    fixture.anchors[1].detail = "\xE2\x86\x92 KIL-7";

    std::array<SystemAnchorRect, MAX_SYSTEM_ANCHORS> anchors{};
    std::array<SystemBackdropRect, MAX_SYSTEM_BACKDROP> backdrop{};
    (void)PlaceSystemView(fixture.View(), layout, 1.0f, SystemScreenTuning{}, anchors, backdrop);

    Assert::IsTrue(anchors[1].detail.height > 0.0f, L"a gate's far-side line was not laid out");
    Assert::IsTrue(anchors[1].detail.y >= anchors[1].label.Bottom(), L"the second line is not under the first");
    Assert::AreEqual(0.0f, anchors[0].detail.height, 0.01f, L"a planet was given a second line");
  }

  /// A span too small is filled and the remainder dropped, which the caller is
  /// expected to notice rather than to discover.
  TEST_METHOD(ASpanTooSmallIsFilledRatherThanOverrun)
  {
    const SystemScreenLayout layout = Layout();
    const Fixture fixture = MakeFixture();
    std::array<SystemAnchorRect, 3> anchors{};
    std::array<SystemBackdropRect, 1> backdrop{};
    const SystemPlacement placed =
      PlaceSystemView(fixture.View(), layout, 1.0f, SystemScreenTuning{}, anchors, backdrop);
    Assert::AreEqual(std::uint32_t{3}, placed.anchors, L"a short span was not filled to its length");
  }
};

TEST_CLASS(SystemHitTests)
{
public:
  /*
   * A press picks the nearest anchor, never the first -- and this is not a
   * theoretical hazard on this screen, which is the point of the scale it is
   * asserted at.
   *
   * **ADR-020's 48 px floor does not scale, and the rings do.** A full ring of
   * eight sits 59.7 px apart at 1x, which the 48 px target just clears; at 0.75
   * they are 44.8 px apart and at 0.5 they are 29.8, so below about 0.8 every
   * neighbouring pair on a full inner ring is inside one finger. That is the
   * floor doing exactly what it is for and it is why nearest-wins is
   * load-bearing here: first-found would let bake order decide which anchor a
   * tap selects, on the screen where a tap is a warp.
   */
  TEST_METHOD(APressPicksTheNearestNotTheFirst)
  {
    constexpr float SCALE = 0.5f;
    const SystemScreenLayout layout = Layout(SCALE);
    const SystemScreenTuning tuning;

    // A ring at §9b.1's capacity, which is where neighbours crowd.
    std::vector<SystemAnchor> full;
    for (std::uint16_t slot = 0; slot < 8; ++slot)
    {
      SystemAnchor anchor;
      anchor.id = static_cast<std::uint16_t>(300 + slot);
      anchor.ring = 0;
      anchor.slot = slot;
      anchor.label = "PLACE";
      full.push_back(anchor);
    }
    SystemViewData data;
    data.anchors = full;
    data.ringCount = 2;

    std::array<SystemAnchorRect, MAX_SYSTEM_ANCHORS> anchors{};
    std::array<SystemBackdropRect, MAX_SYSTEM_BACKDROP> backdrop{};
    const SystemPlacement placed = PlaceSystemView(data, layout, SCALE, tuning, anchors, backdrop);
    const std::span<const SystemAnchorRect> hits{anchors.data(), placed.anchors};

    // The premise, asserted rather than assumed: the two are genuinely inside
    // one target, so this test is about the tie-break and not about the reach.
    const float separation = Distance(anchors[0].x, anchors[0].y, anchors[1].x, anchors[1].y);
    Assert::IsTrue(separation < TargetHeightPixels(tuning.anchorRadius * 2.0f, SCALE),
                   L"the fixture's neighbours do not overlap, so this proves nothing");

    const SystemAnchorRect* exact = HitSystemAnchor(hits, anchors[2].x, anchors[2].y, SCALE, tuning);
    Assert::IsNotNull(exact, L"a press on an anchor hit nothing");
    if (exact != nullptr)
    {
      Assert::AreEqual(std::uint32_t{2}, exact->anchor, L"a press dead on an anchor took a neighbour");
    }

    // Three-fifths of the way from the first to the second resolves to the
    // second, even though the first is earlier in the list and also in range.
    const float x = anchors[0].x + (anchors[1].x - anchors[0].x) * 0.6f;
    const float y = anchors[0].y + (anchors[1].y - anchors[0].y) * 0.6f;
    Assert::IsTrue(Distance(x, y, anchors[0].x, anchors[0].y) < TargetHeightPixels(tuning.anchorRadius * 2.0f, SCALE) *
                                                                 0.5f,
                   L"the earlier anchor is out of range, so first-found would agree anyway");

    const SystemAnchorRect* nearer = HitSystemAnchor(hits, x, y, SCALE, tuning);
    Assert::IsNotNull(nearer, L"a press between two anchors hit nothing");
    if (nearer != nullptr)
    {
      Assert::AreEqual(std::uint32_t{1}, nearer->anchor, L"a press between two anchors took the earlier one");
    }
  }

  /// The star is drawn rather than offered: a press on the centre of the disc
  /// hits nothing, because the star is not in the list that can be hit.
  TEST_METHOD(TheStarIsNotPressable)
  {
    const SystemScreenLayout layout = Layout();
    const Fixture fixture = MakeFixture();
    std::array<SystemAnchorRect, MAX_SYSTEM_ANCHORS> anchors{};
    std::array<SystemBackdropRect, MAX_SYSTEM_BACKDROP> backdrop{};
    const SystemPlacement placed =
      PlaceSystemView(fixture.View(), layout, 1.0f, SystemScreenTuning{}, anchors, backdrop);
    Assert::IsNull(HitSystemAnchor({anchors.data(), placed.anchors}, layout.centreX, layout.centreY, 1.0f,
                                   SystemScreenTuning{}),
                   L"the star was pressable");
  }

  /*
   * The pressable area is the floored target rather than the drawn reticle.
   *
   * A 13 px reticle at 1x is a quarter of what a finger needs; a press 20 px off
   * centre is outside the mark, well inside the target, and must land.
   */
  TEST_METHOD(ThePressableAreaIsWiderThanTheMark)
  {
    const SystemScreenLayout layout = Layout();
    const Fixture fixture = MakeFixture();
    const SystemScreenTuning tuning;
    std::array<SystemAnchorRect, MAX_SYSTEM_ANCHORS> anchors{};
    std::array<SystemBackdropRect, MAX_SYSTEM_BACKDROP> backdrop{};
    const SystemPlacement placed = PlaceSystemView(fixture.View(), layout, 1.0f, tuning, anchors, backdrop);
    const std::span<const SystemAnchorRect> hits{anchors.data(), placed.anchors};

    Assert::IsNotNull(HitSystemAnchor(hits, anchors[0].x, anchors[0].y - 20.0f, 1.0f, tuning),
                      L"a press outside the reticle and inside the target missed");
    Assert::IsNull(HitSystemAnchor(hits, anchors[0].x, anchors[0].y - 80.0f, 1.0f, tuning),
                   L"a press well outside the target still hit");
  }

  /*
   * The two verbs are gated independently, which is `HitMapAction`'s ruling one
   * surface up: a fleet can warp to a planet it cannot dock at, and one flag for
   * both would offer DOCK on open space.
   */
  TEST_METHOD(TheTwoVerbsAreGatedSeparately)
  {
    const SystemScreenLayout layout = Layout();
    const float warpX = layout.warpHere.x + layout.warpHere.width * 0.5f;
    const float warpY = layout.warpHere.y + layout.warpHere.height * 0.5f;
    const float dockX = layout.dockHere.x + layout.dockHere.width * 0.5f;
    const float dockY = layout.dockHere.y + layout.dockHere.height * 0.5f;

    Assert::IsTrue(HitSystemAction(layout, true, true, warpX, warpY) == SystemActionHit::WarpHere, L"WARP did not hit");
    Assert::IsTrue(HitSystemAction(layout, true, true, dockX, dockY) == SystemActionHit::DockHere, L"DOCK did not hit");

    Assert::IsTrue(HitSystemAction(layout, false, true, warpX, warpY) == SystemActionHit::None,
                   L"a dark WARP still answered");
    Assert::IsTrue(HitSystemAction(layout, false, true, dockX, dockY) == SystemActionHit::DockHere,
                   L"a dark WARP took DOCK with it");
    Assert::IsTrue(HitSystemAction(layout, true, false, dockX, dockY) == SystemActionHit::None,
                   L"a dark DOCK still answered");
    Assert::IsTrue(HitSystemAction(layout, true, false, warpX, warpY) == SystemActionHit::WarpHere,
                   L"a dark DOCK took WARP with it");

    Assert::IsTrue(HitSystemAction(layout, true, true, layout.centreX, layout.centreY) == SystemActionHit::None,
                   L"a press on the disc answered a verb");
  }

  /*
   * An empty system draws its chrome and nothing else.
   *
   * A view with no universe answers false at the seam and the screen opens
   * anyway, so this is the state it opens in -- and it must not divide by a ring
   * count it does not have.
   */
  TEST_METHOD(AnEmptySystemPlacesNothingAndDoesNotDivide)
  {
    const SystemScreenLayout layout = Layout();
    const SystemViewData empty;
    std::array<SystemAnchorRect, MAX_SYSTEM_ANCHORS> anchors{};
    std::array<SystemBackdropRect, MAX_SYSTEM_BACKDROP> backdrop{};
    const SystemPlacement placed = PlaceSystemView(empty, layout, 1.0f, SystemScreenTuning{}, anchors, backdrop);
    Assert::AreEqual(std::uint32_t{0}, placed.anchors, L"an empty system placed an anchor");
    Assert::AreEqual(std::uint32_t{0}, placed.backdrop, L"an empty system placed a body");
    Assert::IsNull(HitSystemAnchor({anchors.data(), placed.anchors}, layout.centreX, layout.centreY, 1.0f,
                                   SystemScreenTuning{}),
                   L"an empty system answered a press");
  }
};

} // namespace NeuronClientTests
