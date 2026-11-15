#pragma once

// 皮肤头像（C-6 / D-122，PCL 式头像显示）：
// 输入皮肤 PNG 字节，按 PCL MySkin.xaml.vb::Load 的裁切规则显示头部：
//   脸层  (8*s, 8*s)-(16*s, 16*s)
//   帽子层(40*s, 8*s)-(48*s, 16*s)，仅当皮肤确实含有效第二层时叠加
//   s = width/64（兼容 128×128 高清皮肤与 64×32 老皮肤）
// 整块用圆角正方形几何遮罩裁剪（不是圆形），无皮肤时画占位。

#include <cstdint>
#include <vector>

#include "ui/framework/ui_element.hpp"

namespace sscl::ui {

class SkinAvatar : public UiElement {
public:
    // 圆角正方形半径（DIP），与 theme_radius() 的 8 对齐；主页账号卡与账号
    // 子窗口列表共用，保证形状一致。注意远小于半宽，不会退化成圆形。
    static constexpr float kCornerRadius = 8.0f;

    // 设置皮肤 PNG 字节（解析布局、清空旧位图缓存；非空皮肤以淡入呈现，D-033）。
    void set_png(std::vector<std::uint8_t> png_bytes);

    bool has_skin() const { return !m_png.empty(); }

    // 画到 bounds（方形最佳）；target 变化时重建位图。
    void draw(ID2D1RenderTarget* target, IDWriteFactory* dwrite) override;

    // 动画帧：推进皮肤淡入（D-033）。
    bool tick(float dt_ms) override;

private:
    std::vector<std::uint8_t> m_png;
    Microsoft::WRL::ComPtr<ID2D1Bitmap> m_bitmap;
    ID2D1RenderTarget* m_bitmap_target = nullptr;  // 位图绑定的渲染目标（变化时重建）
    float m_fade = 1.0f;  // 皮肤淡入 0..1（1=完全显示；换肤时归 0 渐入）
    float m_scale = 1.0f;      // width/64，高清皮肤裁切系数
    bool m_hat_layer = false;  // 是否存在有效帽子/头发层（PCL 判定）
};

}  // namespace sscl::ui
