#include "pch.h"

// AGENTS.md §4's two defines, spelled here because this project's pch is
// deliberately bare and nothing else in the build supplies them. `NOMINMAX` is
// not optional in this file: without it `min` and `max` are macros and every
// `std::min` below is a syntax error rather than a call.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include "CppUnitTest.h"

#include <windows.h>

#include "ObjMesh.h"
#include "SignalLamp.h"

#include <DirectXMath.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <set>
#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace Neuron;

/*
 * The signal lamps (ADR-006 §6a).
 *
 * Two halves, and the second is the one that earns the file. The first checks
 * the animation modes against hand-worked numbers -- a duty cycle is either
 * right or subtly wrong for months, and no screenshot settles it. The second
 * loads the meshes actually in GameData and asserts that **no lamp is inside
 * the hull it is bolted to**, which is the failure this whole feature is one
 * mistake away from: a glow whose centre is buried is either an explosion or,
 * once the depth test has had it, nothing at all.
 *
 * No device anywhere. Placement is arithmetic over a bounding box, which is
 * exactly why `SignalLamp.h` is free of D3D12.
 */

namespace NeuronClientTests
{

namespace
{

[[nodiscard]] bool IsLampDirectory(const std::string& _path)
{
  const DWORD attributes = GetFileAttributesA(_path.c_str());
  return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

std::string LampTestBinaryDirectory()
{
  HMODULE module = nullptr;
  if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                         reinterpret_cast<LPCSTR>(&IsLampDirectory), &module) == 0)
  {
    return {};
  }

  char path[MAX_PATH] = {};
  const DWORD length = GetModuleFileNameA(module, path, MAX_PATH);
  if (length == 0 || length >= MAX_PATH)
  {
    return {};
  }

