#pragma once

/*
 * NeuronClient precompiled header.
 *
 * Order is load-bearing (AGENTS.md §4): windows.h lean and with NOMINMAX
 * first, then the graphics headers, which depend on it.
 */

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <cstddef>
#include <cstdint>
