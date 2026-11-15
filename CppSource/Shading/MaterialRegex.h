// OutpostFrontier is a reimplementation of the original 2003 game client and engine,
// faithful to the behaviour of the original game.
// This file was reconstructed by reverse engineering the original game's version 1.1 binaries,
// developed by Digital Anvil.
// Author: Giperion.

#pragma once

#include <memory>
#include <vector>

// Private Shading regexp program; no retail CRT or container layout crosses a DLL.
class FMaterialRegex
{
public:
    static std::unique_ptr<FMaterialRegex> Compile(const char* Pattern);
    static bool Search(const FMaterialRegex* Expression, const char* Text);

private:
    std::vector<unsigned char> Program;
    unsigned char StartCharacter = 0;
    bool bAnchored = false;
    int MustOffset = -1;
    int MustLength = 0;
};
