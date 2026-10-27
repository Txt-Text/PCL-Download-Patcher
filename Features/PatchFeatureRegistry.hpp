#pragma once
#include "IPatchFeature.hpp"
#include <memory>

namespace PclFix {

    // 持有无文件状态的特征对象，所有分析和验证都通过注册接口分派。
    class PatchFeatureRegistry {
    private:
        std::vector<std::unique_ptr<IPatchFeature>> m_features; // 注册对象的唯一所有者。

    public:
        // 注册一个特征；相同身份或空对象会立即拒绝，防止覆盖已有规则。
        void Register(std::unique_ptr<IPatchFeature> feature) {
            if (!feature) throw std::invalid_argument("初始化修补功能失败：功能对象为空。");
            const auto info = feature->Describe(); // 新对象的稳定身份和显示信息。
            if (info.id.empty() || info.name.empty()) throw std::invalid_argument("初始化修补功能失败：缺少功能编号或名称。");
            for (const auto& existing : m_features) {
                if (existing->Describe().id == info.id) throw std::invalid_argument("初始化修补功能失败，功能编号重复：" + info.id);
            }
            m_features.push_back(std::move(feature));
        }

        // 返回只读注册集合，调用者不能转移内部对象的所有权。
        const std::vector<std::unique_ptr<IPatchFeature>>& Features() const { return m_features; }

        // 按稳定身份关联前后结果，不依赖类别数量或注册次序。
        bool Verify(const PatchAnalysis& before, const PatchAnalysis& after) const {
            if (before.ruleId != after.ruleId || before.version != after.version || !before.error.empty() || !after.error.empty() ||
                before.features.size() != m_features.size() || after.features.size() != m_features.size()) return false;
            for (const auto& feature : m_features) {
                const auto info = feature->Describe(); // 本轮验证所对应的注册身份。
                const auto found = std::find_if(before.features.begin(), before.features.end(),
                    [&info](const auto& item) { return item.info.id == info.id; });
                const auto verified = std::find_if(after.features.begin(), after.features.end(),
                    [&info](const auto& item) { return item.info.id == info.id; });
                if (found == before.features.end() || verified == after.features.end()) return false;
                const bool modified = before.CountFeature(info.id) > 0; // 验证范围来自实际修改计划，不能靠扫描数量猜测。
                if (!modified && !(info.required && found->state == FeatureState::Restored)) continue;
                if (verified->state != FeatureState::Restored || verified->patchableCount || !feature->Verify(after, *found)) return false;
            }
            return true;
        }
    };

}
