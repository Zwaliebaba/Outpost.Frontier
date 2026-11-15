// Author: Giperion.

#pragma once

// baseline Intro.cpp Image (`Client\Interface\Intro.cpp`, 0x00565010 ..
// ): a full-screen picture held in an ximage.dll BigImage. Three users:
//   - InitializeEngine's startup screen, a stack local (vptr at 0x005b1a1e, Load 0x005b1a5a,
//     Draw 0x005b1a7d / 0x005b1aa6, Release 0x005b1ab9, dtor 0x005b1c07);
//   - the loading screen the field (+0x04, built by the routine through the routine);
//   - IntroFocus +0x210, the menu logo (ctor 0x0057384d, Load 0x005738f6, Draw 0x005742dd,
//     Release inlined at 0x00573e90, dtor the routine).

struct IBigImage2;

// RTTI .?AUIImage@@, vftable 0x005e2708: {the branch deleting dtor, _purecall, _purecall}.
struct IImage
{
    // (body) / the branch (deleting): only the vptr store.
    virtual ~IImage();
    // slot 1
    virtual bool Load(char const* Path) = 0;
    // slot 2
    virtual void Draw() = 0;
};

// RTTI .?AUImage@@, vftable 0x005e2718: {the routine, the routine, the routine}. 0x18 bytes
// (operator new(0x18) in the routine).
struct Image : public IImage
{
    // Inlined at every construction (0x00565d9e, 0x00573847, 0x005b1a16): vptr, +0x14 = 0.
    // +0x04..+0x13 are not written.
    Image();
    // (the routine deleting).
    ~Image() override;

    // (RET 4): by the extension after the last '.', "bmp" (NEURONCOMPONENTS stricmp) goes to
    // LoadBmp; everything else, including no extension, to LoadTga.
    bool Load(char const* Path) override;
    // nothing without a BigImage; otherwise the 2D ortho set-up, seven saved
    // render states, two texture stages, and the BigImage drawn over the whole surface.
    void Draw() override;

    // release the BigImage and clear +0x14.
    void Release();
    // (RET 4): LoadImageA the DIB <data path>[\]<TgaPath><Path> into a BigImage.
    bool LoadBmp(char const* Path);
    // (RET 4): map the TGA through a "FileSystem" rooted at TgaPath and build a
    // BigImage from the mapped pixels (32-bit formats only).
    bool LoadTga(char const* Path);

    // +0x04..+0x13 -- no instruction in 0x00565010..0x00565d90 or at the three embedding sites
    // reads or writes them. TODO(INTRO): identify (padding of a base or dead fields).
    unsigned char Field_0x04[0x10];
    IBigImage2* BigImage; // +0x14

private:
    Image(Image const&) = delete;
    Image& operator=(Image const&) = delete;
};

static_assert(sizeof(void*) != 4 || sizeof(Image) == 0x18, "baseline Image is 0x18 bytes");

// (cdecl): operator new(0x18); on success vptr + BigImage = 0. Caller the routine.
Image* CreateImage();

#ifdef OUTPOST_FRONTIER_WIDESCREEN_SUPPORT
// Port-only: where Image::Draw puts its 4:3 picture on a wider surface until told otherwise --
// -1 against the left edge, +1 against the right, 0 centred (the default).
void ImageSetWidescreenAlign(int Side);
#endif
