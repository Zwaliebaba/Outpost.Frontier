// pch.h: precompiled header for NeuronServerTests.
//
// windows.h lean and NOMINMAX first (AGENTS.md §4): the server's headers reach
// the transport, which reaches Winsock, and WIN32_LEAN_AND_MEAN is what keeps
// winsock 1 from arriving first and colliding with it.

#ifndef PCH_H
#define PCH_H

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#endif // PCH_H
