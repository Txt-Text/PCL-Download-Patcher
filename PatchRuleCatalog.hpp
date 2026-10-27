#pragma once
#include "Features/PatchFeatureRegistry.hpp"
#include "Features/DownloadPageLoaderFeature.hpp"
#include "Features/ModpackPickerFeature.hpp"
#include "Features/ModpackDispatcherFeature.hpp"
#include "Features/ModifyMinecraftCardFeature.hpp"
#include "Features/ResourceInstallFeature.hpp"
#include "Features/DownloadNavigationFeature.hpp"
#include "Features/ModpackSearchFeature.hpp"
#include "Features/DisabledModDownloadFeature.hpp"
#include "Features/LaunchDownloadFeature.hpp"
#include "Features/EmptyListDownloadFeature.hpp"
#include "Features/UpdateDownloadFeature.hpp"
#include "Features/ModpackImportFeature.hpp"

namespace PclFix {
    // 一套完整版本规则：显示信息、适用起点、验证范围与修补功能放在同一处。
    struct PatchRuleSet {
        std::string id; // 稳定身份，供扫描结果记录使用。
        std::string name; // 控制台显示的简短规则名。
        std::string description; // 开发者可阅读的版本差异说明。
        TargetVersion firstVersion; // 从此版本开始优先使用本套规则，直到下一套规则的起点。
        TargetVersion lastVerifiedVersion; // 已验证范围的上限，超出范围只警告。
        PatchFeatureRegistry features; // 本套规则独立拥有的修补功能实例。

        // 版本范围只决定提示，不决定是否允许生成补丁。
        std::string GetWarning(const TargetVersion& version) const {
            if (version < firstVersion) return "此版本早于当前规则的验证范围，将继续校验结构。";
            if (version > lastVerifiedVersion) return "此版本高于当前规则的验证范围，将继续校验结构。";
            return {};
        }
    };

    // 持有版本规则表，统一选择功能集合及对应说明；业务流程无需知道具体版本分界。
    class PatchRuleCatalog {
    private:
        std::vector<PatchRuleSet> m_rules; // 按版本起点排序，实例随规则表一起生存。

        // 两套规则共有的五项功能只列举一次，新增版本可继续复用或自行组合。
        static void RegisterCommon(PatchFeatureRegistry& registry) {
            registry.Register(std::make_unique<DownloadPageLoaderFeature>());
            registry.Register(std::make_unique<ModpackPickerFeature>());
            registry.Register(std::make_unique<ModpackDispatcherFeature>());
            registry.Register(std::make_unique<ModifyMinecraftCardFeature>());
            registry.Register(std::make_unique<ResourceInstallFeature>());
        }

    public:
        // 登记时校验描述与版本范围，拒绝重复身份、相同版本起点及重叠的验证范围。
        void Register(PatchRuleSet rule) {
            if (rule.id.empty() || rule.name.empty() || rule.description.empty()) throw std::invalid_argument("初始化版本规则失败：缺少编号、名称或说明。");
            if (rule.lastVerifiedVersion < rule.firstVersion) throw std::invalid_argument("初始化版本规则失败：验证范围的上限早于起点。");
            for (const auto& existing : m_rules) {
                if (existing.id == rule.id) throw std::invalid_argument("初始化版本规则失败：规则编号重复。");
                if (!(rule.lastVerifiedVersion < existing.firstVersion || existing.lastVerifiedVersion < rule.firstVersion)) {
                    throw std::invalid_argument("初始化版本规则失败：验证范围重复或重叠。");
                }
            }
            m_rules.push_back(std::move(rule));
            std::sort(m_rules.begin(), m_rules.end(), [](const auto& left, const auto& right) { return left.firstVersion < right.firstVersion; });
        }

        // 选择起点不晚于目标版本的最近规则；更早的版本使用首套，未来版本使用末套。
        // 返回值由本对象持有，不允许在继续注册后保留引用；内置表初始化后只读。
        const PatchRuleSet& Select(const TargetVersion& version) const {
            if (m_rules.empty()) throw std::runtime_error("没有已注册的版本规则，无法检查 PCL。");
            const PatchRuleSet* selected = &m_rules.front(); // 低于最早起点时也继续检查。
            for (const auto& rule : m_rules) {
                if (version < rule.firstVersion) break;
                selected = &rule;
            }
            return *selected;
        }

        // 程序启动时一次性注册所有内置规则。新增版本只在此登记，无需修改扫描和验证流程。
        static const PatchRuleCatalog& BuiltIn() {
            static const PatchRuleCatalog catalog = [] {
                PatchRuleCatalog result; // 完成注册后发布只读表。
                PatchRuleSet initial{ // 首次引入下载拦截的规则及其已验证范围。
                    "download-2.99.0.0", "2.99.0.0 下载功能检查",
                    "检查并恢复首次引入下载拦截时的五项下载功能。",
                    {2, 99, 0, 0}, {2, 99, 0, 0}, {}
                };
                RegisterCommon(initial.features);
                result.Register(std::move(initial));

                PatchRuleSet entries{ // 新增隐藏入口后采用的规则，保留原有版本的功能组合。
                    "download-2.99.0.1", "2.99.0.1 下载拦截与入口恢复",
                    "复用原有五项功能，并恢复新增隐藏的七个下载入口。",
                    {2, 99, 0, 1}, {2, 99, 0, 1}, {}
                };
                RegisterCommon(entries.features);

                entries.features.Register(std::make_unique<DownloadNavigationFeature>());
                entries.features.Register(std::make_unique<ModpackSearchFeature>());
                entries.features.Register(std::make_unique<DisabledModDownloadFeature>());
                entries.features.Register(std::make_unique<LaunchDownloadFeature>());
                entries.features.Register(std::make_unique<EmptyListDownloadFeature>());
                entries.features.Register(std::make_unique<UpdateDownloadFeature>());
                entries.features.Register(std::make_unique<ModpackImportFeature>());
                result.Register(std::move(entries));
                return result;
            }();
            return catalog;
        }
    };
}
