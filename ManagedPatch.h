#pragma once
#include "ManagedPatchContext.hpp"
#include "Features/PatchFeatureRegistry.hpp"
#include <functional>

namespace PclFix {

    // 协调注册特征的扫描、计划生成和候选镜像输出，不包含具体功能规则。
    class ManagedPatch {
    public:
        // 先扫描全部特征，再生成完整计划；任一点失败都不返回可写入的候选。
        static PatchAnalysis Analyze(const std::filesystem::path& path,
            const std::function<void(PatchPointKind)>& on_scan = {},
            const PatchFeatureRegistry& registry = PatchFeatureRegistry::BuiltIn()) {
            PatchAnalysis analysis; // 单次文件分析的完整结果。
            try {
                ManagedPatchContext context; // 仅持有本次目标的元数据副本。
                context.Open(path);
                for (const auto& feature : registry.Features()) {
                    FeatureScanResult result; // 每个子类的状态独立保存。
                    result.info = feature->Describe();
                    if (on_scan) on_scan(result.info.category);
                    try {
                        result = feature->Scan(context);
                        const auto info = feature->Describe(); // 子类不能把结果归属到另一注册身份。
                        if (result.info.id != info.id || result.info.category != info.category ||
                            result.detectedCount != result.patchableCount + result.restoredCount) {
                            throw std::runtime_error("扫描结果不一致，已停止修补；请更新修补器或反馈此问题。");
                        }
                    } catch (const std::exception& error) {
                        result.state = FeatureState::Unsupported;
                        result.reason = error.what();
                    }
                    analysis.features.push_back(std::move(result));
                }
                const bool recognized = std::any_of(analysis.features.begin(), analysis.features.end(),
                    [](const auto& item) { return item.state != FeatureState::NotFound; });
                if (!recognized) throw std::runtime_error("未识别到可支持的特征。");
                const bool pending = std::any_of(analysis.features.begin(), analysis.features.end(),
                    [](const auto& item) { return item.state == FeatureState::Restricted; });
                for (const auto& result : analysis.features) {
                    if (result.state == FeatureState::Unsupported) {
                        throw std::runtime_error(std::string(result.info.name) + "：" + result.reason);
                    }
                    if (result.info.required && result.state == FeatureState::NotFound) {
                        if (pending) {
                            throw std::runtime_error("仅识别到部分特征，未识别到必要特征：" + std::string(result.info.name) + "。目标可能是部分修补版本、社区版或其他改版，无法安全执行完整修补。");
                        }
                        analysis.warning = "仅识别到部分功能的正常结构，其余功能状态无法确认；目标可能是旧版、社区版或其他改版。";
                    }
                }
                for (const auto& feature : registry.Features()) {
                    const auto info = feature->Describe(); // 使用身份关联扫描结果与规则。
                    auto found = std::find_if(analysis.features.begin(), analysis.features.end(),
                        [&info](const auto& item) { return item.info.id == info.id; });
                    if (found == analysis.features.end() || found->state != FeatureState::Restricted) continue;
                    PatchAnalysis featurePlan; // 子类的计划先隔离验证，再合并到完整计划。
                    try {
                        feature->Patch(context, *found, featurePlan);
                        if (featurePlan.TotalPoints() != found->patchableCount) throw std::runtime_error("生成的修补点数量与扫描结果不一致。");
                    } catch (const std::exception& error) {
                        found->state = FeatureState::Unsupported;
                        found->reason = error.what();
                        throw std::runtime_error(std::string(info.name) + "：" + error.what());
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
                if (analysis.TotalPoints()) analysis.image = context.BuildCandidate(analysis);
            } catch (const std::exception& error) {
                analysis.error = error.what();
                analysis.ranges.clear();
                analysis.methods.clear();
                analysis.image.clear();
            }
            return analysis;
        }
    };

}
