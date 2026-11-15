// OutpostFrontier is a reimplementation of the original 2003 game client and engine,
// faithful to the behaviour of the original game.
// This file was reconstructed by reverse engineering the original game's version 1.1 binaries,
// developed by Digital Anvil.
// Author: Giperion.

#pragma once

// Decimal MSVCRT atof semantics for Common's INI paths; implemented with integer
// mantissa arithmetic, without loading an old CRT or inheriting x87 precision.
double MsvcrtAtof(char const* Text);