  std::string full(path, length);
  const std::size_t slash = full.find_last_of("\\/");
  return slash == std::string::npos ? std::string{} : full.substr(0, slash);
}

/// The content tree, by the same walk `ObjMeshTests` uses and for the same
/// reason: a content test that quietly passes with no content is worse than no
/// test. Deliberately a second copy rather than a shared helper -- the two
/// files are compiled into one binary and a shared one would have to live in a
/// third header for the sake of nine lines.
std::string LampMeshDirectory()
{
  const std::string roots[] = {std::string{}, LampTestBinaryDirectory()};
  for (const std::string& root : roots)
  {
    std::string prefix = root.empty() ? std::string{} : root + "/";
    for (int depth = 0; depth < 8; ++depth)
    {
      const std::string candidate = prefix + "GameData/Meshes";
      if (IsLampDirectory(candidate))
      {
        return candidate;
      }
      prefix += "../";
    }
  }
  Assert::Fail(L"GameData/Meshes was not found from the working directory or the test binary's own");
  return {};
}

/// How far the nearest authored vertex is from a point. Cheap, and enough to
/// say whether the *core* of a glow has clear air around it.
[[nodiscard]] float NearestVertexDistance(const ObjMesh& _mesh, const DirectX::XMFLOAT3& _point)
{
  float nearest = 1e30f;
  for (const MeshVertex& vertex : _mesh.vertices)
  {
    const float dx = vertex.position.x - _point.x;
    const float dy = vertex.position.y - _point.y;
    const float dz = vertex.position.z - _point.z;
    nearest = std::min(nearest, std::sqrt(dx * dx + dy * dy + dz * dz));
  }
  return nearest;
}

/// Möller-Trumbore, counting a hit in the positive direction only. Two-sided,
/// because a crossing count does not care which way a face is wound and the
/// corpus is a union of separately closed pieces rather than one solid.
[[nodiscard]] bool RayHitsTriangle(const DirectX::XMFLOAT3& _origin, const DirectX::XMFLOAT3& _direction,
                                   const DirectX::XMFLOAT3& _a, const DirectX::XMFLOAT3& _b, const DirectX::XMFLOAT3& _c)
{
  const DirectX::XMVECTOR origin = DirectX::XMLoadFloat3(&_origin);
  const DirectX::XMVECTOR direction = DirectX::XMLoadFloat3(&_direction);
  const DirectX::XMVECTOR a = DirectX::XMLoadFloat3(&_a);
  const DirectX::XMVECTOR edge0 = DirectX::XMVectorSubtract(DirectX::XMLoadFloat3(&_b), a);
  const DirectX::XMVECTOR edge1 = DirectX::XMVectorSubtract(DirectX::XMLoadFloat3(&_c), a);

  const DirectX::XMVECTOR pvec = DirectX::XMVector3Cross(direction, edge1);
  const float determinant = DirectX::XMVectorGetX(DirectX::XMVector3Dot(edge0, pvec));
  if (std::fabs(determinant) < 1e-9f)
  {
    return false; // Parallel.
  }

  const float inverse = 1.0f / determinant;
  const DirectX::XMVECTOR tvec = DirectX::XMVectorSubtract(origin, a);
  const float u = DirectX::XMVectorGetX(DirectX::XMVector3Dot(tvec, pvec)) * inverse;
  if (u < 0.0f || u > 1.0f)
  {
    return false;
  }

  const DirectX::XMVECTOR qvec = DirectX::XMVector3Cross(tvec, edge0);
  const float v = DirectX::XMVectorGetX(DirectX::XMVector3Dot(direction, qvec)) * inverse;
  if (v < 0.0f || u + v > 1.0f)
  {
    return false;
  }

  return DirectX::XMVectorGetX(DirectX::XMVector3Dot(edge1, qvec)) * inverse > 1e-4f;
}

/*
 * Is `_point` inside the hull?
 *
 * Ray-crossing parity, voted over six directions. Parity alone would be enough
 * for one closed solid; the corpus is a *union* of separately closed pieces
 * (`Structure.obj` alone is 117 named objects) plus the occasional open panel,
 * and an open panel adds a spurious single crossing along whichever ray happens
 * to meet it. Six rays and a majority make that a wrong vote rather than a
 * wrong answer.
 *
 * This is the test the whole feature turns on: a lamp whose centre is inside a
 * hull is either half-buried -- which reads as an explosion rather than a light
 * -- or entirely gone, because the depth test discards it against the very hull
 * it is bolted to. Nothing about that is visible in a screenshot taken from the
 * one angle somebody happened to check.
 */
[[nodiscard]] bool PointIsInsideMesh(const ObjMesh& _mesh, const DirectX::XMFLOAT3& _point)
{
  const DirectX::XMFLOAT3 directions[] = {
      {1.0f, 0.0f, 0.0f},  {-1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f},
      {0.0f, -1.0f, 0.0f}, {0.0f, 0.0f, 1.0f},  {0.573f, 0.577f, 0.582f}, // The last is skewed off-axis on
                                                                          // purpose: an axis-aligned ray through
                                                                          // an axis-aligned hull grazes edges.
  };

  std::uint32_t insideVotes = 0;
  for (const DirectX::XMFLOAT3& direction : directions)
  {
    std::uint32_t crossings = 0;
    for (std::size_t triangle = 0; triangle + 2 < _mesh.indices.size(); triangle += 3)
    {
      if (RayHitsTriangle(_point, direction, _mesh.vertices[_mesh.indices[triangle]].position,
                          _mesh.vertices[_mesh.indices[triangle + 1]].position,
                          _mesh.vertices[_mesh.indices[triangle + 2]].position))
      {
        ++crossings;
      }
    }
    if ((crossings & 1u) != 0u)
    {
      ++insideVotes;
    }
  }
  return insideVotes > 3;
}

} // namespace

