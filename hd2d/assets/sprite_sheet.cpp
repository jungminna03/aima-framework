#include "assets/sprite_sheet.h"

#include "core/log_compat.h"

#include <stb_image.h>

namespace hd2d {

bool load_sprite_sheet(Dx12Device& dev, Dx12ResourceTable& table, const std::string& path, SpriteSheet& out) {
    out = SpriteSheet{};

    int w = 0, h = 0, ch = 0;
    stbi_uc* pixels = stbi_load(path.c_str(), &w, &h, &ch, 4);
    if (!pixels) {
        HD2D_ERROR("sprite '{}': failed to load image ({})", path, stbi_failure_reason());
        return false;
    }

    // 페이퍼 시트: 1행의 정사각 셀 — frame_px = height, frame_count = width / height.
    if (h <= 0 || w <= 0 || w % h != 0) {
        HD2D_ERROR("sprite '{}': size {}x{} must be a row of square cells (width a multiple of height)",
                   path, w, h);
        stbi_image_free(pixels);
        return false;
    }

    const int frame_px = h / kSheetRows;   // kSheetRows == 1
    out.frame_px = frame_px;
    out.frame_count = w / frame_px;
    GpuTexture tex = upload_texture_rgba8(dev, static_cast<uint32_t>(w), static_cast<uint32_t>(h),
                                          pixels);
    stbi_image_free(pixels);

    if (!tex.resource) {
#if defined(HD2D_RENDERER_SDLGPU)
        // Expected on the sdlgpu backend: upload_texture_rgba8 is the inert DX12
        // stub, so there's never a GPU `resource` here. The live billboard path
        // loads the sheet itself (by path); the ECS only needs frame_px/
        // frame_count — and `valid`, because the paper-animation + facing system
        // gates on it. Leaving it false froze cur_frame at 0 on mac.
        out.valid = true;
        HD2D_INFO("loaded sprite '{}' ({}x{}, {} pose cols, frame={}px, sdlgpu live path)",
                  path, w, h, out.frame_count, frame_px);
        return true;
#else
        HD2D_ERROR("sprite '{}': GPU upload failed", path);
        return false;
#endif
    }
    out.texture = table.add_texture(tex);   // host table owns it; store opaque handle
    out.valid = true;
    HD2D_INFO("loaded sprite '{}' ({}x{}, {} pose cols, frame={}px)", path, w, h,
              out.frame_count, frame_px);
    return true;
}

} // namespace hd2d
