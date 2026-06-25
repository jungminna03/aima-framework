#pragma once

// Thin math layer over DirectXMath. DirectXMath ships as a CROSS-PLATFORM,
// header-only library via vcpkg (`directxmath` / `Microsoft::DirectXMath`) — it
// is NOT Windows-only and pulls in no graphics API, so it is a fine general
// math choice for any project (2D or 3D) built on this framework.
//
// NOTE (renderer-less framework): this header deliberately carries ONLY
// engine-agnostic value types (vectors + the DirectXMath SIMD bridge). The
// renderer-specific interleaved `Vertex` layout that lived here in the source
// engine is GRAPHICS and belongs in each project's own renderer — it is NOT in
// this framework. Add your own vertex/index types next to your Renderer impl.

#include <DirectXMath.h>
#include <cstdint>

namespace aima {

namespace dx = DirectX;

struct Float2 {
    float x = 0, y = 0;
};

struct Float3 {
    float x = 0, y = 0, z = 0;
};

struct Float4 {
    float x = 0, y = 0, z = 0, w = 0;
};

inline dx::XMVECTOR load2(const Float2& f) { return dx::XMVectorSet(f.x, f.y, 0.0f, 0.0f); }
inline dx::XMVECTOR load3(const Float3& f) { return dx::XMVectorSet(f.x, f.y, f.z, 0.0f); }
inline dx::XMVECTOR load4(const Float4& f) { return dx::XMVectorSet(f.x, f.y, f.z, f.w); }

} // namespace aima
