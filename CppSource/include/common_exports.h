// OutpostFrontier is a reimplementation of the original 2003 game client and engine,
// faithful to the behaviour of the original game.
// This file was reconstructed by reverse engineering the original game's version 1.1 binaries,
// developed by Digital Anvil.
// Author: Giperion.

#pragma once

// Export when building Common.dll, import when another PE includes these headers.
// Common.vcxproj defines COMMON_EXPORTS. Do not define it in OutpostFrontier / other DLLs.

#ifdef COMMON_EXPORTS
#define COMMON_API __declspec(dllexport)
#else
#define COMMON_API __declspec(dllimport)
#endif

// A member that retail emits into BOTH Common.dll and Server.dll, each with its own
// body and its own ordinal -- what a shared header or a shared .cpp compiled into both
// modules produces. CRemotePhysicsSimulation is the case that forced this macro; see
// the export table in the class comment in common_server.h. A single class-level macro
// cannot express "dllexport in both modules", so these carry their own.
//
// OutpostFrontier.exe is the third such module: retail OutpostFrontier.exe exports its own copies
// of the same inline members -- CDeadReckonedVector ordinals 6, 19, 29, 35, 43, 48, 49
// (.text:0053f890..0053fad0), CRemotePhysicsSimulation 8, 21, 31, 36, 42, 44, 45, 46, 54
// (.text:0053fcd0..00540300) -- so OUTPOST_FRONTIER_EXPORTS (OutpostFrontier.vcxproj) selects
// dllexport too. A member whose body is not compiled into the exe (only header-inline and
// implicit members are) is merely declared there, and the exe's calls to it resolve
// through Common.lib's import thunk -- as retail's did.
#if defined(COMMON_EXPORTS) || defined(SERVER_EXPORTS) || defined(OUTPOST_FRONTIER_EXPORTS)
#define SHARED_INLINE_API __declspec(dllexport)
#else
#define SHARED_INLINE_API __declspec(dllimport)
#endif

// Exported classes may contain std::vector / std::map; consumers do not need their dll-interface.
#pragma warning(disable : 4251)
#pragma warning(disable : 4275)
