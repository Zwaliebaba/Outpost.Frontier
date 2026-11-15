// Author: Giperion.

/* baseline EXE/zlib.dll (Gilles Vollant, 2002-03-13) returns "1.1.4".
 * NeuronRuntime hardcodes that string into deflateInit_/inflateInit_ and does not
 * import zlibVersion, but GetProcAddress("zlibVersion") should match. */

const char* __stdcall OutpostFrontierZlibVersion(void)
{
    return "1.1.4";
}
