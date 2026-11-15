// Author: Giperion.

#pragma once

// DX9RENDERER Ex: the port-only Direct3D 9 extensions of DX9RENDERER.dll (render/IRenderPipelineEx.h). The
// interface methods are members of FDirectX9 (DX9RENDERER.h) defined in DX9RENDEREREx.cpp; this header holds
// their state. Nothing here exists in DX8RENDERER.dll or baseline; see implementation notes, "Ex extensions".

#include "DX9RENDERER.h"

#include <string>
#include <vector>

// A shader program: the source location for reloads, the last bytecode that compiled, and the
// shader objects made from it (null while no device, or when the source never compiled).
struct FRPExProgram
{
    IFileSystem* FileSystem; // AddRef'd
    std::string Path;
    std::string VsEntry;
    std::string PsEntry;
    std::vector<unsigned char> VsBytecode;
    std::vector<unsigned char> PsBytecode;
    IDirect3DVertexShader9* Vs;
    IDirect3DPixelShader9* Ps;
};

enum EDx9RendererExTextureKind : unsigned int
{
    Dx9RendererExTextureRenderTarget = 0, // D3DPOOL_DEFAULT, re-created on every reset
    Dx9RendererExTextureVolume = 1,       // D3DPOOL_MANAGED, survives resets
    Dx9RendererExTextureSceneDepth = 2,   // the INTZ depth buffer (owned by FDx9RendererExState)
};

struct FRPExTexture
{
    EDx9RendererExTextureKind Kind;
    ERPExFormat Format;
    // Render targets: the creation request (Width/Height 0 = BackBufferScale of the back buffer).
    FRPExRenderTargetDesc TargetDesc;
    // The current size (render targets: after the last re-creation).
    unsigned long Width;
    unsigned long Height;
    unsigned long Depth;
    // IDirect3DTexture9 (render target, scene depth) or IDirect3DVolumeTexture9.
    IDirect3DBaseTexture9* Texture;
    // Level 0 of a render target / the scene depth surface; null for volumes.
    IDirect3DSurface9* Surface;
    // Volumes: the creation texels, for a device re-creation after ShutdownPipeline.
    std::vector<unsigned char> VolumeData;
};

struct FDx9RendererExState
{
    FDx9RendererExState();

    std::vector<FRPExProgram*> Programs;
    // Render targets and volume textures created through the interface.
    std::vector<FRPExTexture*> Textures;
    // The INTZ depth buffer; Texture/Surface null when the device has no INTZ depth.
    FRPExTexture SceneDepth;
    // A plain depth surface made when the INTZ texture could not be created after the device
    // was already made without an auto depth-stencil.
    IDirect3DSurface9* FallbackDepth;
    unsigned long BackBufferWidth;
    unsigned long BackBufferHeight;

    // The open pass (BeginExPass .. EndExPass).
    bool bInPass;
    bool bDepthBound;
    IDirect3DSurface9* SavedColor;
    IDirect3DSurface9* SavedDepth;
    D3DVIEWPORT9 SavedViewport;
    unsigned char bSavedViewportRectValid;
    unsigned char bSavedDepthRangeValid;
    unsigned long SavedPending[FDx9RendererRenderStateCache::StateCount];
    // Bit per sampler 0..15 the pass bound a texture on; stages 0..7 also saved their cache.
    unsigned long TouchedSamplers;
    FDx9RendererExStageSave SavedStages[FDx9RendererDeviceStateCaches::StageCount];
    // Bit per sampler that holds the scene depth.
    unsigned long SceneDepthSamplers;
    // What the pass bound on each sampler (not AddRef'd; Get* is unavailable on a pure device).
    IDirect3DBaseTexture9* BoundTextures[16];
    FRPExProgram* BoundProgram;
    // The pass's colour target (null = the back buffer) and its size.
    FRPExTexture* CurrentTarget;
    unsigned long TargetWidth;
    unsigned long TargetHeight;
};
