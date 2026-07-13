#pragma once

// ============================================================================
// core/math_compat.h — HD2D -> aima math shim.
//
// The math value types (Float2/3/4 + the DirectXMath load* bridge) now live in
// aima_framework (core/math.h, namespace aima). HD2D code throughout uses these
// UNQUALIFIED inside `namespace hd2d` (e.g. `Float3 pos`, `load3(v)`), relying on
// ordinary name lookup back when they lived in `namespace hd2d`. This shim brings
// the aima names into `namespace hd2d` via using-declarations so every existing
// unqualified use keeps resolving. Every TU that used `#include "core/math.h"`
// now includes this instead.
//
// NOTE: the renderer-specific 64-byte interleaved `Vertex` struct that used to
// live in core/math.h is GRAPHICS and moved to renderer/rhi.h (namespace hd2d).
// Files that need `Vertex` include "renderer/rhi.h" directly.
// ============================================================================

#include "core/math.h"   // resolves to aima_framework/src/core/math.h (aima::*)

namespace hd2d {

namespace dx = DirectX;   // HD2D code spells the DirectXMath namespace `dx::`

using aima::Float2;
using aima::Float3;
using aima::Float4;
using aima::load2;
using aima::load3;
using aima::load4;

// ===========================================================================
// hd2d::math — OS/머신-중립 수학 파사드 (PAL 단계 1, 2026-06-23)
//
// DirectXMath는 머신 의존(SSE/NEON SIMD)이라 PAL 원칙상 래핑 대상. 게임/렌더러/
// 에셋 코드는 `dx::`/`DirectX::`/`XM*`를 직접 쓰지 않고 이 네임스페이스만 쓴다.
// dx:: 토큰은 이 헤더 내부(파사드 구현)에만 존재한다.
//
//   - 저장형(Mat4x4/Quat/Vec*)은 DirectXMath POD와 **byte-identical** alias이므로
//     live_scene.h / GPU 상수버퍼 / 컴포넌트 레이아웃이 불변(ABI 안전).
//   - 연산형(Vec/Matrix)은 in-register SIMD 별칭.
//   - 규약 고정: LEFT-HANDED · ROW-VECTOR/ROW-MAJOR(p' = p*M, 이동=행3) · [0,1] 깊이.
//     (가법 단계 — 기존 dx:: 사용처는 그대로 두고, 호출부는 후속 단계에서 교체.)
// ===========================================================================
namespace math {

// ---- 저장형 (POD, byte-identical to DirectXMath; ABI 고정) ----
using Vec2   = ::aima::Float2;          // {x,y}
using Vec3   = ::aima::Float3;          // {x,y,z}
using Vec4   = ::DirectX::XMFLOAT4;     // 동차 점/일반 4-벡터
using Quat   = ::DirectX::XMFLOAT4;     // {x,y,z,w}
using Mat4x4 = ::DirectX::XMFLOAT4X4;   // row-major 4x4, 16 floats

static_assert(sizeof(Mat4x4) == 64, "Mat4x4 must be 16 contiguous floats (XMFLOAT4X4 layout)");
static_assert(alignof(Mat4x4) == 4,  "Mat4x4 alignment must match XMFLOAT4X4");
static_assert(sizeof(Quat) == 16,    "Quat must be 4 floats (XMFLOAT4 layout)");
static_assert(sizeof(Vec3) == 12,    "Vec3 must be 3 contiguous floats");
static_assert(sizeof(Vec2) == 8,     "Vec2 must be 2 contiguous floats");

// ---- 연산형 (in-register SIMD) ----
using Vec    = ::DirectX::XMVECTOR;
using Matrix = ::DirectX::XMMATRIX;

// ---- 로드/스토어 (POD <-> register 브리지) ----
inline Vec    load2(const Vec2& f) { return ::DirectX::XMVectorSet(f.x, f.y, 0.0f, 0.0f); }
inline Vec    load3(const Vec3& f) { return ::DirectX::XMVectorSet(f.x, f.y, f.z, 0.0f); }
inline Vec    load4(const Vec4& f) { return ::DirectX::XMLoadFloat4(&f); }
inline Matrix load(const Mat4x4& m) { return ::DirectX::XMLoadFloat4x4(&m); }
inline void   store(Mat4x4& dst, Matrix m) { ::DirectX::XMStoreFloat4x4(&dst, m); }
inline void   store4(Vec4& dst, Vec v) { ::DirectX::XMStoreFloat4(&dst, v); }
inline void   store3(Vec3& dst, Vec v) {
    ::DirectX::XMFLOAT3 t; ::DirectX::XMStoreFloat3(&t, v); dst.x = t.x; dst.y = t.y; dst.z = t.z;
}

// ---- 벡터 빌더/접근자 ----
inline Vec   vec(float x, float y, float z, float w) { return ::DirectX::XMVectorSet(x, y, z, w); }
inline float get_x(Vec v) { return ::DirectX::XMVectorGetX(v); }
inline float get_y(Vec v) { return ::DirectX::XMVectorGetY(v); }
inline float get_z(Vec v) { return ::DirectX::XMVectorGetZ(v); }
inline float get_w(Vec v) { return ::DirectX::XMVectorGetW(v); }
inline Vec   sub(Vec a, Vec b)       { return ::DirectX::XMVectorSubtract(a, b); }
inline Vec   scale_v(Vec v, float s) { return ::DirectX::XMVectorScale(v, s); }
inline Vec   normalize3(Vec v)       { return ::DirectX::XMVector3Normalize(v); }

// ---- 행렬 빌더 (LEFT-HANDED, ROW-VECTOR, [0,1] 깊이) ----
inline Matrix identity()                               { return ::DirectX::XMMatrixIdentity(); }
inline Matrix scaling(float x, float y, float z)       { return ::DirectX::XMMatrixScaling(x, y, z); }
inline Matrix scaling_v(Vec s)                         { return ::DirectX::XMMatrixScalingFromVector(s); }
inline Matrix rotation_y(float radians)                { return ::DirectX::XMMatrixRotationY(radians); }
inline Matrix rotation_quat(Vec q)                     { return ::DirectX::XMMatrixRotationQuaternion(q); }
inline Matrix translation(float x, float y, float z)   { return ::DirectX::XMMatrixTranslation(x, y, z); }
inline Matrix look_at_lh(Vec eye, Vec target, Vec up)  { return ::DirectX::XMMatrixLookAtLH(eye, target, up); }
inline Matrix perspective_fov_lh(float fovy, float aspect, float zn, float zf) {
    return ::DirectX::XMMatrixPerspectiveFovLH(fovy, aspect, zn, zf);
}
inline Matrix orthographic_lh(float w, float h, float zn, float zf) {
    return ::DirectX::XMMatrixOrthographicLH(w, h, zn, zf);
}

// ---- 행렬 연산 ----
inline Matrix mul(Matrix a, Matrix b) { return ::DirectX::XMMatrixMultiply(a, b); } // a 먼저 적용(row-vector)
inline Matrix transpose(Matrix m)     { return ::DirectX::XMMatrixTranspose(m); }
inline Matrix inverse(Matrix m)       { ::DirectX::XMVECTOR det; return ::DirectX::XMMatrixInverse(&det, m); }
inline bool   decompose(Matrix m, Vec& scale, Vec& rot_quat, Vec& trans) {
    return ::DirectX::XMMatrixDecompose(&scale, &rot_quat, &trans, m); // false = 분해 실패
}
inline Vec    transform4(Vec v, Matrix m) { return ::DirectX::XMVector4Transform(v, m); }

} // namespace math

} // namespace hd2d
