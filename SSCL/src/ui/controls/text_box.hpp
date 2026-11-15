#pragma once

// 单行文本框（§8 controls 雏形）：聚焦边框、尾部光标、退格编辑。
// 输入文本来自 AppWindow 的 WM_CHAR（IME 合成结果由系统转为 WM_CHAR）。
// 光标恒在末尾（M1 范围；完整光标移动/选择留待 §8 框架完善）。
//
// 编辑快捷键（D-120）：Ctrl+V 粘贴、Ctrl+C 复制、Ctrl+X 剪切。
// 粘贴内容按 max_length 截断并过滤控制字符（换行/制表符不进单行框）；
// 密码框禁止复制/剪切，避免明文外泄。

#include <string>

#include "ui/framework/ui_element.hpp"

namespace sscl::ui {

class TextBox : public UiElement {
public:
    explicit TextBox(std::wstring text = {}, float font_size = 13.0f);

    void draw(ID2D1RenderTarget* target, IDWriteFactory* dwrite) override;

    // 字符输入；返回是否产生视觉变化。
    bool handle_char(wchar_t ch);
    // 按键编辑（VK_BACK/VK_RETURN + Ctrl 组合键）；返回是否产生视觉变化。
    bool handle_key_down(unsigned long key);

    // 写入一段文本（过滤控制字符、按剩余长度截断）；返回是否产生视觉变化。
    // 供粘贴与程序化填充共用，保证两条路径的清洗/截断规则一致。
    bool insert_text(const std::wstring& text);
    // Ctrl+V / Ctrl+C / Ctrl+X；密码框的复制/剪切直接返回 false。
    bool paste_from_clipboard();
    bool copy_to_clipboard() const;
    bool cut_to_clipboard();

    void set_focused(bool focused) { m_focused = focused; }
    bool is_focused() const { return m_focused; }

    std::wstring text;
    std::wstring placeholder;  // 空文本时显示的灰字
    std::size_t max_length = 16;
    bool password = false;     // D-118：密码框只显示掩码（●），text 仍是明文

private:
    float m_font_size;
    bool m_focused = false;
};

}  // namespace sscl::ui
