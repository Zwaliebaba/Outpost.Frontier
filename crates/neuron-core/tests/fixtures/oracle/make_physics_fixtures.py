"""Writes the oracle inputs of work package P2.3 (physics) in physics/.

The inputs are synthetic. Run it with Python 3 from any directory; it
rewrites every file in physics/. The oracle harness (CppSource/Oracle,
oracle_physics.cpp) and the Rust test tests/oracle_physics.rs read the same
files and must produce byte-identical vectors.

Every number is a bit pattern in lower-case hex: an f32 in eight digits, an
f64 in sixteen. Tokens are separated by single spaces.

- physics/collision_a.ini, collision_b.ini
                     [CollisionConsts] tables for Collision::Load
- physics/damage.txt Collision's damage functions, one call per line:
    load <file>                    Collision::Free, then Collision::Load
                                   on every header of the file
    type <u32>                     GetImpactDamageFromType
    calc <u32> <rv f32 x3> <n f32 x3>
                                   CalcCollisionDamage(type, rel vel, normal)
    scaled <cdf f32> <factor f32> <max f32> <actual f32>
                                   COLLISION_DAMAGE_FACTOR = cdf, then
                                   CalcCollisionDamage(factor, max, actual)
- physics/drv.txt    CDeadReckonedVector call sequences:
    seq <t f64> <p f32 x3>         a new object; t and p are the arguments of
                                   the ProjectVelocity and CalcError probes
                                   dumped after every call of the sequence
    add <pos f32 x3> <t f64>       AddSample
    doit <dt f64>                  Doit
    maxc <t f64>                   SetMaxCorrectiveConvergeTime
    reset | normp | normt          Reset, NormalizeProjectedPosition,
                                   NormalizeTrackedPosition
- physics/remote.txt CRemotePhysicsSimulation call sequences:
    seq                            a new object
    update <pos f32 x3> <q f32 x4 (w x y z)> <t f64>
                                   RemoteUpdate
    doit <dt f64>                  Doit
    sync <p1 f32 x3> <p2 f32 x3> <q f32 x4>
                                   CheckForSync
    reset                          Reset

The sequences avoid NaN inputs (NaN payloads are not part of the port) and
the degenerate orientations for which Doit's Gram-Schmidt divides by zero.
"""

import math
import os
import random
import struct

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, 'physics')

rng = random.Random(0x0F15_2303)


def f32(value):
    return '%08x' % struct.unpack('<I', struct.pack('<f', value))[0]


def f64(value):
    return '%016x' % struct.unpack('<Q', struct.pack('<d', value))[0]


def vec(x, y, z):
    return ' '.join(f32(v) for v in (x, y, z))


def write(name, lines, header):
    os.makedirs(OUT, exist_ok=True)
    text = '\n'.join(['# ' + line for line in header] + lines) + '\n'
    with open(os.path.join(OUT, name), 'wb') as f:
        f.write(text.encode())


def write_raw(name, text):
    os.makedirs(OUT, exist_ok=True)
    with open(os.path.join(OUT, name), 'wb') as f:
        f.write(text.encode())


# --- Collision damage -------------------------------------------------------

# DecodeType bits (common_archetype.cpp) of the names the tables use.
FIGHTER = 0x10000
ASTEROID = 0x20000000
MOON = 0x1
PLANET = 0x2
CRUISER = 0x80000
SUN = 0x4
GUNBOAT = 0x40000
TYPES = [FIGHTER, ASTEROID, MOON, PLANET, CRUISER, SUN, GUNBOAT, 0x8, 0, 0xffffffff]

# The first value of a repeated type stays; unknown names and NONE decode to
# 0 and are skipped; [Other] is refused; a damage line without an amount
# reads 0; the second [CollisionConsts] adds to the same table.
COLLISION_A = """[CollisionConsts]
damage = FIGHTER, 100
damage = asteroid, 50.5
damage = FIGHTER, 999
damage = nonsense, 7
damage = MOON, 0.125
damage = NONE, 5
damage = CRUISER
max_impact_speed = 200
[Other]
damage = PLANET, 3
[CollisionConsts]
damage = PLANET, 12.75
"""

# A zero impact speed (the ratio becomes infinite), a huge and a negative
# amount.
COLLISION_B = """[CollisionConsts]
damage = Sun, 1e30
damage = GUNBOAT, -40
damage = fighter, 3
max_impact_speed = 0
"""

COLLISION_C = """[CollisionConsts]
damage = FIGHTER, 75
damage = MOON, 1
max_impact_speed = 333.3
"""

NAN = float('nan')
INF = float('inf')


