#pragma once

#include <cstdint>

#include <d3d12.h>
#include <dxgi1_6.h>

#if defined(_DEBUG)
#   include <dxgidebug.h>
#endif

#define D3DX12_NO_STATE_OBJECT_HELPERS
#include "d3dx12.h"

#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dxguid.lib")

#define IID_GRAPHICS_PPV_ARGS(ppType) __uuidof(ppType), (ppType).put_void()

#define D3D12_GPU_VIRTUAL_ADDRESS_NULL ((D3D12_GPU_VIRTUAL_ADDRESS)0)
#define D3D12_GPU_VIRTUAL_ADDRESS_UNKNOWN ((D3D12_GPU_VIRTUAL_ADDRESS) - 1)

constexpr D3D12_CPU_DESCRIPTOR_HANDLE D3D12_CPU_HANDLE_NULL = {D3D12_GPU_VIRTUAL_ADDRESS_NULL};
constexpr D3D12_CPU_DESCRIPTOR_HANDLE D3D12_CPU_HANDLE_UNKNOWN = {D3D12_GPU_VIRTUAL_ADDRESS_UNKNOWN};

constexpr D3D12_GPU_DESCRIPTOR_HANDLE D3D12_GPU_HANDLE_NULL = {D3D12_GPU_VIRTUAL_ADDRESS_NULL};
constexpr D3D12_GPU_DESCRIPTOR_HANDLE D3D12_GPU_HANDLE_UNKNOWN = {D3D12_GPU_VIRTUAL_ADDRESS_UNKNOWN};

#ifndef MAKEFOURCC
#   define MAKEFOURCC(ch0, ch1, ch2, ch3) \
     ((DWORD)(BYTE)(ch0) | ((DWORD)(BYTE)(ch1) << 8) | ((DWORD)(BYTE)(ch2) << 16) | ((DWORD)(BYTE)(ch3) << 24))
#endif

namespace Neuron
{

consteval std::uint32_t FixedMessage(const char* _msg)
{
  return static_cast<std::uint32_t>(_msg[0]) << 24 | static_cast<std::uint32_t>(_msg[1]) << 16 | static_cast<std::uint32_t>(_msg[2]) << 8 |
         static_cast<std::uint32_t>(_msg[3]);
}

#if defined _DEBUG
inline void SetName(ID3D12Object* _object, const LPCWSTR _name)
{
  winrt::check_hresult(_object->SetName(_name));
}

inline void SetNameIndexed(ID3D12Object* _object, const LPCWSTR _name, const UINT _index)
{
  WCHAR fullName[50];
  if (swprintf_s(fullName, L"%s[%u]", _name, _index) > 0)
  {
    winrt::check_hresult(_object->SetName(fullName));
  }
}

#else
inline void SetName([[maybe_unused]] ID3D12Object* _object, [[maybe_unused]] const LPCWSTR _name) {}
inline void SetNameIndexed([[maybe_unused]] ID3D12Object* _object, [[maybe_unused]] const LPCWSTR _name, [[maybe_unused]] const UINT _index)
{
}
#endif

} // namespace Neuron

#define NAME_D3D12_OBJECT(x) ::Neuron::SetName((x).get(), L#x)
#define NAME_D3D12_OBJECT_INDEXED(x, n) ::Neuron::SetNameIndexed((x)[n].get(), L#x, n)
