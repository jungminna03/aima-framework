#pragma once

// ============================================================================
// renderer/device.h — backend-agnostic umbrella for the device header.
//
// Pulls in the real DX12 `Dx12Device` on Windows (HD2D_RENDERER_D3D12) and the
// SDL clear-to-color stub `Dx12Device` everywhere else. Both expose the SAME
// class name + public method surface, so renderer passes, game plugins and the
// host all `#include "renderer/device.h"` and stay source-identical across
// backends. The Windows path is byte-identical to including dx12/device.h.
// ============================================================================

#if defined(HD2D_RENDERER_D3D12)
#include "renderer/dx12/device.h"
#else
#include "renderer/sdlgpu/device.h"
#endif