def damage_lines():
    edge_normals = [
        (0.0, 0.0, 0.0),
        (-0.0, 0.0, -0.0),
        (NAN, 0.0, 0.0),
        (0.0, NAN, 1.0),
        (1.0, 0.0, NAN),
        (-1.0, 0.0, 0.0),
        (0.0, -2.0, 0.0),
        (0.0, 0.0, -0.5),
        (-0.6, -0.8, 0.0),
        (1.0, 0.0, 0.0),
        (0.0, 1.0, 0.0),
        (0.0, 0.0, 1.0),
        (3.0, 4.0, 12.0),
        (1e-20, 0.0, 0.0),
        (1e-30, 1e-30, 1e-30),
        (1e30, 0.0, 0.0),
        (3e38, 3e38, 3e38),
        (INF, 0.0, 0.0),
    ]
    edge_vels = [
        (0.0, 0.0, 0.0),
        (5.0, -30.0, 1.0),
        (-120.0, 45.5, 300.0),
        (1e30, -1e30, 1e30),
        (INF, 0.0, 0.0),
        (NAN, 0.0, 0.0),
        (0.1, 0.2, 0.3),
    ]

    def calc(type_id, vel, normal):
        return 'calc %08x %s %s' % (type_id, vec(*vel), vec(*normal))

    def random_vec(scale):
        return tuple(rng.uniform(-scale, scale) for _ in range(3))

    lines = []
    # Before any load: the default speed (250) and an empty table.
    lines.append('type %08x' % FIGHTER)
    lines.append(calc(FIGHTER, (5.0, -30.0, 1.0), (0.0, 2.0, 0.0)))
    lines.append(calc(FIGHTER, (INF, 0.0, 0.0), (1.0, 0.0, 0.0)))
    for table in ('collision_a.ini', 'collision_b.ini', 'collision_c.ini'):
        lines.append('load physics/' + table)
        for type_id in TYPES:
            lines.append('type %08x' % type_id)
        for type_id in (FIGHTER, ASTEROID, MOON, PLANET, CRUISER, 0x8):
            for normal in edge_normals:
                lines.append(calc(type_id, edge_vels[1], normal))
            for vel in edge_vels:
                lines.append(calc(type_id, vel, (0.0, -1.0, 0.0)))
                lines.append(calc(type_id, vel, (0.3, 0.4, -1.2)))
        for type_id in (SUN, GUNBOAT):
            for vel in edge_vels[:4]:
                lines.append(calc(type_id, vel, (0.0, 0.0, 2.0)))
        for _ in range(40):
            type_id = rng.choice((FIGHTER, ASTEROID, MOON, PLANET))
            normal = random_vec(rng.choice((1.0, 3.0, 1000.0)))
            lines.append(calc(type_id, random_vec(rng.choice((10.0, 400.0))), normal))

    def scaled(cdf, factor, max_speed, actual):
        return 'scaled %s %s %s %s' % (f32(cdf), f32(factor), f32(max_speed), f32(actual))

    for cdf in (0.25, 0.3, 0.0, -1.0, 1e30):
        for factor, max_speed, actual in (
                (400.0, 100.0, 50.0),
                (400.0, 100.0, 150.0),
                (400.0, 100.0, 100.0),
                (0.7, 3.0, 1.0),
                (0.7, 3.0, 3.0),
                (1.0, NAN, 1.0),
                (1.0, 1.0, NAN),
                (1.0, 0.0, 0.0),
                (1.0, 0.0, -1.0),
                (1.0, -5.0, -10.0),
                (1.0, INF, 1.0),
                (1.0, 1e-30, 0.0),
                (-2.5, 80.0, 79.99),
                (1e30, 2.0, 1.0)):
            lines.append(scaled(cdf, factor, max_speed, actual))
    for _ in range(30):
        lines.append(scaled(rng.choice((0.25, 0.1, 2.0)), rng.uniform(0.0, 5000.0),
                            rng.uniform(0.0, 300.0), rng.uniform(0.0, 300.0)))
    return lines


# --- CDeadReckonedVector ----------------------------------------------------


class Drv:
    def __init__(self, lines, t=0.5, p=(1.0, 2.0, 3.0)):
        self.lines = lines
        lines.append('seq %s %s' % (f64(t), vec(*p)))

    def add(self, pos, t):
        self.lines.append('add %s %s' % (vec(*pos), f64(t)))

    def doit(self, dt):
        self.lines.append('doit %s' % f64(dt))

    def maxc(self, t):
        self.lines.append('maxc %s' % f64(t))

    def op(self, name):
        self.lines.append(name)


