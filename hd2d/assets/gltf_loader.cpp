#include "assets/gltf_loader.h"

#include "core/log_compat.h"
#include "renderer/rhi.h"        // hd2d::Vertex (64-byte interleaved layout)

#define CGLTF_IMPLEMENTATION
#include <cgltf.h>

#include <nlohmann/json.hpp>
#include <stb_image.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <map>

namespace hd2d {

namespace {

constexpr float kPi = 3.14159265358979f;
// Blender's glTF exporter converts light power (W) to photometric units with
// 683 lm/W (and /4pi sr for point/spot). We invert that here so the shader can
// run the same radiometric falloff Eevee uses.
constexpr float kWattsToLumens = 683.0f;

// ---------------------------------------------------------------------------
// Coordinate conversion. glTF is Y-up right-handed, the engine Y-up
// left-handed: conjugate by F = diag(1,1,-1). For matrices stored for the
// row-vector convention this flips the sign of every element with exactly one
// index equal to 2 (the z row/column), translation z included.
// ---------------------------------------------------------------------------
dx::XMFLOAT4X4 to_engine_matrix(const cgltf_float m16[16]) {
    dx::XMFLOAT4X4 e;
    // glTF column-major array == row-major array of the transposed matrix,
    // which is exactly the row-vector-convention matrix we want.
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            e.m[i][j] = m16[i * 4 + j];
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            if ((i == 2) != (j == 2)) e.m[i][j] = -e.m[i][j];
    return e;
}

std::string upper(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return s;
}

// ---------------------------------------------------------------------------
// extras (Blender custom properties) -> NodeExtras, via nlohmann::json.
// ---------------------------------------------------------------------------
NodeExtras parse_extras(const cgltf_extras& extras) {
    NodeExtras out;
    if (!extras.data) return out;
    const nlohmann::json j = nlohmann::json::parse(extras.data, nullptr, /*throw*/ false);
    if (j.is_discarded() || !j.is_object()) return out;

    auto str = [&](const char* k, std::string& dst) {
        if (j.contains(k) && j[k].is_string()) dst = j[k].get<std::string>();
    };
    auto num = [&](const char* k, float& dst) {
        if (j.contains(k) && j[k].is_number()) dst = j[k].get<float>();
    };
    auto flag = [&](const char* k, bool& dst) {
        if (!j.contains(k)) return;
        const auto& v = j[k];
        if (v.is_boolean()) dst = v.get<bool>();
        else if (v.is_number()) dst = v.get<double>() != 0.0;
    };

    str("body", out.body);
    str("shape", out.shape);
    num("mass", out.mass);
    num("friction", out.friction);
    num("restitution", out.restitution);
    flag("cloth", out.cloth);
    num("wind", out.wind);
    flag("nocast", out.nocast);
    str("spawn", out.spawn);
    num("world_strength", out.world_strength);
    if (j.contains("world_color") && j["world_color"].is_array() &&
        j["world_color"].size() >= 3) {
        for (int i = 0; i < 3; ++i)
            if (j["world_color"][i].is_number())
                out.world_color[i] = j["world_color"][i].get<float>();
        out.has_world = true;
    }
    if (out.cloth) out.body = "none";  // cloth is simulated, never a collider
    return out;
}

// ---------------------------------------------------------------------------
// Textures. Decode embedded (buffer view) or external (uri) images with stb
// and upload with a CPU mip chain. Cached per (image, srgb) so an image shared
// by several materials uploads once per color space.
// ---------------------------------------------------------------------------
struct TextureCache {
    Dx12Device* dev = nullptr;
    const cgltf_data* data = nullptr;
    std::filesystem::path base_dir;
    std::map<std::pair<const cgltf_image*, bool>, int> by_image;
    std::vector<GpuTexture>* out = nullptr;

