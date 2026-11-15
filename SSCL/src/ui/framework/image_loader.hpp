#pragma once

// PNG → ID2D1Bitmap（WIC 解码；皮肤头像显示用，C-6）。
// 只在 UI 层使用；解码失败返回 nullptr（调用方画占位）。

#include <d2d1.h>
#include <wrl/client.h>

#include <cstdint>
#include <vector>

namespace sscl::ui {

// 将内存中的 PNG/JPEG 图像解码为位图（目标渲染器绑定的像素格式 BGRA）。
// 失败（损坏/不支持）返回空指针。
Microsoft::WRL::ComPtr<ID2D1Bitmap> load_bitmap_from_bytes(ID2D1RenderTarget* target,
                                                           const std::vector<std::uint8_t>& bytes);

// ICO 多帧解码：取 ≥ desired_px 的最小帧，否则最大帧（图标资源用）。
Microsoft::WRL::ComPtr<ID2D1Bitmap> load_bitmap_from_ico(ID2D1RenderTarget* target,
                                                         const std::vector<std::uint8_t>& bytes,
                                                         unsigned int desired_px);

// 皮肤布局（D-122，PCL MySkin.xaml.vb::Load 同款）：
// scale = width/64（高清皮肤放大系数）、hat_layer = 是否存在有效帽子/头发层。
struct SkinLayout {
    unsigned int width = 0;
    unsigned int height = 0;
    float scale = 1.0f;
    bool hat_layer = false;
};

// 解码 PNG 像素并判断布局；失败返回 false（调用方按单层处理）。
bool analyze_skin_layout(const std::vector<std::uint8_t>& bytes, SkinLayout* out);

}  // namespace sscl::ui
