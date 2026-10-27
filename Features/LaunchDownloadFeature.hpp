#pragma once
#include "MethodPatchFeature.hpp"
#include "DownloadVisibilityCode.hpp"

namespace PclFix {
    // 没有选择游戏时，恢复启动按钮原有的下载引导状态。
    class LaunchDownloadFeature final : public MethodPatchFeature {
    private:
        // 扫描时只确认结构与状态，写入时按相同条件生成当前入口的修改。
        void AnalyzeMethod(Context& context, mdMethodDef method, const Body& body, PatchAnalysis& analysis, bool build_patch) const override {
            const auto& items = body.instructions;
            size_t first = 0; // 首次判断已选游戏之后的无游戏分支。
            for (size_t index = 0; index + 2 < items.size(); ++index) {
                if (Member(context, items[index], L"PCL.ModMinecraft", L"get_McInstanceSelected")) {
                    Require(!first && (items[index + 1].opcode == 0x2d || items[index + 1].opcode == 0x3a));
                    first = index + 2; break;
                }
            }
            Require(first && first + 3 < items.size());
            const DownloadVisibilityCode visibility(context); // 显示设置决定下载引导是否可用。
            const bool restricted = context.IntegerValue(body, items[first]) == 1 && context.LocalIndex(body, items[first + 1], true) == 0;
            const size_t last = first + (restricted ? 1 : visibility.InstructionCount());
            Require(last + 1 < items.size() && context.LocalIndex(body, items[last], true) == 0 &&
                (items[last + 1].opcode == 0x2b || items[last + 1].opcode == 0x38));
            if (!restricted) Require(Matches(context, body, first, last, [&](IlAssembler& output) { visibility.Emit(output, 2, 1); }));
            Record(context, method, body, analysis, build_patch, restricted, [&](IlAssembler& output) {
                Replace(context, body, output, first, last, [&](IlAssembler& expression) { visibility.Emit(expression, 2, 1); });
            });
        }

    public:
        // 注册本入口的唯一名称、类别及目标函数。
        LaunchDownloadFeature() : MethodPatchFeature({"launch-download", "启动页下载引导", PatchPointKind::DownloadEntries, true}, L"PCL.PageLaunchLeft", L"RefreshButtonsUI") {}
    };
}
