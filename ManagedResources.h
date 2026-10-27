#pragma once
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <stdexcept>
#include <string_view>

namespace PclFix {
    // 仅解析 .resources v2 的目录与内置 ByteArray 类型，不反序列化对象，也不执行依赖程序集。
    class ManagedResourceReader {
    private:
        std::span<const unsigned char> m_bytes; // 借用 PE 中的资源字节，生命周期由分析器保证。

        // 统一校验范围，防止资源目录中的偏移或长度越界。
        void Check(size_t offset, size_t length) const {
            if (offset > m_bytes.size() || length > m_bytes.size() - offset) throw std::runtime_error("程序内置资源的读取位置超出文件范围，文件可能已损坏。");
        }

        // 安全读取未对齐的小端整数。
        uint32_t Word(size_t offset) const {
            Check(offset, 4); uint32_t value = 0; std::memcpy(&value, m_bytes.data() + offset, 4); return value;
        }

        // .resources 字符串长度和类型编号使用 BinaryReader 的七位整数格式。
        uint32_t Number(size_t& cursor) const {
            uint32_t result = 0;
            for (unsigned shift = 0; shift < 35; shift += 7) {
                Check(cursor, 1); const unsigned value = m_bytes[cursor++];
                if (shift == 28 && value > 15) throw std::runtime_error("程序内置资源中的长度数据过大，无法读取。");
                result |= (value & 127) << shift;
                if (!(value & 128)) return result;
            }
            throw std::runtime_error("程序内置资源中的长度数据不完整，文件可能已损坏。");
        }

    public:
        // 不复制或持有目标的所有权。
        explicit ManagedResourceReader(std::span<const unsigned char> bytes) : m_bytes(bytes) {}

        // 按 Unicode 资源名读取原始字节数组；未找到时返回空 optional。
        std::optional<std::span<const unsigned char>> ByteArray(std::wstring_view name) const {
            if (Word(0) != 0xbeefcace || Word(4) < 1) throw std::runtime_error("程序内置资源的文件格式无法识别。");
            size_t cursor = 12; const uint32_t headerLength = Word(8); Check(cursor, headerLength); cursor += headerLength;
            if (Word(cursor) != 2) throw std::runtime_error("程序内置资源的保存格式暂不受支持。");
            const uint32_t count = Word(cursor + 4), types = Word(cursor + 8); cursor += 12;
            if (count > m_bytes.size() / 8 || types > m_bytes.size()) throw std::runtime_error("程序内置资源的数量记录无效，文件可能已损坏。");
            for (uint32_t index = 0; index < types; ++index) { const auto length = Number(cursor); Check(cursor, length); cursor += length; }
            cursor = (cursor + 7) & ~size_t(7); Check(cursor, static_cast<size_t>(count) * 8 + 4);
            const size_t positions = cursor + count * 4; cursor += static_cast<size_t>(count) * 8;
            const size_t dataSection = Word(cursor); cursor += 4; const size_t names = cursor;
            if (dataSection < names || dataSection > m_bytes.size()) throw std::runtime_error("程序内置资源的保存位置无效，文件可能已损坏。");
            for (uint32_t index = 0; index < count; ++index) {
                const uint32_t offset = Word(positions + index * 4);
                if (offset > dataSection - names) throw std::runtime_error("无法找到程序内置资源的名称，文件可能已损坏。");
                size_t item = names + offset; const uint32_t length = Number(item); Check(item, length);
                if (item > dataSection || length > dataSection - item || dataSection - item - length < 4 || length % 2) throw std::runtime_error("程序内置资源的名称数据无效，文件可能已损坏。");
                bool matches = length / 2 == name.size();
                for (size_t character = 0; matches && character < name.size(); ++character) {
                    const unsigned value = m_bytes[item + character * 2] | (static_cast<unsigned>(m_bytes[item + character * 2 + 1]) << 8);
                    if (value != static_cast<unsigned>(name[character])) matches = false;
                }
                if (!matches) continue;
                const uint32_t relative = Word(item + length); Check(dataSection, relative); item = dataSection + relative;
                if (Number(item) != 32) throw std::runtime_error("内置依赖文件的保存方式暂不受支持。");
                const uint32_t size = Word(item); item += 4; Check(item, size);
                return m_bytes.subspan(item, size);
            }
            return std::nullopt;
        }
    };
}
