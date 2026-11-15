#include "core/download/adaptive_concurrency.hpp"

#include <algorithm>

namespace sscl::core {

AdaptiveConcurrency::AdaptiveConcurrency(AdaptiveConcurrencyConfig config)
    : m_config(config) {
    if (m_config.min_concurrent < 1) {
        m_config.min_concurrent = 1;
    }
    if (m_config.max_concurrent < m_config.min_concurrent) {
        m_config.max_concurrent = m_config.min_concurrent;
    }
    if (m_config.start_concurrent <= 0) {
        m_config.start_concurrent = m_config.min_concurrent;
    }
    if (m_config.shrink_after_samples < 1) {
        m_config.shrink_after_samples = 1;
    }
    m_target = clamp_target(m_config.start_concurrent);
}

int AdaptiveConcurrency::clamp_target(int value) const {
    return (std::max)(m_config.min_concurrent, (std::min)(m_config.max_concurrent, value));
}

int AdaptiveConcurrency::step_up(int value) const {
    return clamp_target(value + (std::max)(1, value / 4));
}

int AdaptiveConcurrency::step_down(int value) const {
    return clamp_target(value - (std::max)(1, value / 4));
}

int AdaptiveConcurrency::sample(double bytes_per_second, std::uint64_t remaining_files) {
    // 没有吞吐数据（刚启动/全部阻塞）或已经在下最后几个文件：维持现状。
    if (bytes_per_second <= 0.0) {
        return m_target;
    }
    if (remaining_files > 0 && remaining_files <= static_cast<std::uint64_t>(m_target)) {
        return m_target;
    }

    if (m_best_speed <= 0.0) {
        // 第一个有效采样只建立基线。
        m_best_speed = bytes_per_second;
        m_low_streak = 0;
        return m_target;
    }

    if (bytes_per_second > m_best_speed * m_config.grow_ratio) {
        m_best_speed = bytes_per_second;
        m_low_streak = 0;
        m_target = step_up(m_target);
        return m_target;
    }

    if (bytes_per_second < m_best_speed * m_config.shrink_ratio) {
        ++m_low_streak;
        if (m_low_streak >= m_config.shrink_after_samples) {
            m_low_streak = 0;
            // 以当前吞吐作为新基线，避免降档后立刻再次触发降档。
            m_best_speed = bytes_per_second;
            m_target = step_down(m_target);
        }
        return m_target;
    }

    // 缓慢提升也计入历史最佳（避免基线过时），但不足以触发加档。
    m_best_speed = (std::max)(m_best_speed, bytes_per_second);
    m_low_streak = 0;
    return m_target;
}

int AdaptiveConcurrency::on_error() {
    m_best_speed = 0.0;
    m_low_streak = 0;
    m_target = clamp_target(m_target / 2);
    return m_target;
}

}  // namespace sscl::core
