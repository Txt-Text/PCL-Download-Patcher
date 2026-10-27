#pragma once
#include <compare>
#include <cstdint>
#include <string>

namespace PclFix {
    // EXE 中的程序集版本；按四段数字比较，不能用字符串比较版本先后。
    struct TargetVersion {
        uint16_t major = 0; // 主版本。
        uint16_t minor = 0; // 次版本。
        uint16_t build = 0; // 第三段版本。
        uint16_t revision = 0; // 第四段版本。

        auto operator<=>(const TargetVersion&) const = default;

        // 显示真实程序集版本，不依赖用户修改的文件名。
        std::string ToString() const {
            return std::to_string(major) + "." + std::to_string(minor) + "." +
                std::to_string(build) + "." + std::to_string(revision);
        }
    };
}
