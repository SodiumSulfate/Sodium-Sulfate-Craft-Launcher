#include "ui/controls/skin_avatar.hpp"

#include <algorithm>

#include <d2d1helper.h>

#include "ui/framework/anim.hpp"
#include "ui/framework/image_loader.hpp"
#include "ui/framework/motion.hpp"
#include "ui/theme/theme.hpp"

namespace sscl::ui {

void SkinAvatar::set_png(std::vector<std::uint8_t> png_bytes) {
    const bool had_skin = !m_png.empty();
    m_png = std::move(png_bytes);
    m_bitmap.Reset();
    m_bitmap_target = nullptr;
    m_scale = 1.0f;
    m_hat_layer = false;
    if (!m_png.empty()) {
        // D-122：解码一次像素，拿到高清缩放系数与「是否有帽子层」，
        // 避免每帧重新解析 PNG（也避免把 64×32 老皮肤的错位色块盖到脸上）。
        SkinLayout layout;
        if (analyze_skin_layout(m_png, &layout)) {
            m_scale = layout.scale > 0.0f ? layout.scale : 1.0f;
            m_hat_layer = layout.hat_layer;
        }
    }
    // D-033：新皮肤淡入呈现（占位切换保持即时，避免启动首帧空头像）。
    if (!m_png.empty()) {
        m_fade = had_skin ? 0.15f : 0.0f;  // 换肤轻微淡入；首次加载完整淡入
    } else {
        m_fade = 1.0f;
    }
}

bool SkinAvatar::tick(float dt_ms) {
    const bool hover = UiElement::tick(dt_ms);
    const bool fading = motion::tween(m_fade, 1.0f, dt_ms, motion::Kind::kFade);
    return hover || fading;
}

void SkinAvatar::draw(ID2D1RenderTarget* target, IDWriteFactory* dwrite) {
    (void)dwrite;
    if (target == nullptr) {
        return;
    }
    const D2D1_RECT_F dest = bounds;
    const float width = dest.right - dest.left;
    const float height = dest.bottom - dest.top;
    if (width <= 0.0f || height <= 0.0f) {
        return;
    }
    // D-122：圆角正方形（不是圆形）。半径按控件尺寸收敛，避免小头像变成椭圆。
    const float radius = (std::min)(kCornerRadius, (std::min)(width, height) * 0.5f);
    const D2D1_ROUNDED_RECT rounded = D2D1::RoundedRect(dest, radius, radius);

    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush;
    if (FAILED(target->CreateSolidColorBrush(theme_container(), brush.GetAddressOf()))) {
        return;
    }
    // 底色也是圆角方形：四个角透明，露出列表行/账号卡的父级背景。
    target->FillRoundedRectangle(rounded, brush.Get());

    if (!m_png.empty()) {
        if (m_bitmap == nullptr || m_bitmap_target != target) {
            m_bitmap = load_bitmap_from_bytes(target, m_png);
            m_bitmap_target = target;
        }
        if (m_bitmap != nullptr) {
            Microsoft::WRL::ComPtr<ID2D1Factory> factory;
            target->GetFactory(factory.GetAddressOf());
            Microsoft::WRL::ComPtr<ID2D1RoundedRectangleGeometry> clip;
            Microsoft::WRL::ComPtr<ID2D1Layer> layer;
            const bool can_clip =
                factory != nullptr &&
                SUCCEEDED(factory->CreateRoundedRectangleGeometry(rounded, clip.GetAddressOf())) &&
                SUCCEEDED(target->CreateLayer(layer.GetAddressOf()));
            if (can_clip) {
                // Layer 的 opacity 统一驱动两层皮肤淡入，避免两层各自半透明叠加。
                const D2D1_LAYER_PARAMETERS params = D2D1::LayerParameters(
                    dest, clip.Get(), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                    D2D1::Matrix3x2F::Identity(), m_fade, nullptr, D2D1_LAYER_OPTIONS_NONE);
                target->PushLayer(params, layer.Get());
            } else {
                // 极少数设备不支持 Layer：退化为直角裁剪，至少保证头像内容正确。
                target->PushAxisAlignedClip(dest, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
            }

            const float s = m_scale > 0.0f ? m_scale : 1.0f;
            // PCL：脸层 (8,8)-(16,16)，帽子层 (40,8)-(48,16)；高清皮肤整体乘 s。
            const D2D1_RECT_F face = D2D1::RectF(8.0f * s, 8.0f * s, 16.0f * s, 16.0f * s);
            target->DrawBitmap(m_bitmap.Get(), dest, can_clip ? 1.0f : m_fade,
                               D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR, face);
            if (m_hat_layer) {
                const D2D1_RECT_F hat = D2D1::RectF(40.0f * s, 8.0f * s, 48.0f * s, 16.0f * s);
                target->DrawBitmap(m_bitmap.Get(), dest, can_clip ? 1.0f : m_fade,
                                   D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR, hat);
            }

            if (can_clip) {
                target->PopLayer();
            } else {
                target->PopAxisAlignedClip();
            }
        }
    }

    // 描边与遮罩同形（圆角正方形），皮肤与占位保持一致。
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> border;
    if (SUCCEEDED(target->CreateSolidColorBrush(theme_border(), border.GetAddressOf()))) {
        target->DrawRoundedRectangle(rounded, border.Get(), 1.0f);
    }
}

}  // namespace sscl::ui
