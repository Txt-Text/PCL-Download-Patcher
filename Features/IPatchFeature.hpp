#pragma once
#include "../PatchTypes.hpp"
#include "../ManagedPatchContext.hpp"

namespace PclFix {

    class ManagedPatchContext;
    struct PatchAnalysis;

    // 每个下载功能修补模块都实现此类。
    class IPatchFeature {
    protected:
        // 仅在实际限制提示出现时重建，不以版本号或普通下载文字判定受限。
        static bool IsRestriction(ManagedPatchContext& context, const ManagedPatchContext::Instruction& instruction) {
            return instruction.opcode == 0x72 && context.UserString(instruction.token).find(L"PCL 暂时不再提供 MC 下载功能") != std::wstring::npos;
        }

    public:
        virtual ~IPatchFeature() = default;

        // 返回稳定的用户界面描述和分类信息。
        virtual PatchFeatureInfo Describe() const = 0;

        // 只读取目标程序集，判断该特征是否存在、已经恢复或不兼容。
        virtual FeatureScanResult Scan(ManagedPatchContext& context) const = 0;

        // 根据扫描结果把本特征的修改加入统一补丁计划。
        virtual void Patch(ManagedPatchContext& context, const FeatureScanResult& result,
            PatchAnalysis& analysis) const = 0;

        // 根据完整分析结果确认本特征已经达到正常状态。
        virtual bool Verify(const PatchAnalysis& analysis,
            const FeatureScanResult& result) const = 0;
    };

}