def circle(radius, degrees, centre=(0.0, 0.0, 0.0), tilt=0.0):
    a = math.radians(degrees)
    x = radius * math.cos(a)
    y = radius * math.sin(a)
    return (centre[0] + x, centre[1] + y * math.cos(tilt), centre[2] + y * math.sin(tilt))


def drv_lines():
    lines = []

    # One sample: no velocity, no window.
    d = Drv(lines)
    d.add((1.0, 2.0, 3.0), 10.0)
    d.doit(0.1)
    d.doit(0.2)
    d.op('normp')
    d.op('normt')

    # Two samples on a line: dt12 is zero, the secant capped at 0.25.
    d = Drv(lines, 0.25, (15.0, 3.0, 4.0))
    d.add((0.0, 0.0, 0.0), 1.0)
    d.add((10.0, 0.0, 0.0), 2.0)
    for _ in range(3):
        d.doit(0.1)
    d.doit(0.5)

    # A parabola x = t^2: the direction reverses, so the quadratic fit runs.
    d = Drv(lines, 1.0, (16.0, 0.0, 0.0))
    d.add((0.0, 0.0, 0.0), 10.0)
    d.add((1.0, 0.0, 0.0), 11.0)
    d.add((4.0, 0.0, 0.0), 12.0)
    d.doit(0.5)
    d.doit(0.6)
    d.add((9.0, 0.0, 0.0), 13.0)
    d.doit(0.25)
    d.doit(1.0)

    # Equal time stamps: dt01 zero, no velocity, no blend.
    d = Drv(lines, 0.1, (0.0, 0.0, 0.0))
    d.add((1.0, 1.0, 1.0), 5.0)
    d.add((2.0, 1.0, 1.0), 5.0)
    d.add((3.0, 1.5, 1.0), 5.0)
    d.doit(0.05)
    d.add((4.0, 2.0, 1.0), 5.5)
    d.doit(0.05)
    d.add((5.0, 2.0, 1.0), 5.5)
    d.doit(0.3)

    # Reversal (the newest leg turns back): the secant fallback with the
    # window cap; then a larger cap.
    d = Drv(lines, 0.75, (2.0, 0.5, 0.0))
    d.add((0.0, 0.0, 0.0), 0.0)
    d.add((10.0, 0.0, 0.0), 1.0)
    d.add((0.0, 0.0, 0.0), 2.0)
    d.doit(0.1)
    d.doit(0.3)
    d.maxc(1.5)
    d.add((5.0, 1.0, 0.0), 4.0)
    d.doit(1.0)
    d.doit(1.0)
    d.add((5.0, 1.0, 0.0), 4.5)
    d.doit(0.2)

    # A near-straight path (1 degree per step on a circle): the fit runs and
    # the blend is a straight line.
    d = Drv(lines, 0.05, (100.0, 3.0, 0.0))
    t = 0.0
    for step in range(6):
        d.add(circle(100.0, step * 1.0), t)
        d.doit(0.04)
        d.doit(0.04)
        t += 0.1

    # A curved path (30 degrees per step): the fit and the quadratic blend,
    # with the time since the last sample shorter and longer than the gap.
    d = Drv(lines, 0.2, (40.0, 10.0, -5.0))
    t = 3.0
    for step, waits in enumerate(([0.1], [0.2, 0.15], [0.05], [0.3, 0.3], [0.12], [0.02, 0.5])):
        d.add(circle(50.0, step * 30.0, (5.0, -3.0, 2.0), 0.4), t)
        for wait in waits:
            d.doit(wait)
        t += 0.25 + 0.05 * step

    # Reset mid-way: the samples stay, Doit with no samples projects from
    # the old newest one, the next sample refills all three.
    d = Drv(lines, 0.3, (1.0, 1.0, 1.0))
    d.add((1.0, 2.0, 3.0), 1.0)
    d.add((2.0, 3.0, 5.0), 1.2)
    d.add((2.5, 5.0, 8.0), 1.4)
    d.doit(0.1)
    d.maxc(0.75)
    d.op('reset')
    d.doit(0.1)
    d.add((-4.0, 2.0, 0.5), 2.0)
    d.doit(0.1)
    d.add((-3.0, 2.5, 0.5), 2.1)
    d.doit(0.05)

    # Large coordinates, where every store rounds.
    d = Drv(lines, 0.15, (123456.7, -98765.4, 5432.1))
    t = 1000.0
    for step in range(5):
        p = circle(3000.0, step * 7.0, (120000.0, -100000.0, 5000.0), 1.1)
        d.add(p, t)
        d.doit(0.033)
        d.doit(0.034)
        t += 0.1 + 0.01 * step

    # Normalisation on a unit vector that turns.
    d = Drv(lines, 0.1, (0.0, 1.0, 0.0))
    for step in range(4):
        a = math.radians(step * 20.0)
        d.add((math.cos(a), math.sin(a), 0.0), step * 0.1)
        d.doit(0.05)
        d.op('normp')
        d.op('normt')
        d.doit(0.03)

    # Random walks.
    for _ in range(8):
        d = Drv(lines, rng.uniform(0.0, 1.0), tuple(rng.uniform(-50.0, 50.0) for _ in range(3)))
        pos = [rng.uniform(-500.0, 500.0) for _ in range(3)]
        vel = [rng.uniform(-80.0, 80.0) for _ in range(3)]
        t = rng.uniform(0.0, 100.0)
        for _ in range(rng.randint(6, 14)):
            r = rng.random()
            if r < 0.45:
                d.add(tuple(pos), t)
                gap = rng.choice((0.0, rng.uniform(0.02, 0.5), rng.uniform(0.02, 0.5)))
                t += gap
                for axis in range(3):
                    vel[axis] += rng.uniform(-30.0, 30.0)
                    pos[axis] += vel[axis] * gap
            elif r < 0.9:
                d.doit(rng.uniform(0.005, 0.6))
            elif r < 0.95:
                d.maxc(rng.uniform(0.05, 1.0))
            else:
                d.op('reset')
    return lines


