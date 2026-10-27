#pragma once
#include "MethodPatchFeature.hpp"

namespace PclFix {
    // 整合包资源页的本地安装按钮只应在整合包类别下出现。
    class ModpackSearchFeature final : public MethodPatchFeature {
    private:
        // 扫描时只确认结构与状态，写入时按相同条件生成当前入口的修改。
        void AnalyzeMethod(Context& context, mdMethodDef method, const Body& body, PatchAnalysis& analysis, bool build_patch) const override {
            const auto& items = body.instructions;
            size_t first = 0, last = 0; // 按钮可见性表达式的指令边界。
            for (size_t index = 0; index < items.size(); ++index) {
                if (Member(context, items[index], L"PCL.PageResource", L"get_BtnSearchInstallModPack")) { Require(!first); first = index + 1; }
                if (first && Member(context, items[index], L"System.Windows.UIElement", L"set_Visibility")) { last = index; break; }
            }
            Require(first && last > first && first >= 3);
            const auto field = items[first - 3].token; // 属性赋值保存的资源类型。
            Require(items[first - 3].opcode == 0x7d && Member(context, items[first - 3], L"PCL.PageResource", L"_Type"));
            const auto type = context.FieldType(field);
            auto modpack = context.EnumLiteral(context.Metadata(), type, L"ModPack");
            if (!modpack) modpack = context.EmbeddedEnum(context.EnumDependencyName(field), type, L"ModPack");
            Require(modpack.has_value());
            auto normal = [&](IlAssembler& output) {
                const int visible = output.Label(), end = output.Label();
                output.Op(0x03); output.Integer(*modpack); output.Branch(0x3b, visible);
                output.Integer(2); output.Branch(0x38, end); output.Mark(visible); output.Integer(0); output.Mark(end);
            };
            const bool restored = Matches(context, body, first, last, normal);
            if (!restored) Require(Matches(context, body, first, last, [&](IlAssembler& output) {
                output.Op(0x03); output.Integer(*modpack); output.Op(0x26); output.Op(0x26); output.Integer(2);
            }));
            Record(context, method, body, analysis, build_patch, !restored, [&](IlAssembler& output) { Replace(context, body, output, first, last, normal); });
        }

    public:
        // 注册本入口的唯一名称、类别及目标函数。
        ModpackSearchFeature() : MethodPatchFeature({"modpack-search-button", "整合包搜索页安装按钮", PatchPointKind::DownloadEntries, true}, L"PCL.PageResource", L"set_PageType") {}
    };
}
