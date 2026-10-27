#pragma once
#include "MethodPatchFeature.hpp"

namespace PclFix {
    // 恢复 Minecraft 更新提示中的“下载”按钮，不修改普通确认框。
    class UpdateDownloadFeature final : public MethodPatchFeature {
    private:
        // 扫描时只确认结构与状态，写入时按相同条件生成当前入口的修改。
        void AnalyzeMethod(Context& context, mdMethodDef method, const Body& body, PatchAnalysis& analysis, bool build_patch) const override {
            const auto& items = body.instructions;
            size_t button = 0, calls = 0; // 唯一更新提示的位置及其消息框调用数量。
            for (size_t index = 0; index + 2 < items.size(); ++index) {
                if (items[index].opcode == 0x72 && context.UserString(items[index].token) == L"Minecraft 更新提示") {
                    Require(!button && items[index + 1].opcode == 0x72 && context.UserString(items[index + 1].token) == L"确定" && items[index + 2].opcode == 0x72);
                    button = index + 2;
                }
                if (Member(context, items[index], L"PCL.ModMain", L"MyMsgBox")) ++calls;
            }
            Require(button && calls == 1);
            const auto text = context.UserString(items[button].token); // 已恢复必须明确是下载按钮。
            Require(text.empty() || text == L"下载");
            Record(context, method, body, analysis, build_patch, text.empty(), [&](IlAssembler& output) {
                Replace(context, body, output, button, button + 1, [&](IlAssembler& expression) { expression.Token(0x72, context.StringToken(L"下载")); });
            });
        }

    public:
        // 注册本入口的唯一名称、类别及目标函数。
        UpdateDownloadFeature() : MethodPatchFeature({"update-download", "Minecraft 更新提示下载按钮", PatchPointKind::DownloadEntries, true}, L"PCL.ModMinecraft", L"McDownloadClientUpdateHint") {}
    };
}
