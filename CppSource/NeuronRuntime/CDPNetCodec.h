// Author: Giperion.

#pragma once

#include <zlib.h>

// Independent reusable deflate/inflate streams; each operation resets its stream.
class CDPNetCodec
{
public:
    CDPNetCodec();
    ~CDPNetCodec();

    // single-shot deflate(Z_FINISH) + deflateReset. Rejects InputSize < 0x20
    // (baseline: too small to bother compressing) by returning false.
    bool Compress(const void* Input, unsigned int InputSize, void* Output, unsigned int OutputCapacity);
    // single-shot inflate(Z_FINISH) + inflateReset.
    bool Decompress(const void* Input, unsigned int InputSize, void* Output, unsigned int OutputCapacity);

    unsigned long GetLastCompressedSize() const { return LastCompressedSize; }
    unsigned long GetLastDecompressedSize() const { return LastDecompressedSize; }

private:
    z_stream DeflateStream{};
    z_stream InflateStream{};
    unsigned long LastCompressedSize = 0;
    unsigned long LastDecompressedSize = 0;
};
