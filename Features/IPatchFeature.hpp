#pragma once
#include "../PatchTypes.hpp"
#include "../ManagedPatchContext.hpp"

namespace PclFix {

    class ManagedPatchContext;
    struct PatchAnalysis;

    // 每个下载功能修补模块都实现此类。
    class IPatchFeature {
    protected:
        // 检查“加载提示字符串、构造异常、抛出”的指令与签名，不读取提示文字的内容。
        // 此辅助函数只确认报错结构；是否属于下载拦截由各功能的调用关系和完整结构决定。
        static bool IsExceptionThrow(ManagedPatchContext& context, const ManagedPatchContext::MethodBody& body, size_t index) {
            const auto& items = body.instructions; // 当前目标函数的已解码指令。
            if (index + 2 >= items.size() || items[index].opcode != 0x72 || items[index + 1].opcode != 0x73 || items[index + 2].opcode != 0x7a) return false;
            mdToken owner = 0; // 异常构造函数的声明类型。
            if (context.MemberName(items[index + 1].token, owner) != L".ctor" || context.TypeName(owner) != L"System.Exception") return false;
            const auto signature = context.Signature(items[index + 1].token); // 只接受单字符串参数构造函数。
            return signature.instance && signature.result == L"void" && signature.parameters == std::vector<std::wstring>{L"string"};
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