TEST_CLASS(LampAnimationTests)
{
public:
  TEST_METHOD(ASteadyLampIsAlwaysFullyOn)
  {
    std::vector<LampInstance> lamps;
    MeshLampBounds bounds;
    bounds.boundsMin = DirectX::XMFLOAT3{-5.0f, -2.0f, -20.0f};
    bounds.boundsMax = DirectX::XMFLOAT3{5.0f, 3.0f, 20.0f};
    bounds.planeRadiusMetres = 20.0f;
    bounds.deckPortMetres = 2.0f;
    bounds.deckStarboardMetres = 2.0f;
    bounds.deckSpineMetres = 3.0f;
    BuildLampRig(LampRig::Ship, bounds, lamps);
    Assert::AreEqual<std::size_t>(3, lamps.size(), L"a ship carries exactly three lamps");

    // The first two are the navigation lights, and a navigation light that
    // blinked would be a different signal.
    for (std::size_t i = 0; i < 2; ++i)
    {
      for (float t = 0.0f; t < 4.0f; t += 0.13f)
      {
        Assert::AreEqual(1.0f, LampLevel(lamps[i], 0.0f, t), 1e-6f, L"a steady lamp does not vary");
      }
    }
  }

  TEST_METHOD(AStrobeIsOnForEightPercentOfItsCycleAndOffForTheRest)
  {
    LampInstance strobe{};
    strobe.mode = static_cast<float>(LampMode::Strobe);
    strobe.rateHz = 1.0f;
    strobe.dutyFraction = 0.08f;

    // Sampled across exactly one second at 1 Hz, so the lit fraction *is* the
    // duty cycle. A tolerance of one sample, because the boundary lands between
    // two of them.
    constexpr std::uint32_t SAMPLES = 1000;
    std::uint32_t lit = 0;
    for (std::uint32_t i = 0; i < SAMPLES; ++i)
    {
      if (LampLevel(strobe, 0.0f, static_cast<float>(i) / static_cast<float>(SAMPLES)) > 0.5f)
      {
        ++lit;
      }
    }
    Assert::IsTrue(lit >= 79 && lit <= 81, L"a strobe is lit for 8% of its cycle");

    // And it really does go out -- the opacity floor is applied downstream, so
    // the level itself must reach zero or "off" is only dimmer.
    Assert::AreEqual(0.0f, LampLevel(strobe, 0.0f, 0.5f), 1e-6f);
  }

  TEST_METHOD(APulseNeverFallsBelowItsFloorAndReachesFull)
  {
    LampInstance pulse{};
    pulse.mode = static_cast<float>(LampMode::Pulse);
    pulse.rateHz = 0.65f;
    pulse.floorFraction = 0.45f;

    float lowest = 2.0f;
    float highest = -1.0f;
    for (float t = 0.0f; t < 10.0f; t += 0.002f)
    {
      const float level = LampLevel(pulse, 0.0f, t);
      lowest = std::min(lowest, level);
      highest = std::max(highest, level);
    }
    Assert::IsTrue(lowest >= 0.45f - 1e-3f, L"a berth light never goes out");
    Assert::IsTrue(lowest <= 0.46f, L"and it does reach its floor rather than hovering above it");
    Assert::IsTrue(highest >= 0.999f, L"and it reaches full");
  }

  TEST_METHOD(AChaseRunsRoundItsRingRatherThanBlinkingTogether)
  {
    // The station's rim, which is the fixture the mode exists for.
    ObjMesh mesh;
    ObjDiagnostic error;
    Assert::IsTrue(LoadObjMesh(LampMeshDirectory(), "Structure.obj", mesh, error));

    std::vector<LampInstance> lamps;
    BuildLampRig(LampRig::Station, LampBoundsFor(mesh), lamps);

    // Collect the rim lamps -- the white chase ones -- in table order.
    std::vector<const LampInstance*> rim;
    for (const LampInstance& lamp : lamps)
    {
      if (static_cast<LampMode>(static_cast<std::uint8_t>(lamp.mode)) == LampMode::Chase)
      {
        rim.push_back(&lamp);
      }
    }
    Assert::AreEqual<std::size_t>(16, rim.size(), L"sixteen lamps around the platform rim");

    // Each one's phase is its own place in the ring, which is what makes the
    // flash travel instead of the whole ring blinking at once.
    for (std::size_t i = 0; i < rim.size(); ++i)
    {
      Assert::AreEqual(static_cast<float>(i) / 16.0f, rim[i]->phaseTurns, 1e-5f);
    }

    // And the consequence: at any instant at most a couple of them are lit, and
    // over a full cycle every one of them has its turn.
    std::set<std::size_t> everLit;
    std::size_t worstSimultaneous = 0;
    for (float t = 0.0f; t < 4.0f; t += 0.01f)
    {
      std::size_t simultaneous = 0;
      for (std::size_t i = 0; i < rim.size(); ++i)
      {
        if (LampLevel(*rim[i], 0.0f, t) > 0.9f)
        {
          ++simultaneous;
          everLit.insert(i);
        }
      }
      worstSimultaneous = std::max(worstSimultaneous, simultaneous);
    }
    Assert::AreEqual<std::size_t>(16, everLit.size(), L"every lamp in the ring takes its turn");
    Assert::IsTrue(worstSimultaneous <= 4, L"a chase is a travelling flash, not the whole ring blinking");
  }

  TEST_METHOD(NeighbouringHullsDoNotStrobeTogether)
  {
    /*
     * The per-ship phase, which is the reason `InstanceRecord` grew a field.
     *
     * Ids one apart are exactly what a freshly spawned wing is, so what has to
     * hold is that the hash **decorrelates** them -- and that is a statistical
     * claim, not a per-pair one. Two independent uniforms land within 0.02 of
     * each other about 4% of the time, so "no consecutive pair is ever close"
     * is not a property a good hash has; it is a property only a *suspiciously
     * regular* one would have. The first draft of this test asserted exactly
     * that and failed, correctly, on a hash that is fine.
     *
     * So the assertion is on the mean separation, which is 1/3 for independent
     * uniforms and near zero for anything that carries the id through.
     */
    double totalSeparation = 0.0;
    constexpr std::uint32_t SAMPLES = 4096;
    for (std::uint32_t id = 0; id < SAMPLES; ++id)
    {
      const float here = LampPhaseTurns(id);
      Assert::IsTrue(here >= 0.0f && here < 1.0f, L"a phase is a fraction of a turn");
      totalSeparation += std::fabs(static_cast<double>(LampPhaseTurns(id + 1)) - here);
    }
    const double meanSeparation = totalSeparation / SAMPLES;
    Assert::IsTrue(meanSeparation > 0.28,
                   L"consecutive ids must be uncorrelated -- 1/3 is what independent phases give, and a hash that "
                   L"carried the id through would give nearly nothing");

    // And the phase is a property of the ship, not of the moment: the same id
    // must give the same answer for as long as it lives.
    Assert::AreEqual(LampPhaseTurns(4242u), LampPhaseTurns(4242u), 0.0f);

    // Spread, in sixteenths. A hash that clustered would leave buckets empty.
    std::set<int> buckets;
    for (std::uint32_t id = 0; id < 256; ++id)
    {
      buckets.insert(static_cast<int>(LampPhaseTurns(id) * 16.0f));
    }
    Assert::AreEqual<std::size_t>(16, buckets.size(), L"the phases cover the cycle rather than clustering");

    // And the case that actually matters on screen: one wing of eight, spawned
    // together, must not share a blink. Eight phases, no two within a strobe's
    // own on-window of each other -- checked on the real ids rather than on an
    // average, because this is the picture the acceptance describes.
    std::vector<float> wing;
    for (std::uint32_t id = 1; id <= 8; ++id)
    {
      wing.push_back(LampPhaseTurns(id));
    }
    std::sort(wing.begin(), wing.end());
    for (std::size_t i = 1; i < wing.size(); ++i)
    {
      Assert::IsTrue(wing[i] - wing[i - 1] > 0.02f, L"a wing of eight does not strobe as one object");
    }
  }

  TEST_METHOD(BrightnessNeverGoesOutAndSwellsBothWays)
  {
    Assert::AreEqual(0.18f, LampOpacity(0.0f), 1e-6f, L"an unlit lamp is a dim fitting, not a hole");
    Assert::AreEqual(0.80f, LampOpacity(1.0f), 1e-6f);
    Assert::AreEqual(0.72f, LampScale(0.0f), 1e-6f, L"-28% at the bottom");
    Assert::AreEqual(1.28f, LampScale(1.0f), 1e-6f, L"+28% at the top");

    // Clamped rather than extrapolated: a level outside 0..1 cannot make a lamp
    // brighter than the vocabulary allows.
    Assert::AreEqual(0.80f, LampOpacity(4.0f), 1e-6f);
    Assert::AreEqual(0.18f, LampOpacity(-1.0f), 1e-6f);
  }

  TEST_METHOD(LampSizeIsFlooredForStrikeCraftAndCappedForCapitals)
  {
    // Below ~4 m a lamp is sub-pixel at fleet-view zoom; above 14 m it reads as
    // an explosion. Both ends are the point of the formula.
    Assert::AreEqual(3.5f, ShipLampSizeMetres(0.0f), 1e-5f);
    Assert::AreEqual(7.5f, ShipLampSizeMetres(40.0f), 1e-5f);
    Assert::AreEqual(14.0f, ShipLampSizeMetres(105.0f), 1e-5f, L"the cap bites at 105 m of hull");
    Assert::AreEqual(14.0f, ShipLampSizeMetres(1000.0f), 1e-5f, L"and stays bitten");
    Assert::AreEqual(3.5f, ShipLampSizeMetres(-10.0f), 1e-5f, L"a negative length is not a negative lamp");
  }
};