    int get(const cgltf_texture* tex, bool srgb) {
        if (!tex || !tex->image) return -1;
        const cgltf_image* img = tex->image;
        auto it = by_image.find({img, srgb});
        if (it != by_image.end()) return it->second;

        int w = 0, h = 0, ch = 0;
        stbi_uc* pixels = nullptr;
        if (img->buffer_view && img->buffer_view->buffer && img->buffer_view->buffer->data) {
            const auto* bytes =
                static_cast<const stbi_uc*>(img->buffer_view->buffer->data) +
                img->buffer_view->offset;
            pixels = stbi_load_from_memory(bytes, static_cast<int>(img->buffer_view->size),
                                           &w, &h, &ch, 4);
        } else if (img->uri && std::strncmp(img->uri, "data:", 5) != 0) {
            const std::string p = (base_dir / img->uri).string();
            pixels = stbi_load(p.c_str(), &w, &h, &ch, 4);
        }
        if (!pixels) {
            HD2D_WARN("glTF texture '{}': decode failed ({})",
                      img->name ? img->name : "?", stbi_failure_reason());
            by_image[{img, srgb}] = -1;
            return -1;
        }
        GpuTexture gpu = upload_texture_rgba8_mips(*dev, static_cast<uint32_t>(w),
                                                   static_cast<uint32_t>(h), pixels, srgb);
        stbi_image_free(pixels);
        if (!gpu.resource) {
            by_image[{img, srgb}] = -1;
            return -1;
        }
        out->push_back(gpu);
        const int idx = static_cast<int>(out->size()) - 1;
        by_image[{img, srgb}] = idx;
        return idx;
    }
};

int sampler_flags_of(const cgltf_texture* tex) {
    int flags = 0;
    if (tex && tex->sampler) {
        const cgltf_sampler* s = tex->sampler;
        if (s->mag_filter == cgltf_filter_type_nearest) flags |= 1;
        if (s->wrap_s == cgltf_wrap_mode_clamp_to_edge ||
            s->wrap_t == cgltf_wrap_mode_clamp_to_edge)
            flags |= 2;
    }
    return flags;
}

LoadedMaterial build_material(const cgltf_material& m, TextureCache& cache) {
    LoadedMaterial out;
    out.name = m.name ? m.name : "";
    if (m.has_pbr_metallic_roughness) {
        const auto& pbr = m.pbr_metallic_roughness;
        for (int c = 0; c < 4; ++c) out.base_color[c] = pbr.base_color_factor[c];
        out.metallic = pbr.metallic_factor;
        out.roughness = pbr.roughness_factor;
        out.tex_base = cache.get(pbr.base_color_texture.texture, /*srgb*/ true);
        out.tex_mr = cache.get(pbr.metallic_roughness_texture.texture, /*srgb*/ false);
        out.sampler_flags = sampler_flags_of(pbr.base_color_texture.texture);
    }
    out.tex_normal = cache.get(m.normal_texture.texture, /*srgb*/ false);
    out.tex_emissive = cache.get(m.emissive_texture.texture, /*srgb*/ true);
    for (int c = 0; c < 3; ++c) out.emissive[c] = m.emissive_factor[c];
    if (m.has_emissive_strength)
        out.emissive_strength = m.emissive_strength.emissive_strength;
    out.alpha_mode = m.alpha_mode == cgltf_alpha_mode_mask    ? 1
                     : m.alpha_mode == cgltf_alpha_mode_blend ? 2
                                                              : 0;
    out.alpha_cutoff = m.alpha_cutoff;
    out.double_sided = m.double_sided;
    return out;
}

// ---------------------------------------------------------------------------
// Geometry. Verts are kept in LOCAL space (node transform stays separate) but
// already converted to engine handedness (z flipped, winding swapped).
// ---------------------------------------------------------------------------

// UV-space tangent generation for meshes without TANGENT (low-poly friendly
// per-triangle accumulation + Gram-Schmidt against the normal).
void generate_tangents(std::vector<Vertex>& verts, const std::vector<uint32_t>& idx) {
    std::vector<dx::XMFLOAT3> tan(verts.size(), {0, 0, 0});
    std::vector<dx::XMFLOAT3> bit(verts.size(), {0, 0, 0});
    for (size_t i = 0; i + 2 < idx.size(); i += 3) {
        const Vertex& v0 = verts[idx[i]];
        const Vertex& v1 = verts[idx[i + 1]];
        const Vertex& v2 = verts[idx[i + 2]];
        const float e1x = v1.px - v0.px, e1y = v1.py - v0.py, e1z = v1.pz - v0.pz;
        const float e2x = v2.px - v0.px, e2y = v2.py - v0.py, e2z = v2.pz - v0.pz;
        const float du1 = v1.u - v0.u, dv1 = v1.v - v0.v;
        const float du2 = v2.u - v0.u, dv2 = v2.v - v0.v;
        const float det = du1 * dv2 - du2 * dv1;
        if (std::fabs(det) < 1e-8f) continue;
        const float r = 1.0f / det;
        const dx::XMFLOAT3 t{(e1x * dv2 - e2x * dv1) * r, (e1y * dv2 - e2y * dv1) * r,
                             (e1z * dv2 - e2z * dv1) * r};
        const dx::XMFLOAT3 b{(e2x * du1 - e1x * du2) * r, (e2y * du1 - e1y * du2) * r,
                             (e2z * du1 - e1z * du2) * r};
        for (int k = 0; k < 3; ++k) {
            auto& ta = tan[idx[i + k]];
            ta.x += t.x; ta.y += t.y; ta.z += t.z;
            auto& ba = bit[idx[i + k]];
            ba.x += b.x; ba.y += b.y; ba.z += b.z;
        }
    }
    for (size_t v = 0; v < verts.size(); ++v) {
        const dx::XMVECTOR n = dx::XMVectorSet(verts[v].nx, verts[v].ny, verts[v].nz, 0);
        dx::XMVECTOR t = dx::XMVectorSet(tan[v].x, tan[v].y, tan[v].z, 0);
        // Gram-Schmidt; fall back to +X if degenerate.
        t = dx::XMVectorSubtract(t, dx::XMVectorScale(n, dx::XMVectorGetX(dx::XMVector3Dot(n, t))));
        if (dx::XMVectorGetX(dx::XMVector3LengthSq(t)) < 1e-10f)
            t = dx::XMVectorSet(1, 0, 0, 0);
        t = dx::XMVector3Normalize(t);
        const dx::XMVECTOR b = dx::XMVectorSet(bit[v].x, bit[v].y, bit[v].z, 0);
        const float sign =
            dx::XMVectorGetX(dx::XMVector3Dot(dx::XMVector3Cross(n, t), b)) < 0.0f ? -1.0f
                                                                                   : 1.0f;
        verts[v].tx = dx::XMVectorGetX(t);
        verts[v].ty = dx::XMVectorGetY(t);
        verts[v].tz = dx::XMVectorGetZ(t);
        verts[v].tw = sign;
    }
}

LoadedPrimitive build_primitive(Dx12Device& dev, Dx12ResourceTable& table, const cgltf_data* data,
                                const cgltf_primitive& prim) {
    LoadedPrimitive out;

    const cgltf_accessor* pos = nullptr;
    const cgltf_accessor* nrm = nullptr;
    const cgltf_accessor* tng = nullptr;
    const cgltf_accessor* uv0 = nullptr;
    const cgltf_accessor* col = nullptr;
    const cgltf_accessor* pin = nullptr;
    for (cgltf_size a = 0; a < prim.attributes_count; ++a) {
        const cgltf_attribute& attr = prim.attributes[a];
        switch (attr.type) {
            case cgltf_attribute_type_position: pos = attr.data; break;
            case cgltf_attribute_type_normal:   nrm = attr.data; break;
            case cgltf_attribute_type_tangent:  tng = attr.data; break;
            case cgltf_attribute_type_texcoord:
                if (attr.index == 0) uv0 = attr.data;
                break;
            case cgltf_attribute_type_color:
                if (attr.index == 0) col = attr.data;
                break;
            case cgltf_attribute_type_custom:
                if (attr.name && upper(attr.name) == "_PIN") pin = attr.data;
                break;
            default: break;
        }
    }
    if (!pos) return out;

    const cgltf_size vcount = pos->count;
    std::vector<Vertex> verts(vcount);
    for (cgltf_size i = 0; i < vcount; ++i) {
        float p[3] = {0, 0, 0}, n[3] = {0, 1, 0}, t[4] = {1, 0, 0, 1};
        float uv[2] = {0, 0}, c[4] = {1, 1, 1, 1};
        cgltf_accessor_read_float(pos, i, p, 3);
        if (nrm) cgltf_accessor_read_float(nrm, i, n, 3);
        if (tng) cgltf_accessor_read_float(tng, i, t, 4);
        if (uv0) cgltf_accessor_read_float(uv0, i, uv, 2);
        if (col) cgltf_accessor_read_float(col, i, c, 4);  // vec3 colors leave a=1
        Vertex& v = verts[i];
        v.px = p[0]; v.py = p[1]; v.pz = -p[2];            // flip Z (handedness)
        v.nx = n[0]; v.ny = n[1]; v.nz = -n[2];
        v.tx = t[0]; v.ty = t[1]; v.tz = -t[2];
        v.tw = -t[3];                                      // mirrored handedness
        v.u = uv[0]; v.v = uv[1];
        v.cr = c[0]; v.cg = c[1]; v.cb = c[2]; v.ca = c[3];
    }

    std::vector<uint32_t> indices;
    if (prim.indices) {
        indices.resize(prim.indices->count);
        for (cgltf_size i = 0; i < prim.indices->count; ++i)
            indices[i] = static_cast<uint32_t>(cgltf_accessor_read_index(prim.indices, i));
    } else {
        indices.resize(vcount);
        for (cgltf_size i = 0; i < vcount; ++i) indices[i] = static_cast<uint32_t>(i);
    }
    // Reverse winding to match the Z flip.
    for (size_t i = 0; i + 2 < indices.size(); i += 3)
        std::swap(indices[i + 1], indices[i + 2]);

    if (!tng && uv0) generate_tangents(verts, indices);

    out.material = prim.material ? static_cast<int>(prim.material - data->materials) : -1;

    // CPU copy for physics / cloth (local engine space).
    out.cpu.positions.resize(vcount);
    for (cgltf_size i = 0; i < vcount; ++i)
        out.cpu.positions[i] = Float3{verts[i].px, verts[i].py, verts[i].pz};
    out.cpu.indices = indices;
    if (pin) {
        out.cpu.pin.resize(vcount);
        for (cgltf_size i = 0; i < vcount; ++i) {
            float w = 0.0f;
            cgltf_accessor_read_float(pin, i, &w, 1);
            out.cpu.pin[i] = w;
        }
    }

    GpuSubmesh sm;
    sm.vb = create_upload_buffer(dev, verts.data(), verts.size() * sizeof(Vertex));
    sm.ib = create_upload_buffer(dev, indices.data(), indices.size() * sizeof(uint32_t));
    out.index_count = static_cast<uint32_t>(indices.size());
    if (!sm.vb || !sm.ib) {
#if defined(HD2D_RENDERER_SDLGPU)
        // SDL_GPU backend: the DX12 upload (create_upload_buffer) is a stub that
        // returns empty resources, but the CPU mesh copy above is real — the
        // geometry pass draws from out.cpu. Register the (empty) submesh so the
        // handle stays valid + index_count is preserved.
        sm.index_count = out.index_count;
        out.mesh = table.add_mesh(std::move(sm));
        return out;
#else
        out.index_count = 0;   // genuine upload failure on DX12
        return out;
#endif
    }
    sm.vbv.BufferLocation = sm.vb->GetGPUVirtualAddress();
    sm.vbv.SizeInBytes = static_cast<UINT>(verts.size() * sizeof(Vertex));
    sm.vbv.StrideInBytes = sizeof(Vertex);
    sm.ibv.BufferLocation = sm.ib->GetGPUVirtualAddress();
    sm.ibv.SizeInBytes = static_cast<UINT>(indices.size() * sizeof(uint32_t));
    sm.ibv.Format = DXGI_FORMAT_R32_UINT;
    sm.index_count = out.index_count;
    out.mesh = table.add_mesh(std::move(sm));   // table(host) owns the resource; store handle
    return out;
}

LoadedLight build_light(const cgltf_light& l) {
    LoadedLight out;
    for (int c = 0; c < 3; ++c) out.color[c] = l.color[c];
    out.range = l.range;
    switch (l.type) {
        case cgltf_light_type_directional:
            out.type = 0;
            out.intensity_w = l.intensity / kWattsToLumens;  // lux -> W/m^2
            break;
        case cgltf_light_type_spot:
            out.type = 2;
            out.intensity_w = l.intensity * 4.0f * kPi / kWattsToLumens;  // cd -> W
            out.inner_cos = std::cos(l.spot_inner_cone_angle);
            out.outer_cos = std::cos(l.spot_outer_cone_angle);
            break;
        case cgltf_light_type_point:
        default:
            out.type = 1;
            out.intensity_w = l.intensity * 4.0f * kPi / kWattsToLumens;  // cd -> W
            break;
    }
    return out;
}

} // namespace

bool load_gltf(Dx12Device& dev, Dx12ResourceTable& table, const std::string& path, LoadedScene& out) {
    cgltf_options options{};
    cgltf_data* data = nullptr;
    if (cgltf_parse_file(&options, path.c_str(), &data) != cgltf_result_success) {
        HD2D_ERROR("glTF parse failed: {}", path);
        return false;
    }
    if (cgltf_load_buffers(&options, data, path.c_str()) != cgltf_result_success) {
        HD2D_ERROR("glTF load buffers failed: {}", path);
        cgltf_free(data);
        return false;
    }

    TextureCache cache;
    cache.dev = &dev;
    cache.data = data;
    cache.base_dir = std::filesystem::path(path).parent_path();
    cache.out = &out.textures;

    std::vector<LoadedMaterial> materials(data->materials_count);
    for (cgltf_size m = 0; m < data->materials_count; ++m)
        materials[m] = build_material(data->materials[m], cache);

    size_t lights = 0, extras_nodes = 0;
    for (cgltf_size n = 0; n < data->nodes_count; ++n) {
        const cgltf_node& node = data->nodes[n];
        if (!node.mesh && !node.light && !node.extras.data) continue;

        LoadedNodeEx ln;
        ln.name = node.name ? node.name : "";
        cgltf_float m16[16];
        cgltf_node_transform_world(&node, m16);
        ln.world = to_engine_matrix(m16);
        ln.extras = parse_extras(node.extras);
        if (node.extras.data) ++extras_nodes;

        if (node.mesh) {
            for (cgltf_size p = 0; p < node.mesh->primitives_count; ++p) {
                LoadedPrimitive prim = build_primitive(dev, table, data, node.mesh->primitives[p]);
                if (prim.index_count > 0) ln.prims.push_back(std::move(prim));
            }
        }
        if (node.light) {
            ln.light = build_light(*node.light);
            ++lights;
        }
        if (!ln.prims.empty() || ln.light || !ln.extras.spawn.empty() || ln.extras.has_world)
            out.nodes.push_back(std::move(ln));
    }

    out.materials = std::move(materials);

    HD2D_INFO("loaded glTF '{}': {} nodes, {} materials, {} textures, {} lights, {} extras",
              path, out.nodes.size(), out.materials.size(), out.textures.size(), lights,
              extras_nodes);
    cgltf_free(data);
    return !out.nodes.empty();
}

} // namespace hd2d
