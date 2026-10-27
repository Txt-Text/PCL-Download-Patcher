#pragma once
#include "ManagedPatchContext.hpp"
#include "PatchRuleCatalog.hpp"
#include <functional>

namespace PclFix {

    // 协调注册特征的扫描、计划生成和候选镜像输出，不包含具体功能规则。
    class ManagedPatch {
    public:
        // 独立扫描和生成每个功能的修改；无法确认的功能跳过，文件级错误仍停止写入。
        static PatchAnalysis Analyze(const std::filesystem::path& path,
            const std::function<void(PatchPointKind)>& on_scan = {},
            const PatchFeatureRegistry* override_registry = nullptr) {
            PatchAnalysis analysis; // 单次文件分析的完整结果。
            try {
                ManagedPatchContext context; // 仅持有本次目标的元数据副本。
                context.Open(path);
                analysis.version = context.GetVersion();
                const auto& version = *analysis.version; // 只认程序集真实版本，不认文件名中的版本。
                const auto& rule = PatchRuleCatalog::BuiltIn().Select(version); // 描述、范围和功能来自同一条登记。
                const PatchFeatureRegistry* registry = &rule.features; // 自定义功能集合只用于扩展调用与测试。
                if (override_registry) {
                    registry = override_registry;
                    analysis.ruleId = "custom";
                    analysis.ruleName = "自定义修补功能";
                } else {
                    analysis.ruleId = rule.id;
                    analysis.ruleName = rule.name;
                    analysis.warning = rule.GetWarning(version);
                }
                for (const auto& feature : registry->Features()) {
                    FeatureScanResult result; // 每个子类的状态独立保存。
                    result.info = feature->Describe();
                    if (on_scan) on_scan(result.info.category);
                    try {
                        result = feature->Scan(context);
                        const auto info = feature->Describe(); // 子类不能把结果归属到另一注册身份。
                        if (result.info.id != info.id || result.info.category != info.category ||
                            result.detectedCount != result.patchableCount + result.restoredCount) {
                            throw std::runtime_error("此功能的扫描结果不一致，已跳过。请更新修补器或反馈此问题。");
                        }
                    } catch (const std::exception& error) {
                        result.state = FeatureState::Unsupported;
                        result.reason = error.what();
                    }
                    analysis.features.push_back(std::move(result));
                }
                bool retry = false; // 某功能生成失败后重新建立元数据，丢弃它已添加的字符串和函数引用。
                do {
                    retry = false;
                    analysis.ranges.clear(); analysis.methods.clear();
                    ManagedPatchContext planning; // 每轮从原文件创建干净的候选，失败功能不会污染其他功能。
                    planning.Open(path);
                    for (const auto& feature : registry->Features()) {
                        const auto info = feature->Describe(); // 使用身份关联扫描结果与规则。
                        auto found = std::find_if(analysis.features.begin(), analysis.features.end(),
                            [&info](const auto& item) { return item.info.id == info.id; });
                        if (found == analysis.features.end() || found->state != FeatureState::Restricted) continue;
                        PatchAnalysis featurePlan; // 子类的计划先隔离验证，再合并到完整计划。
                        try {
                            feature->Patch(planning, *found, featurePlan);
                            if (featurePlan.TotalPoints() != found->patchableCount) throw std::runtime_error("生成的修补点数量与扫描结果不一致。");
                        } catch (const std::exception& error) {
                            found->state = FeatureState::Unsupported;
                            found->reason = error.what();
                            found->patchableCount = 0;
                            found->detectedCount = found->restoredCount;
                            retry = true;
                            break; // 本轮计划全部丢弃，下一轮只重放仍然可修补的功能。
                        }
                        for (auto& range : featurePlan.ranges) {
                            range.featureId = info.id;
                            range.kind = info.category;
                            analysis.ranges.push_back(std::move(range));
                        }
                        for (auto& method : featurePlan.methods) {
                            method.featureId = info.id;
                            method.kind = info.category;
                            analysis.methods.push_back(std::move(method));
                        }
                    }
                    if (!retry && analysis.TotalPoints()) analysis.image = planning.BuildCandidate(analysis);
                } while (retry);
                if (const auto skipped = analysis.SkippedFeatures()) {
                    if (!analysis.warning.empty()) analysis.warning += "\n";
                    analysis.warning += "有 " + std::to_string(skipped) + " 项功能验证失败或无法确认，已跳过。将仅修补通过校验的功能。";
                }
            } catch (const std::exception& error) {
                analysis.error = error.what();
                analysis.ranges.clear();
                analysis.methods.clear();
                analysis.image.clear();
            }
            return analysis;
        }

        // 显式注册表用于扩展功能和独立测试，不改变生产入口的版本选择。
        static PatchAnalysis Analyze(const std::filesystem::path& path,
            const std::function<void(PatchPointKind)>& on_scan, const PatchFeatureRegistry& registry) {
            return Analyze(path, on_scan, &registry);
        }
    };

}
