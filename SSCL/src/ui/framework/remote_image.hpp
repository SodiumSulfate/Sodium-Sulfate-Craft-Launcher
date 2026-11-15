#pragma once

// 远程图标加载（design/mod-download.md §8.7；PCL ResourceProject.LogoUrl 对齐）：
// URL → 内存 LRU(64) → 磁盘 <data>/cache/icons/<sha1(url)>.img → WIC 解码。
// 后台线程取字节（HTTPS、8s 超时、≤2MB），完成后经 post_ui 回调重绘；
// 失败标记 failed，由调用方画首字母占位。所有公开方法线程安全。

#include <d2d1.h>
#include <wrl/client.h>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace sscl::ui {

class RemoteImage {
public:
    static RemoteImage& instance();

    // 由程序入口在窗口创建后调用一次；post_ui 负责把回调切回 UI 线程。
    void configure(std::filesystem::path cache_dir,
                   std::function<void(std::function<void()>)> post_ui);

    // 请求图标；已缓存/在途时直接返回。on_ready 在加载完成后经 post_ui 调用一次。
    void request(const std::string& url, std::function<void()> on_ready);

    // 返回可绘制位图（渲染目标变化时重新解码）；未就绪/失败返回空。
    Microsoft::WRL::ComPtr<ID2D1Bitmap> bitmap(ID2D1RenderTarget* target,
                                                const std::string& url);

    void clear();

private:
    struct Entry {
        std::vector<std::uint8_t> bytes;
        bool failed = false;
        ID2D1RenderTarget* target = nullptr;
        Microsoft::WRL::ComPtr<ID2D1Bitmap> image;
    };

    std::filesystem::path cache_file(const std::string& url) const;
    std::vector<std::uint8_t> fetch(const std::string& url) const;
    void touch(const std::string& url);

    mutable std::mutex m_mutex;
    std::filesystem::path m_cache_dir;
    std::function<void(std::function<void()>)> m_post_ui;
    std::unordered_map<std::string, Entry> m_entries;
    std::unordered_set<std::string> m_inflight;
    std::vector<std::string> m_lru;
};

}  // namespace sscl::ui