TEST_CLASS(ShippedLampPlacementTests)
{
public:
  TEST_METHOD(NoLampOnAnyShippedHullSitsInsideItsGeometry)
  {
    /*
     * **The acceptance this whole file exists for.**
     *
     * A glow billboard whose centre is inside a hull is either half-buried --
     * which reads as an explosion rather than a lamp -- or gone entirely, since
     * the depth test discards it against the surface it is bolted to. Every
     * anchor in `SignalLamp.cpp` is written to clear the geometry by a multiple
     * of the lamp's own size; this is the check that they actually do, against
     * the meshes as shipped rather than against the ones they were written for.
     *
     * Run at the *drawn* scale, because that is what is on screen: the meshes
     * are fitted to the class table's silhouette radii at load, and a lamp
     * placed correctly on the authored art could still be buried in the fitted
     * art if a fraction had been written as a constant by mistake.
     */
    struct Case
    {
      const char* file;
      LampRig rig;
      float drawnRadiusMetres;
      std::size_t expectedLamps;
    };

    // The radii the composition root supplies, from Game::SilhouetteRadiusMetres.
    const Case cases[] = {
        {"Interceptor.obj", LampRig::Ship, 21.25f, 3},  {"Bomber.obj", LampRig::Ship, 31.25f, 3},
        {"Corvette.obj", LampRig::Ship, 40.0f, 3},      {"Frigate.obj", LampRig::Ship, 62.5f, 3},
        {"Hauler.obj", LampRig::Ship, 81.25f, 3},       {"Miner.obj", LampRig::Ship, 75.0f, 3},
        {"Carrier.obj", LampRig::Ship, 133.75f, 3},     {"Battleship.obj", LampRig::Ship, 150.0f, 3},
        {"Structure.obj", LampRig::Station, 250.0f, 25}, {"Stargate.obj", LampRig::Gate, 168.75f, 10},
    };

    const std::string directory = LampMeshDirectory();
    for (const Case& testCase : cases)
    {
      ObjMesh mesh;
      ObjDiagnostic error;
      const std::wstring name(std::wstring(testCase.file, testCase.file + std::strlen(testCase.file)));
      Assert::IsTrue(LoadObjMesh(directory, testCase.file, mesh, error), name.c_str());
      Assert::IsTrue(FitObjMeshToPlaneRadius(mesh, testCase.drawnRadiusMetres), name.c_str());

      std::vector<LampInstance> lamps;
      BuildLampRig(testCase.rig, LampBoundsFor(mesh), lamps);
      Assert::AreEqual(testCase.expectedLamps, lamps.size(), name.c_str());

      for (const LampInstance& lamp : lamps)
      {
        Assert::IsTrue(lamp.sizeMetres > 0.0f, name.c_str());

        // The hard requirement: the centre is in open space.
        Assert::IsFalse(PointIsInsideMesh(mesh, lamp.localPosition), name.c_str());

        /*
         * And the softer one: the *core* of the glow has clear air around it.
         *
         * A quarter of the lamp's width is half its radius, which is where
         * `LampPS`'s white core has fallen to nothing -- so a lamp that passes
         * this reads as a bulb sitting proud of the hull. It is deliberately
         * not the full radius: the gate's 36 m mouth lamps are mounted on a
         * ring whose studs are 13 m away, by the art direction's own numbers,
         * and their haloes are *supposed* to wash over it.
         */
        const float clearance = NearestVertexDistance(mesh, lamp.localPosition);
        Assert::IsTrue(clearance > 0.25f * lamp.sizeMetres, name.c_str());
      }
    }
  }

  TEST_METHOD(ShipNavigationLampsSitAtTheWidestPointsAboveTheDeck)
  {
    ObjMesh mesh;
    ObjDiagnostic error;
    Assert::IsTrue(LoadObjMesh(LampMeshDirectory(), "Battleship.obj", mesh, error));
    Assert::IsTrue(FitObjMeshToPlaneRadius(mesh, 150.0f));

    std::vector<LampInstance> lamps;
    BuildLampRig(LampRig::Ship, LampBoundsFor(mesh), lamps);
    Assert::AreEqual<std::size_t>(3, lamps.size());

    const LampInstance& port = lamps[0];
    const LampInstance& starboard = lamps[1];
    const LampInstance& dorsal = lamps[2];

    // Port is +x -- OpaqueVS states it, and the bank rotates +x toward +y,
    // which is what lifts the port wing. Red to port, green to starboard, and
    // getting that pair the wrong way round is the one mistake here that looks
    // entirely fine in a screenshot.
    Assert::AreEqual(mesh.boundsMax.x, port.localPosition.x, 1e-3f, L"the port lamp is at the hull's widest point to port");
    Assert::AreEqual(mesh.boundsMin.x, starboard.localPosition.x, 1e-3f);
    Assert::AreEqual(LAMP_RED.y, port.colour.y, 1e-6f, L"port is red");
    Assert::AreEqual(LAMP_GREEN.x, starboard.colour.x, 1e-6f, L"starboard is green");

    /*
     * Above the deck **where the lamp is**, and nowhere near the box's ceiling.
     *
     * This is the regression the whole `deckPortMetres` field exists for. The
     * proposal said "just above bbox top"; on this hull that is 100 m up, set
     * by a thin antenna amidships, while the deck out at the beam is 37 m. A
     * wing lamp anchored to the box hung four lamp-widths clear of the hull in
     * open space -- visibly wrong, and invisible to a "not inside geometry"
     * check. So: comfortably above the local deck, and comfortably *below* the
     * box's ceiling, which is the assertion that fails if anyone puts the
     * simpler rule back.
     */
    const MeshLampBounds bounds = LampBoundsFor(mesh);
    Assert::IsTrue(bounds.deckPortMetres < mesh.boundsMax.y - 10.0f,
                   L"this hull is the interesting case: its box ceiling is well above its beam deck");
    Assert::IsTrue(port.localPosition.y > bounds.deckPortMetres, L"a nav lamp clears the deck it sits on");
    Assert::IsTrue(port.localPosition.y < bounds.deckPortMetres + 14.0f, L"and does not float above it");
    Assert::IsTrue(port.localPosition.y < mesh.boundsMax.y, L"the box ceiling is the antenna, not the deck");
    Assert::IsTrue(starboard.localPosition.y > bounds.deckStarboardMetres);
    Assert::IsTrue(starboard.localPosition.y < mesh.boundsMax.y);
    Assert::IsTrue(dorsal.localPosition.y > bounds.deckSpineMetres, L"the dorsal strobe clears the spine");
    Assert::AreEqual(0.0f, dorsal.localPosition.x, 1e-6f, L"and sits on the centreline");

    // Mid-length for the nav lamps, 55% toward the stern for the strobe. Stern
    // is +z, because forward is -z.
    const float midLength = (mesh.boundsMin.z + mesh.boundsMax.z) * 0.5f;
    Assert::AreEqual(midLength, port.localPosition.z, 1e-3f);
    Assert::IsTrue(dorsal.localPosition.z > midLength, L"the strobe is aft of midships");
  }

  TEST_METHOD(TheStationAndGateRigsAreTheFixturesTheArtDirectionNames)
  {
    ObjMesh station;
    ObjDiagnostic error;
    Assert::IsTrue(LoadObjMesh(LampMeshDirectory(), "Structure.obj", station, error));
    Assert::IsTrue(FitObjMeshToPlaneRadius(station, 250.0f));

    std::vector<LampInstance> stationLamps;
    BuildLampRig(LampRig::Station, LampBoundsFor(station), stationLamps);

    std::size_t chase = 0;
    std::size_t pulse = 0;
    std::size_t strobe = 0;
    for (const LampInstance& lamp : stationLamps)
    {
      switch (static_cast<LampMode>(static_cast<std::uint8_t>(lamp.mode)))
      {
      case LampMode::Chase:
        ++chase;
        break;
      case LampMode::Pulse:
        ++pulse;
        break;
      case LampMode::Strobe:
        ++strobe;
        break;
      default:
        Assert::Fail(L"the station carries no steady lamps");
        break;
      }
    }
    Assert::AreEqual<std::size_t>(16, chase, L"sixteen rim lamps run the approach sequence");
    Assert::AreEqual<std::size_t>(4, pulse, L"one amber pulse per berth");
    Assert::AreEqual<std::size_t>(5, strobe, L"one red strobe per berth, plus the mast beacon");

    // The rim sits inside the platform, the berth strobes out at the boom tips.
    // Both stated as fractions of the silhouette radius, so this is also the
    // check that a retune would move them rather than strand them.
    for (const LampInstance& lamp : stationLamps)
    {
      const float radius = std::sqrt(lamp.localPosition.x * lamp.localPosition.x + lamp.localPosition.z * lamp.localPosition.z);
      Assert::IsTrue(radius <= 251.0f, L"no station lamp reaches past the silhouette");
    }

    // The mast beacon is the topmost thing on the station, above the mast that
    // defines the bounding box's own ceiling.
    float highest = -1e30f;
    for (const LampInstance& lamp : stationLamps)
    {
      highest = std::max(highest, lamp.localPosition.y);
    }
    Assert::IsTrue(highest > station.boundsMax.y, L"the hazard beacon clears the mast");

    ObjMesh gate;
    Assert::IsTrue(LoadObjMesh(LampMeshDirectory(), "Stargate.obj", gate, error));
    Assert::IsTrue(FitObjMeshToPlaneRadius(gate, 168.75f));

    std::vector<LampInstance> gateLamps;
    BuildLampRig(LampRig::Gate, LampBoundsFor(gate), gateLamps);
    Assert::AreEqual<std::size_t>(10, gateLamps.size());

    // Eight around the fore mouth, all at the same depth ahead of the ring, and
    // all the same distance from the ring's axis -- a ring, in other words,
    // which is a thing arithmetic can check and a screenshot cannot.
    const float depth = gateLamps[0].localPosition.z;
    Assert::IsTrue(depth < gate.boundsMin.z + 0.5f * (gate.boundsMax.z - gate.boundsMin.z), L"the mouth ring is forward");
    for (std::size_t i = 0; i < 8; ++i)
    {
      const LampInstance& lamp = gateLamps[i];
      const float ringRadius =
          std::sqrt(lamp.localPosition.x * lamp.localPosition.x + lamp.localPosition.y * lamp.localPosition.y);
      Assert::AreEqual(depth, lamp.localPosition.z, 1e-3f, L"every mouth lamp is at the same depth");
      Assert::AreEqual(81.3f, ringRadius, 0.5f, L"and the same distance from the axis");
      Assert::IsTrue(static_cast<LampMode>(static_cast<std::uint8_t>(lamp.mode)) == LampMode::Chase);
    }

    // The spire strobe above the box's ceiling, the keel pulse below its floor.
    Assert::IsTrue(gateLamps[8].localPosition.y > gate.boundsMax.y, L"the spire strobe clears the spire");
    Assert::IsTrue(gateLamps[9].localPosition.y < gate.boundsMin.y, L"the keel pulse hangs below the spikes");
  }

  TEST_METHOD(AClassWithNoRigGetsNoLampsAndADegenerateMeshIsRefused)
  {
    MeshLampBounds bounds;
    bounds.boundsMin = DirectX::XMFLOAT3{-5.0f, -2.0f, -20.0f};
    bounds.boundsMax = DirectX::XMFLOAT3{5.0f, 3.0f, 20.0f};
    bounds.planeRadiusMetres = 20.0f;

    std::vector<LampInstance> lamps;
    BuildLampRig(LampRig::None, bounds, lamps);
    Assert::AreEqual<std::size_t>(0, lamps.size(), L"a class the game has no opinion about carries no lamps");

    // A mesh with no box has nowhere to hang a lamp, and the honest answer is
    // none rather than one at the origin -- which would be inside whatever is
    // there.
    MeshLampBounds empty;
    BuildLampRig(LampRig::Ship, empty, lamps);
    BuildLampRig(LampRig::Station, empty, lamps);
    BuildLampRig(LampRig::Gate, empty, lamps);
    Assert::AreEqual<std::size_t>(0, lamps.size());
  }
};

} // namespace NeuronClientTests
