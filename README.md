# Outpost.Frontier

A native C++23 rebuild of Homeworld: Deserts of Kharak (Blackbird Interactive, 2016): the same
simulation, missions and content, with no Unity or Mono at runtime, verified bit for bit against
the original.

| Read | For |
|---|---|
| [Design/](Design/README.md) | the goal, the measured inventory of the original, architecture, asset pipeline, **the plan**, verification, risks, decisions |
| [CONTRIBUTING.md](CONTRIBUTING.md) | how code is written and built here |
| [Tools/Extract](Tools/Extract/README.md) | rebuilding the inputs (decompiled code, extracted assets) from your installation |

Status: phase 0 (foundations) done; next is phase 1 (the Recorder) and phase 2 (the asset
pipeline). See [Design/Plan.md](Design/Plan.md).

The original game's code and data are never in this repository; everything is produced locally
from a purchased installation. Private study.
