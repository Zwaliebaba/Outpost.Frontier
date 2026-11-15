// OutpostFrontier is a reimplementation of the original 2003 game client and engine,
// faithful to the behaviour of the original game.
// This file was reconstructed by reverse engineering the original game's version 1.1 binaries,
// developed by Digital Anvil.
// Author: Giperion.

#pragma once

// Export when building Server.dll, import when another PE includes these headers.
// Server.vcxproj defines SERVER_EXPORTS. Do not define it in OutpostFrontier / other DLLs.

#ifdef SERVER_EXPORTS
#define SERVER_API __declspec(dllexport)
#else
#define SERVER_API __declspec(dllimport)
#endif

// Suppress dll-interface warnings for STL members
#pragma warning(disable : 4251)
#pragma warning(disable : 4275)
