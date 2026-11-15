#pragma once

// SVG 图标 → Direct2D 路径几何。
// - SVG 以 RCDATA 嵌入 EXE（assets/icons/*.svg），不依赖外部文件；
// - 支持填充型（Material Symbols 的 <path>）与描边型（version-*.svg 的
//   fill="none" stroke="currentColor" + <rect>/<circle>/<path>）两类；
// - 每个图元按需 FillGeometry 或 DrawGeometry（round cap/join），统一用调用方颜色；
// - 解析 path d + viewBox，按目标尺寸矢量缩放；失败只记日志并返回 false，不崩溃。
//
// D-119：缓存按 ID2D1Factory 分桶。
// 本进程有多个窗口（主窗口 + 账号管理子窗口），每个 Renderer 各自 CreateFactory；
// 早先的实现「工厂一变就清空全部缓存」会让两个窗口**每帧互相洗掉**对方解析好的
// 图标几何 —— 每次重绘都要重新解析 SVG（建几何 + 走路径解析器），表现为子窗口
// 点按按钮时概率性卡顿（取决于这期间哪个窗口刚重绘过）。现在每个工厂各存一份，
// 互不干扰；几何始终由创建它的工厂渲染，不做跨工厂复用。

#include <d2d1.h>
#include <wrl/client.h>

#include <cstddef>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace sscl::ui {

class IconManager {
public:
    static IconManager& instance();

    // 在 rect 内等比居中绘制；color 为图标填充色，opacity 可做禁用态。
    bool draw(ID2D1RenderTarget* target, const std::string& name,
              const D2D1_RECT_F& rect, const D2D1_COLOR_F& color, float opacity = 1.0f);

    // 是否已成功解析过该图标（任一分桶命中即可）。
    bool has(const std::string& name) const;

private:
    // 一个 SVG 可能同时含填充型与描边型图元（如 version-neoforge.svg 的轮廓 + 眼睛）。
    // stroke=true 用 DrawGeometry 画描边，false 用 FillGeometry 画实心。
    struct Shape {
        Microsoft::WRL::ComPtr<ID2D1PathGeometry> geometry;
        bool stroke = false;
        float stroke_width = 2.0f;  // viewBox 单位，随整体缩放
    };
    struct Icon {
        std::vector<Shape> shapes;
        float view_x = 0.0f;
        float view_y = 0.0f;
        float view_w = 24.0f;
        float view_h = 24.0f;
    };

    // 同一工厂的解析结果（几何必须与创建它的工厂配套）。
    struct IconSet {
        Microsoft::WRL::ComPtr<ID2D1Factory> factory;
        Microsoft::WRL::ComPtr<ID2D1StrokeStyle> stroke_style;
        std::unordered_map<std::string, Icon> icons;
        std::unordered_map<std::string, bool> missing;
    };

    IconManager() = default;

    // 取出（或新建）target 所属工厂的分桶。窗口销毁后其分桶不再命中，
    // 超过上限时按「非当前」优先淘汰，几何随分桶一起释放。
    IconSet* set_for(ID2D1RenderTarget* target);
    const Icon* get_in(IconSet* set, const std::string& name);
    bool load(IconSet* set, const std::string& name, Icon* icon);
    bool parse_svg(IconSet* set, const std::string& svg, Icon* icon);

    // 最多保留的分桶数：主窗口 + 账号子窗口 + 余量。unique_ptr 保证 Icon* 稳定。
    static constexpr std::size_t kMaxSets = 4;

    std::vector<std::unique_ptr<IconSet>> m_sets;
    IconSet* m_active = nullptr;
};

}  // namespace sscl::ui
