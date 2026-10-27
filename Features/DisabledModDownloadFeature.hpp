#pragma once
#include "MethodPatchFeature.hpp"
#include "DownloadVisibilityCode.hpp"

namespace PclFix {
    // 恢复禁用 Mod 页面上的下载入口，同时保留隐藏设置与 ShowHidden 的原有影响。
    class DisabledModDownloadFeature final : public MethodPatchFeature {
    private:
        // 扫描时只确认结构与状态，写入时按相同条件生成当前入口的修改。
        void AnalyzeMethod(Context& context, mdMethodDef method, const Body& body, PatchAnalysis& analysis, bool build_patch) const override {
            const auto& items = body.instructions;
            Require(items.size() > 3 && context.Locals(body) == std::vector<std::wstring>{L"System.Windows.Visibility"});
            size_t store = 0; // 保存可见性结果的局部变量位置。
            for (size_t index = 0; index < items.size(); ++index) if (context.LocalIndex(body, items[index], true) == 0) { Require(!store); store = index; }
            Require(store && Member(context, items[store + 2], L"PCL.PageInstanceModDisabled", L"get_BtnDownload"));
            const DownloadVisibilityCode visibility(context, true); // 完整恢复原有设置判断。
            const bool restored = Matches(context, body, 0, store, [&](IlAssembler& output) { visibility.Emit(output); });
            if (!restored) Require(store == 1 && context.IntegerValue(body, items[0]) == 2);
            Record(context, method, body, analysis, build_patch, !restored, [&](IlAssembler& output) {
                Replace(context, body, output, 0, store, [&](IlAssembler& expression) { visibility.Emit(expression); });
            });
        }

    public:
        // 注册本入口的唯一名称、类别及目标函数。
        DisabledModDownloadFeature() : MethodPatchFeature({"disabled-mod-download", "禁用 Mod 页下载按钮", PatchPointKind::DownloadEntries, true}, L"PCL.PageInstanceModDisabled", L"BtnDownload_Loaded") {}
    };
}
