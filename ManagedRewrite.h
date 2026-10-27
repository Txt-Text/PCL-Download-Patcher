#pragma once
#include <windows.h>
#include <cor.h>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <map>
#include <limits>
#include <span>
#include <stdexcept>
#include <vector>

namespace PclFix {
    // 小型 CIL 汇编器：标签代表控制流边界，统一使用长跳转，避免插入代码后短跳转溢出。
    class IlAssembler {
    private:
        struct Node {
            int label = -1; // 非负标签对应原方法偏移；负数标签由重建规则分配。
            bool isLabel = false; // 标签不占用 IL 字节。
            unsigned opcode = 0; // 标准的一字节或 FE 前缀指令。
            std::vector<unsigned char> operand; // 非跳转指令的原始操作数。
            std::vector<int> targets; // 分支目标，switch 可以含有多个目标。
        };
        std::vector<Node> m_nodes; // 保持源程序顺序的指令和标签。
        int m_nextLabel = -2; // -1 保留给非标签节点的缺省值。

        // 将小整数写成小端字节，不依赖对齐或类型别名转换。
        template<class T> static void Append(std::vector<unsigned char>& output, T value) {
            const auto* bytes = reinterpret_cast<const unsigned char*>(&value);
            output.insert(output.end(), bytes, bytes + sizeof(T));
        }

        // 返回指令编码长度，所有分支在布局阶段就已经具有最终宽度。
        static size_t Size(const Node& node) {
            if (node.isLabel) return 0;
            if (node.opcode == 0x45) return 5 + node.targets.size() * 4;
            return (node.opcode > 0xff ? 2 : 1) + (node.targets.empty() ? node.operand.size() : 4);
        }

    public:
        // 创建仅属于本次重建的新标签。
        int Label() { return m_nextLabel--; }

        // 在当前位置定义标签，重复标签会在输出阶段被拒绝。
        void Mark(int label) { Node node; node.label = label; node.isLabel = true; m_nodes.push_back(std::move(node)); }

        // 添加不带操作数的指令。
        void Op(unsigned opcode) { Raw(opcode, {}); }

        // 添加普通指令及其操作数；源方法的非分支指令可以直接复用。
        void Raw(unsigned opcode, std::span<const unsigned char> operand) {
            Node node; node.opcode = opcode; node.operand.assign(operand.begin(), operand.end()); m_nodes.push_back(std::move(node));
        }

        // 添加元数据令牌、四字节整数等小端操作数。
        void Token(unsigned opcode, uint32_t token) {
            Raw(opcode, {reinterpret_cast<const unsigned char*>(&token), sizeof(token)});
        }

        // 添加整数常量，使用固定长度编码以简化重建和校验。
        void Integer(int value) { Token(0x20, static_cast<uint32_t>(value)); }

        // 加载或存储局部变量，索引来自当前目标方法的签名。
        void Local(bool store, unsigned index) {
            if (index < 4) Op((store ? 0x0a : 0x06) + index);
            else if (index < 256) { const unsigned char value = static_cast<unsigned char>(index); Raw(store ? 0x13 : 0x11, {&value, 1}); }
            else { const uint16_t value = static_cast<uint16_t>(index); Raw(store ? 0xfe0e : 0xfe0c, {reinterpret_cast<const unsigned char*>(&value), 2}); }
        }

        // 添加分支，同时把短操作码规范化成长操作码。
        void Branch(unsigned opcode, int target) {
            if (opcode >= 0x2b && opcode <= 0x37) opcode += 0x0d;
            if (opcode == 0xde) opcode = 0xdd;
            Node node; node.opcode = opcode; node.targets = {target}; m_nodes.push_back(std::move(node));
        }

        // 保留源方法的 switch 分发表。
        void Switch(std::vector<int> targets) {
            Node node; node.opcode = 0x45; node.targets = std::move(targets); m_nodes.push_back(std::move(node));
        }

        // 汇编并返回标签的新偏移；异常处理区域也使用同一映射重定位。
        std::vector<unsigned char> Build(std::map<int, uint32_t>& offsets) const {
            size_t length = 0; // 第一遍计算所有标签和指令位置。
            for (const auto& node : m_nodes) {
                if (node.isLabel && !offsets.emplace(node.label, static_cast<uint32_t>(length)).second) throw std::runtime_error("生成的补丁包含重复的跳转标记，已停止修补。");
                length += Size(node);
                if (length > static_cast<size_t>((std::numeric_limits<int>::max)())) throw std::runtime_error("生成的补丁代码过大，无法写入。");
            }
            std::vector<unsigned char> output; output.reserve(length); // 第二遍填入相对跳转位移。
            for (const auto& node : m_nodes) {
                if (node.isLabel) continue;
                const size_t end = output.size() + Size(node);
                if (node.opcode > 0xff) output.push_back(0xfe);
                output.push_back(static_cast<unsigned char>(node.opcode));
                if (node.opcode == 0x45) Append(output, static_cast<uint32_t>(node.targets.size()));
                if (node.targets.empty()) output.insert(output.end(), node.operand.begin(), node.operand.end());
                for (const int target : node.targets) {
                    const auto found = offsets.find(target);
                    if (found == offsets.end()) throw std::runtime_error("修改后的代码存在无效跳转或异常处理位置，已停止修补。");
                    Append(output, static_cast<int32_t>(static_cast<int64_t>(found->second) - static_cast<int64_t>(end)));
                }
            }
            return output;
        }
    };
}
