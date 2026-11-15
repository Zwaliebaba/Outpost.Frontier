// OutpostFrontier is a reimplementation of the original 2003 game client and engine,
// faithful to the behaviour of the original game.
// This file was reconstructed by reverse engineering the original game's version 1.1 binaries,
// developed by Digital Anvil.
// Author: Giperion.

// OutpostServer.exe crash reporting (retail .text:00408b70..0040911c): an unhandled-exception
// filter that closes a hosted DirectPlay session and hands the crash to Dr. Watson (dw15.exe).
// Instruction-identical to OutpostFrontier.exe 005a5390 / 005a54e0 / 005a5910 (the client's copy
// lives in src/OutpostFrontier/Client/main.cpp); retail links two copies, so does the port.
// MFC-free header: CrashReport.cpp is a non-MFC TU (dalib_net.h, common_errorhandler.h).
#pragma once

// .text:004090f0 — copies AppName to the 0x80-wchar crash name (.data:00426b20), forces its
// last wchar to zero and installs the filter (.text:00408cc0) with SetUnhandledExceptionFilter.
void InstallCrashHandler(const wchar_t* AppName);
