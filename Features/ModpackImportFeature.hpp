#pragma once
#include "MethodPatchFeature.hpp"

namespace PclFix {
    // 恢复版本选择页中已创建但未加入列表的“导入整合包”选项。
    class ModpackImportFeature final : public MethodPatchFeature {
    private:
        // 扫描时只确认结构与状态，写入时按相同条件生成当前入口的修改。
        void AnalyzeMethod(Context& context, mdMethodDef method, const Body& body, PatchAnalysis& analysis, bool build_patch) const override {
            const auto& items = body.instructions;
            size_t title = 0, store = 0; // 唯一选项的标题和保存位置。
            for (size_t index = 0; index + 1 < items.size(); ++index) {
                if (items[index].opcode == 0x72 && context.UserString(items[index].token) == L"导入整合包") {
                    Require(!title && Member(context, items[index + 1], L"PCL.MyListItem", L"set_Title")); title = index;
                }
            }
            Require(title);
            for (size_t index = title + 2; index < items.size(); ++index) if (context.LocalIndex(body, items[index], true) >= 0) { store = index; break; }
            Require(store && store + 1 < items.size());
            const int slot = context.LocalIndex(body, items[store], true); // 不假定整合包选项总在同一个局部变量编号。
            const auto locals = context.Locals(body);
            Require(static_cast<size_t>(slot) < locals.size() && locals[slot] == L"PCL.MyListItem");
            const auto main = context.ResolveType(L"PCL.ModMain");
            const auto page = context.ResolveField(main, L"FrmSelectLeft", L"PCL.PageSelectLeft");
            const auto panel = context.Resolve(L"PCL.PageSelectLeft", L"get_PanList", L"System.Windows.Controls.StackPanel", {}, true);
            const auto children = context.Resolve(L"System.Windows.Controls.Panel", L"get_Children", L"System.Windows.Controls.UIElementCollection", {}, true);
            const auto add = context.Resolve(L"System.Windows.Controls.UIElementCollection", L"Add", L"int32", {L"System.Windows.UIElement"}, true);
            auto insert = [&](IlAssembler& output) {
                output.Token(0x7e, page); output.Token(0x6f, panel); output.Token(0x6f, children);
                output.Local(false, slot); output.Token(0x6f, add); output.Op(0x26);
            };
            const size_t first = store + 1; // 原版应在保存选项后将它加入列表。
            const bool restored = first + 6 <= items.size() && Matches(context, body, first, first + 6, insert);
            const size_t callback = first + (restored ? 6 : 0);
            Require(callback < items.size() && context.LocalIndex(body, items[callback], false) == slot);
            bool handler = false, attached = false; // 确认此选项仍连接真正的整合包安装功能。
            for (size_t index = callback + 1; index < items.size(); ++index) {
                if (items[index].opcode == 0xfe06 && TypeFromToken(items[index].token) == mdtMethodDef) {
                    const auto target = context.Body(items[index].token);
                    for (const auto& instruction : target.instructions) if (Member(context, instruction, L"PCL.ModModpack", L"ModpackInstall")) handler = true;
                }
                if (Member(context, items[index], L"PCL.MyListItem", L"add_Click")) { attached = true; break; }
            }
            Require(handler && attached);
            Record(context, method, body, analysis, build_patch, !restored, [&](IlAssembler& output) {
                for (size_t index = 0; index < items.size(); ++index) {
                    context.CopyInstruction(output, body, items[index]);
                    if (index == store) insert(output);
                }
                output.Mark(static_cast<int>(body.length));
            });
        }

    public:
        // 注册本入口的唯一名称、类别及目标函数。
        ModpackImportFeature() : MethodPatchFeature({"modpack-import", "版本选择页整合包导入", PatchPointKind::DownloadEntries, true}, L"PCL.PageSelectLeft", L"McFolderListUI") {}
    };
}
