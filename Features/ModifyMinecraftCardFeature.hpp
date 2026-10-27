#pragma once
#include "IPatchFeature.hpp"
#include "../ManagedPatchContext.hpp"

namespace PclFix {

    // 负责恢复修改页面中 Minecraft 卡片的可见状态。
    class ModifyMinecraftCardFeature final : public IPatchFeature {
    private:
        // 识别修改页中由限制常量控制的 Minecraft 卡片可见性表达式。
        static void AnalyzeType(ManagedPatchContext& context, mdTypeDef type, PatchAnalysis& analysis, bool build_patch) {
            ManagedPatchContext::MetadataEnumeration enumeration{context.Metadata()};
            mdMethodDef methods[128]{}; ULONG count = 0;
            while (SUCCEEDED(context.Metadata()->EnumMethods(&enumeration.handle, type, methods, 128, &count)) && count) {
                for (ULONG methodIndex = 0; methodIndex < count; ++methodIndex) {
                    mdToken methodOwner = 0;
                    if (context.MemberName(methods[methodIndex], methodOwner) != L"LoadInstanceFinished") continue;
                    ManagedPatchContext::MethodBody body;
                    try { body = context.Body(methods[methodIndex]); } catch (const std::exception&) { continue; }
                    for (size_t index = 0; index + 2 < body.instructions.size(); ++index) {
                        const auto& getter = body.instructions[index];
                        const auto& value = body.instructions[index + 1];
                        const auto& setter = body.instructions[index + 2];
                        mdToken getterOwner = 0;
                        mdToken setterOwner = 0;
                        if ((getter.opcode != 0x28 && getter.opcode != 0x6f) ||
                            context.MemberName(getter.token, getterOwner) != L"get_CardMinecraft" ||
                            context.TypeName(getterOwner) != L"PCL.PageDownloadInstall" ||
                            (value.opcode != 0x18 && value.opcode != 0x16) ||
                            (setter.opcode != 0x28 && setter.opcode != 0x6f) ||
                            context.MemberName(setter.token, setterOwner) != L"set_Visibility" ||
                            context.TypeName(setterOwner) != L"System.Windows.UIElement") continue;
                        if (value.opcode == 0x16) { ++analysis.restoredCount; continue; }
                        if (!build_patch) { analysis.ranges.push_back({}); continue; }
                        context.AddBytePatch(body, value, 0x18, 0x16, PatchPointKind::ModifyPage, analysis);
                    }
                }
            }
        }

    public:
        // 返回该具体功能的稳定身份、显示名称和分类。
        PatchFeatureInfo Describe() const override { return {"modify-minecraft-card", "修改页面的 Minecraft 选项", PatchPointKind::ModifyPage, true}; }

        // 只读检查自身结构，未匹配不能当作已经恢复。
        FeatureScanResult Scan(ManagedPatchContext& context) const override {
            FeatureScanResult result; // 本文件的特征状态与目标类型集合。
            result.info = Describe();
            for (const auto type : context.TypeDefinitions()) if (context.TypeName(type) == L"PCL.PageDownloadModify") result.typeDefinitions.push_back(type);
            PatchAnalysis probe; // 仅用于记录扫描命中的结构，不生成候选 PE。
            for (const auto type : result.typeDefinitions) AnalyzeType(context, type, probe, false);
            result.patchableCount = probe.TotalPoints();
            result.restoredCount = probe.restoredCount;
            result.detectedCount = result.patchableCount + result.restoredCount;
            result.state = result.patchableCount ? FeatureState::Restricted :
                (result.restoredCount ? FeatureState::Restored : FeatureState::NotFound);
            return result;
        }

        // 只有受限结构才生成修改；注册表负责标记身份并合并完整计划。
        void Patch(ManagedPatchContext& context, const FeatureScanResult& result, PatchAnalysis& analysis) const override {
            for (const auto type : result.typeDefinitions) AnalyzeType(context, type, analysis, true);
        }

        // 根据具体特征身份复查恢复数量，不借用同类别其他功能的结果。
        bool Verify(const PatchAnalysis& analysis, const FeatureScanResult& result) const override {
            const auto after = std::find_if(analysis.features.begin(), analysis.features.end(), [&result](const auto& item) { return item.info.id == result.info.id; });
            return analysis.error.empty() && after != analysis.features.end() && after->patchableCount == 0 &&
                after->restoredCount >= result.restoredCount + result.patchableCount;
        }
    };

}
