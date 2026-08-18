#pragma once

/*
 * Outpost precompiled header. Same order as NeuronCore's (AGENTS.md §4):
 * windows.h first and lean, then the standard library.
 */

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cstddef>
#include <cstdint>