# --- CRemotePhysicsSimulation -----------------------------------------------


def quat_axis(axis, degrees, scale=1.0):
    n = math.sqrt(sum(a * a for a in axis))
    h = math.radians(degrees) / 2.0
    s = math.sin(h) / n
    return tuple(scale * v for v in (math.cos(h), axis[0] * s, axis[1] * s, axis[2] * s))


def quat(q):
    return ' '.join(f32(v) for v in q)


IDENTITY = (1.0, 0.0, 0.0, 0.0)


class Remote:
    def __init__(self, lines):
        self.lines = lines
        lines.append('seq')

    def update(self, pos, q, t):
        self.lines.append('update %s %s %s' % (vec(*pos), quat(q), f64(t)))

    def doit(self, dt):
        self.lines.append('doit %s' % f64(dt))

    def sync(self, p1, p2, q):
        self.lines.append('sync %s %s %s' % (vec(*p1), vec(*p2), quat(q)))

    def op(self, name):
        self.lines.append(name)


def remote_lines():
    lines = []

    # Straight flight, identity orientation.
    r = Remote(lines)
    r.update((0.0, 0.0, 0.0), IDENTITY, 0.0)
    r.doit(0.25)
    r.update((10.0, 0.0, 0.0), IDENTITY, 1.0)
    r.doit(0.1)
    r.doit(0.4)
    r.sync((15.0, 0.0, 0.0), (15.0, 0.0, 0.0), IDENTITY)
    r.sync((15.0, 0.0, 0.0), (17.0, 0.0, 0.0), IDENTITY)
    r.sync((15.0, 0.0, 0.0), (17.5, 0.0, 0.0), IDENTITY)
    r.sync((15.0, 0.0, 0.0), (15.0, 0.0, 0.0), quat_axis((0.0, 0.0, 1.0), 30.0))
    r.sync((15.0, 0.0, 0.0), (15.0, 0.0, 0.0), quat_axis((0.0, 1.0, 0.0), 29.0))
    r.doit(0.5)

    # A turning ship on a curved path: the blend inside the window, the
    # projection after it, and the tolerances scaled by far samples.
    # The probes take the true pose at the current time, so the position
    # test passes and the axis tests decide; then offsets past each bound.
    r = Remote(lines)
    t = 2.0
    for step in range(7):
        q = quat_axis((0.2, 0.3, 1.0), step * 10.0)
        pos = circle(80.0, step * 12.0, (100.0, 50.0, -20.0), 0.3)
        r.update(pos, q, t)
        waits = [0.03, 0.03] + ([0.08] if step % 2 == 1 else [])
        for wait in waits:
            r.doit(wait)
        now = step + sum(waits) / 0.1
        true_pos = circle(80.0, now * 12.0, (100.0, 50.0, -20.0), 0.3)
        true_q = quat_axis((0.2, 0.3, 1.0), now * 10.0)
        far = (true_pos[0] + 700.0, true_pos[1], true_pos[2])
        very_far = (true_pos[0], true_pos[1] - 9000.0, true_pos[2])
        r.sync(pos, true_pos, true_q)
        r.sync(far, true_pos, quat_axis((0.2, 0.3, 1.0), now * 10.0 + 25.0))
        r.sync(true_pos, true_pos, quat_axis((0.2, 0.3, 1.0), now * 10.0 + 35.0))
        r.sync(very_far, (true_pos[0] + 12.0, true_pos[1], true_pos[2]), true_q)
        r.sync(true_pos, (true_pos[0] + 2.5, true_pos[1], true_pos[2]), true_q)
        r.sync(true_pos, (true_pos[0], true_pos[1] + 1.5, true_pos[2]), true_q)
        t += 0.1

    # The back-off: a clock past it doubles it and answers yes, a forced
    # sync puts it back to 5.
    r = Remote(lines)
    r.update((1.0, 2.0, 3.0), quat_axis((1.0, 0.0, 0.0), 45.0), 0.0)
    r.update((2.0, 2.0, 3.0), quat_axis((1.0, 0.0, 0.0), 45.0), 0.5)
    r.doit(6.0)
    r.sync((0.0, 0.0, 0.0), (0.0, 0.0, 0.0), IDENTITY)
    r.sync((14.0, 2.0, 3.0), (14.0, 2.0, 3.0), quat_axis((1.0, 0.0, 0.0), 45.0))
    r.doit(5.0)
    r.sync((0.0, 0.0, 0.0), (0.0, 0.0, 0.0), IDENTITY)
    r.sync((0.0, 0.0, 0.0), (0.0, 0.0, 0.0), IDENTITY)
    r.op('reset')
    r.doit(0.1)
    r.update((3.0, 2.0, 3.0), quat_axis((1.0, 0.0, 0.0), 50.0), 1.0)
    r.doit(0.1)

    # A quaternion that is not unit (QuaternionToMatrix scales by 2/|q|^2),
    # equal time stamps and a reversal.
    r = Remote(lines)
    r.update((0.0, 0.0, 0.0), quat_axis((0.0, 1.0, 0.0), 20.0, 2.0), 0.0)
    r.update((0.0, 0.0, 5.0), quat_axis((0.0, 1.0, 0.0), 25.0, 0.5), 0.0)
    r.doit(0.1)
    r.update((0.0, 0.0, 10.0), quat_axis((0.0, 1.0, 0.0), 30.0), 0.2)
    r.doit(0.1)
    r.update((0.0, 0.0, 4.0), quat_axis((0.0, 1.0, 0.0), 35.0), 0.4)
    r.doit(0.1)
    r.doit(0.2)
    r.sync((0.0, 0.0, 4.0), (0.0, 0.0, 4.0), quat_axis((0.0, 1.0, 0.0), 35.0))

    # Random flights: a smooth path and a steady turn.
    for _ in range(5):
        r = Remote(lines)
        pos = [rng.uniform(-2000.0, 2000.0) for _ in range(3)]
        vel = [rng.uniform(-100.0, 100.0) for _ in range(3)]
        axis = tuple(rng.uniform(-1.0, 1.0) for _ in range(3))
        rate = rng.uniform(-60.0, 60.0)
        angle = rng.uniform(0.0, 360.0)
        t = rng.uniform(0.0, 50.0)
        for index in range(rng.randint(8, 16)):
            x = 0.0 if index == 0 else rng.random()
            q = quat_axis(axis, angle)
            if x < 0.4:
                r.update(tuple(pos), q, t)
                gap = rng.uniform(0.05, 0.3)
                t += gap
                angle += rate * gap
                for k in range(3):
                    vel[k] += rng.uniform(-20.0, 20.0)
                    pos[k] += vel[k] * gap
            elif x < 0.8:
                r.doit(rng.uniform(0.01, 0.3))
            elif x < 0.97:
                off = rng.choice((0.0, 1.0, 3.0, 800.0))
                p2 = tuple(p + rng.uniform(-off, off) for p in pos)
                r.sync(tuple(pos), p2, quat_axis(axis, angle + rng.uniform(-30.0, 30.0)))
            else:
                r.op('reset')
    return lines


def main():
    write_raw('collision_a.ini', COLLISION_A)
    write_raw('collision_b.ini', COLLISION_B)
    write_raw('collision_c.ini', COLLISION_C)
    write('damage.txt', damage_lines(), ['Collision damage calls (make_physics_fixtures.py)'])
    write('drv.txt', drv_lines(), ['CDeadReckonedVector sequences (make_physics_fixtures.py)'])
    write('remote.txt', remote_lines(), ['CRemotePhysicsSimulation sequences (make_physics_fixtures.py)'])


if __name__ == '__main__':
    main()
