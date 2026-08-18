#pragma once

/*
 * NeuronServer precompiled header. windows.h lean and NOMINMAX first
 * (AGENTS.md §4) -- WIN32_LEAN_AND_MEAN also keeps winsock 1 out, which is what
 * lets the transport include winsock2.h without a conflict.
 */

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cstddef>
#include <cstdint>
