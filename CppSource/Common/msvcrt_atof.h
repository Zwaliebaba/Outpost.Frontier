// Author: Giperion.

#pragma once

// Decimal MSVCRT atof semantics for Common's INI paths; implemented with integer
// mantissa arithmetic, without loading an old CRT or inheriting x87 precision.
double MsvcrtAtof(char const* Text);
