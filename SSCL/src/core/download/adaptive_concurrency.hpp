#pragma once

// 自适应并发控制（批次 42，D-061）：文件级下载的并发窗口。
// 目标：小文件多时尽量占满带宽，又不因连接数失控拖垮程序。
// 策略：AIMD 变体——吞吐比历史最佳提升 ≥ grow_ratio 时加档（+25%，上限封顶），
// 连续 shrink_after_samples 次低于历史最佳 × shrink_ratio 时降档（-25%，下限兜底）；
// 网络错误/超时立即减半。纯逻辑、无 I/O，便于单元测试。

#include <cstdint>

namespace sscl::core {

struct AdaptiveConcurrencyConfig {
    int min_concurrent = 4;
    int max_concurrent = 32;
    int start_concurrent = 8;
    int sample_ms = 500;           // 采样窗口（调用方负责按此间隔喂 sample）
    double grow_ratio = 1.05;      // 比历史最佳快 5% → 加档
    double shrink_ratio = 0.70;    // 低于历史最佳 70% 视为拥塞
    int shrink_after_samples = 2;  // 连续多少次拥塞采样才降档
};

class AdaptiveConcurrency {
public:
    explicit AdaptiveConcurrency(AdaptiveConcurrencyConfig config = {});

    // 当前目标并发（正在下载的文件数上限）。
    int target() const { return m_target; }
    const AdaptiveConcurrencyConfig& config() const { return m_config; }

    // 输入一个采样窗口的聚合吞吐（字节/秒）与剩余待下载文件数；返回新的目标并发。
    // remaining_files 只用于避免文件快下完时无意义地加档。
    int sample(double bytes_per_second, std::uint64_t remaining_files);

    // 网络错误/超时：立即减半并重置历史最佳（回到保守档位）。
    int on_error();

private:
    int clamp_target(int value) const;
    int step_up(int value) const;
    int step_down(int value) const;

    AdaptiveConcurrencyConfig m_config;
    int m_target = 8;
    double m_best_speed = 0.0;  // 历史最佳采样吞吐（B/s）
    int m_low_streak = 0;       // 连续低于 shrink_ratio 的采样次数
};

}  // namespace sscl::core
