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
