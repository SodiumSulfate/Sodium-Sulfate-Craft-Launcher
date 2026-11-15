#pragma once

// 动画语言（D-020 引入 / D-033 固化为配套规则）：
// - tween：线性步进逼近目标值（dt/duration），约 90~180ms，返回是否仍在动画；
// - ease_out：二次缓出（页面/弹窗滑入缩放）。
// 新增或修改 GUI 逻辑时必须配套动画（悬停渐变、开关淡入淡出、内容切换淡入等），
// 统一使用本头的 tween/ease_out 与上述时长档位，见 knowledge/ui-animations.md。

namespace sscl::ui {

// 渐变推进：value 向 target 步进（step = dt_ms / duration_ms）；返回是否仍在动画。
inline bool tween(float& value, float target, float dt_ms, float duration_ms = 90.0f) {
    const float step = dt_ms / duration_ms;
    if (value < target) {
        value = target < value + step ? target : value + step;
    } else if (value > target) {
        value = target > value - step ? target : value - step;
    }
    return value != target;
}

// ease-out（二次）：0..1 输入，起步快收尾缓。
inline float ease_out(float t) {
    const float c = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
    return 1.0f - (1.0f - c) * (1.0f - c);
}

// 钳制到 0..1（动画混合系数用）。
inline float clamp01(float v) {
    return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

}  // namespace sscl::ui
