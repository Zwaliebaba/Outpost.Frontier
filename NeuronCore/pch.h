#pragma once

/*
 * NeuronCore precompiled header.
 *
 * Windows first and lean: WIN32_LEAN_AND_MEAN keeps the socket headers out of
 * windows.h so Winsock2 can be included cleanly by the transport, and NOMINMAX
 * stops the min/max macros from colliding with the standard library. AGENTS.md
 * §4 fixes this order.
 */

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cstddef>
#include <cstdint>
