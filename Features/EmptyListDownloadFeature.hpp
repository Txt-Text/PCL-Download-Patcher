#pragma once
#include "MethodPatchFeature.hpp"
#include "DownloadVisibilityCode.hpp"

namespace PclFix {
    // 只恢复未安装游戏时的下载按钮，隐藏模式下的空列表仍保持隐藏。
    class EmptyListDownloadFeature final : public MethodPatchFeature {
    private:
        // 扫描时只确认结构与状态，写入时按相同条件生成当前入口的修改。
        void AnalyzeMethod(Context& context, mdMethodDef method, const Body& body, PatchAnalysis& analysis, bool build_patch) const override {
            const auto& items = body.instructions;
            std::vector<size_t> getters; // 两种空列表状态各访问一次按钮。
            for (size_t index = 0; index < items.size(); ++index) if (Member(context, items[index], L"PCL.PageSelectRight", L"get_BtnEmptyDownload")) getters.push_back(index);
            Require(getters.size() == 2 && getters[0] + 2 < items.size() &&
                context.IntegerValue(body, items[getters[0] + 1]) == 2 && Member(context, items[getters[0] + 2], L"System.Windows.UIElement", L"set_Visibility"));
            const size_t first = getters.back() + 1; // 仅修改“未找到游戏”的分支。
            const DownloadVisibilityCode visibility(context);
            const bool restricted = context.IntegerValue(body, items[first]) == 2;
            const size_t last = first + (restricted ? 1 : visibility.InstructionCount());
            Require(last < items.size() && Member(context, items[last], L"System.Windows.UIElement", L"set_Visibility"));
            if (!restricted) Require(Matches(context, body, first, last, [&](IlAssembler& output) { visibility.Emit(output); }));
            Record(context, method, body, analysis, build_patch, restricted, [&](IlAssembler& output) {
                Replace(context, body, output, first, last, [&](IlAssembler& expression) { visibility.Emit(expression); });
            });
        }

    public:
        // 注册本入口的唯一名称、类别及目标函数。
        EmptyListDownloadFeature() : MethodPatchFeature({"empty-list-download", "空版本列表下载按钮", PatchPointKind::DownloadEntries, true}, L"PCL.PageSelectRight", L"McInstanceListUI") {}
    };
}
