#pragma once
#include "MethodPatchFeature.hpp"

namespace PclFix {
    // 恢复下载页左侧的原版游戏下载导航及默认选中页面。
    class DownloadNavigationFeature final : public MethodPatchFeature {
    private:
        // 扫描时只确认结构与状态，写入时按相同条件生成当前入口的修改。
        void AnalyzeMethod(Context& context, mdMethodDef method, const Body& body, PatchAnalysis& analysis, bool build_patch) const override {
            const auto& items = body.instructions; // 构造函数必须先完成原始页面初始化。
            Require(!body.extraSections && items.size() >= 8);
            const auto installId = context.EnumLiteral(context.Metadata(), L"PCL.FormMain/PageSubType", L"DownloadInstall"); // 目标中原版游戏页面的编号。
            const auto modId = context.EnumLiteral(context.Metadata(), L"PCL.FormMain/PageSubType", L"DownloadMod"); // 目标中 Mod 页面编号。
            Require(installId.has_value() && modId.has_value());
            const auto initialize = context.Resolve(L"PCL.PageDownloadLeft", L"InitializeComponent", L"void", {}, true);
            size_t tail = 0; // 初始化后的拦截代码起点。
            for (size_t index = 0; index < items.size(); ++index) if (items[index].opcode == 0x28 && items[index].token == initialize) { Require(!tail); tail = index + 1; }
            Require(tail == 7 && items[0].opcode == 0x02 && Member(context, items[1], L"PCL.MyPageLeft", L".ctor") &&
                items[2].opcode == 0x02 && context.IntegerValue(body, items[3]) == *installId &&
                items[4].opcode == 0x7d && Member(context, items[4], L"PCL.PageDownloadLeft", L"PageID") && items[5].opcode == 0x02);
            const bool restricted = tail + 1 != items.size(); // 正常构造函数在初始化后立即返回。
            Require(items.back().opcode == 0x2a);
            if (restricted) {
                Require(items.size() == tail + 16);
                const auto install = context.Resolve(L"PCL.PageDownloadLeft", L"get_ItemInstall", L"PCL.MyListItem", {}, true);
                const auto label = context.Resolve(L"PCL.PageDownloadLeft", L"get_LabResource", L"System.Windows.Controls.TextBlock", {}, true);
                const auto mod = context.Resolve(L"PCL.PageDownloadLeft", L"get_ItemMod", L"PCL.MyListItem", {}, true);
                const auto visibility = context.Resolve(L"System.Windows.UIElement", L"set_Visibility", L"void", {L"System.Windows.Visibility"}, true);
                const auto checked = context.Resolve(L"PCL.MyListItem", L"set_Checked", L"void", {L"bool"}, true);
                Require(Matches(context, body, tail, items.size() - 1, [&](IlAssembler& output) {
                    output.Op(0x02); output.Token(0x6f, install); output.Integer(2); output.Token(0x6f, visibility);
                    output.Op(0x02); output.Token(0x6f, label); output.Integer(2); output.Token(0x6f, visibility);
                    output.Op(0x02); output.Integer(*modId); output.Token(0x7d, items[4].token);
                    output.Op(0x02); output.Token(0x6f, mod); output.Integer(1); output.Token(0x6f, checked);
                }));
            }
            Record(context, method, body, analysis, build_patch, restricted, [&](IlAssembler& output) {
                Replace(context, body, output, tail, items.size() - 1, [](IlAssembler&) {});
            });
        }

    public:
        // 注册本入口的唯一名称、类别及目标函数。
        DownloadNavigationFeature() : MethodPatchFeature({"download-navigation", "下载页面导航", PatchPointKind::DownloadEntries, true}, L"PCL.PageDownloadLeft", L".ctor") {}
    };
}
