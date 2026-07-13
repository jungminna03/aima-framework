#pragma once

#include "core/math_compat.h"
#include "renderer/gpu_resources.h"

#include <optional>
#include <string>
#include <vector>

namespace hd2d {

// ----------------------------------------------------------------------------
// Blender-as-scene-editor glTF loader.
// Spec: docs/superpowers/specs/2026-06-05-blender-pipeline-design.md
//
// Carries the full Principled-BSDF/glTF PBR material set, embedded textures,
// punctual lights (converted back to radiometric watts so the engine can match
// Eevee), Blender custom properties (glTF `extras`), and a CPU copy of every
// mesh for physics / cloth. glTF (Y-up RH) -> engine (Y-up LH) by Z-flip.
// ----------------------------------------------------------------------------

// glTF PBR metallic-roughness material. Texture fields index LoadedScene::
// textures, -1 = none (bind the engine fallback).
struct LoadedMaterial {
    std::string name;
    float base_color[4] = {1, 1, 1, 1};
    float metallic = 0.0f;
    float roughness = 1.0f;
    float emissive[3] = {0, 0, 0};
    float emissive_strength = 1.0f;        // KHR_materials_emissive_strength
    int tex_base = -1;                     // sRGB
    int tex_mr = -1;                       // linear (G=roughness, B=metallic)
    int tex_normal = -1;                   // linear tangent-space
    int tex_emissive = -1;                 // sRGB
    int alpha_mode = 0;                    // 0 opaque, 1 mask, 2 blend
    float alpha_cutoff = 0.5f;
    bool double_sided = false;
    // Sampler choice from the baseColor texture: bit0 = nearest, bit1 = clamp.
    int sampler_flags = 0;
};

// CPU-side mesh copy (engine space, winding already fixed) for Jolt shapes and
// cloth. `pin` mirrors the `_PIN` vertex attribute (empty when absent).
struct CpuMesh {
    std::vector<Float3> positions;
    std::vector<uint32_t> indices;
    std::vector<float> pin;
};

// One drawable primitive of a node + its material + its CPU copy.
struct LoadedPrimitive {
    rhi::GpuMesh mesh;            // host Dx12ResourceTable handle (PAL render keystone)
    uint32_t     index_count = 0; // geometry presence (both backends) — sdlgpu draws from cpu
    int material = -1;
    CpuMesh cpu;
};

// Parsed Blender custom properties (glTF extras). See spec §1.2 for the
// authoring contract; defaults make an unannotated map mesh a static collider.
struct NodeExtras {
    std::string body = "static";           // "static" | "dynamic" | "none"
    std::string shape;                     // "" = default (mesh / convex)
    float mass = 1.0f;
    float friction = 0.5f;
    float restitution = 0.0f;
    bool cloth = false;
    float wind = 1.0f;
    bool nocast = false;                   // exclude from shadow casting
    std::string spawn;                     // empties: "player" etc.
    bool has_world = false;                // world_color / world_strength present
    float world_color[3] = {0, 0, 0};
    float world_strength = 1.0f;
};

// A punctual light, converted to radiometric units (Eevee-comparable).
// type: 0 directional (intensity = W/m^2), 1 point, 2 spot (intensity = W).
struct LoadedLight {
    int type = 1;
    float color[3] = {1, 1, 1};
    float intensity_w = 0.0f;
    float range = 0.0f;                    // 0 = unbounded
    float inner_cos = 1.0f;                // spot cone cosines
    float outer_cos = 0.7071f;
};

// One glTF node: full world transform (engine space, row-vector convention),
// drawable primitives, parsed extras, and an optional light.
struct LoadedNodeEx {
    std::string name;
    dx::XMFLOAT4X4 world;
    std::vector<LoadedPrimitive> prims;
    NodeExtras extras;
    std::optional<LoadedLight> light;
};

struct LoadedScene {
    std::vector<LoadedNodeEx> nodes;
    std::vector<LoadedMaterial> materials;   // indexed by LoadedPrimitive::material
    std::vector<GpuTexture> textures;        // indexed by LoadedMaterial::tex_*
};

// Load a .glb/.gltf scene. Returns false on parse/upload failure (logged).
bool load_gltf(Dx12Device& dev, Dx12ResourceTable& table, const std::string& path, LoadedScene& out);

} // namespace hd2d
