// Author: Giperion.

// OutpostServer.exe crash reporting (baseline module storage..0040911c): an unhandled-exception
// filter that closes a hosted DirectPlay session and hands the crash to Dr. Watson (dw15.exe).
// Instruction-identical to OutpostFrontier.exe 005a5390 / 005a54e0 / 005a5910 (the client's copy
// lives in src/OutpostFrontier/Client/main.cpp); baseline links two copies, so does the port.
// MFC-free header: CrashReport.cpp is a non-MFC TU (neuronruntime_net.h, common_errorhandler.h).
#pragma once

// — copies AppName to the 0x80-wchar crash name (module storage), forces its
// last wchar to zero and installs the filter (module storage) with SetUnhandledExceptionFilter.
void InstallCrashHandler(const wchar_t* AppName);
