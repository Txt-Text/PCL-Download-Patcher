#pragma once
#include "IPatchFeature.hpp"
#include "DownloadPageLoaderFeature.hpp"
#include "ModpackPickerFeature.hpp"
#include "ModpackDispatcherFeature.hpp"
#include "ModifyMinecraftCardFeature.hpp"
#include "ResourceInstallFeature.hpp"
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

        // 内置功能仅在此注册；程序启动时初始化，后续各文件复用同一组规则。
        static const PatchFeatureRegistry& BuiltIn() {
            static const PatchFeatureRegistry registry = [] {
                PatchFeatureRegistry result; // 初始化完成后只读发布的注册集合。
                result.Register(std::make_unique<DownloadPageLoaderFeature>());
                result.Register(std::make_unique<ModpackPickerFeature>());
                result.Register(std::make_unique<ModpackDispatcherFeature>());
                result.Register(std::make_unique<ModifyMinecraftCardFeature>());
                result.Register(std::make_unique<ResourceInstallFeature>());
                return result;
            }();
            return registry;
        }

        // 按稳定身份关联前后结果，不依赖类别数量或注册次序。
        bool Verify(const PatchAnalysis& before, const PatchAnalysis& after) const {
            if (!before.error.empty() || !after.error.empty() || after.TotalPoints() ||
                before.features.size() != m_features.size() || after.features.size() != m_features.size()) return false;
            for (const auto& feature : m_features) {
                const auto info = feature->Describe(); // 本轮验证所对应的注册身份。
                const auto found = std::find_if(before.features.begin(), before.features.end(),
                    [&info](const auto& item) { return item.info.id == info.id; });
                const auto verified = std::find_if(after.features.begin(), after.features.end(),
                    [&info](const auto& item) { return item.info.id == info.id; });
                if (found == before.features.end() || verified == after.features.end() ||
                    found->state == FeatureState::Unsupported || verified->state == FeatureState::Unsupported ||
                    verified->state == FeatureState::Restricted || verified->patchableCount ||
                    (info.required && verified->state != FeatureState::Restored) ||
                    !feature->Verify(after, *found)) return false;
            }
            return true;
        }
    };

}
