// pch.h: precompiled header for NeuronClientTests.
//
// Mirrors NeuronClient/pch.h: windows.h lean and NOMINMAX first, then the
// graphics headers, because the client's public headers use HWND and D3D12
// types (AGENTS.md §4).

#ifndef PCH_H
#define PCH_H

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#endif // PCH_H
