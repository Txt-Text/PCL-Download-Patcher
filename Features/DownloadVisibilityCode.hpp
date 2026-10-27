#pragma once
#include "IPatchFeature.hpp"

namespace PclFix {
    // 恢复 PCL 原有的下载入口显示条件，继续尊重用户的隐藏设置和临时显示操作。
    class DownloadVisibilityCode {
    private:
        mdString m_setting = 0; // 当前目标已有的 UiHiddenPageDownload 字符串。
        mdMethodSpec m_get = 0; // 当前目标已有的 Settings.Get<bool> 调用。
        mdToken m_forceShow = 0; // 用户临时显示隐藏入口的状态读取函数。
        mdToken m_selectRight = 0; // 版本列表页面实例，只有禁用 Mod 页面需要。
        mdToken m_showHidden = 0; // 版本列表显示隐藏版本的状态。
        bool m_checkHidden; // 是否同时考虑版本列表的 ShowHidden。

    public:
        // 仅读取目标已有信息，扫描阶段不增加类型、字符串或函数引用。
        explicit DownloadVisibilityCode(ManagedPatchContext& context, bool check_hidden = false)
            : m_checkHidden(check_hidden) {
            ManagedPatchContext::MetadataEnumeration strings{context.Metadata()};
            mdString batch[128]{}; ULONG count = 0; // 分批读取当前字符串表。
            while (SUCCEEDED(context.Metadata()->EnumUserStrings(&strings.handle, batch, 128, &count)) && count) {
                for (ULONG index = 0; index < count; ++index) if (context.UserString(batch[index]) == L"UiHiddenPageDownload") m_setting = batch[index];
            }
            const auto get = context.Resolve(L"PCL.Settings", L"Get", L"!!0", {L"string", L"PCL.ModMinecraft/McInstance"}, false, 1);
            ManagedPatchContext::MetadataEnumeration methods{context.Metadata()};
            mdMethodSpec specs[128]{}; // 泛型调用中的 bool 类型参数必须完全匹配。
            while (SUCCEEDED(context.Metadata()->EnumMethodSpecs(&methods.handle, get, specs, 128, &count)) && count) {
                mdToken parent = 0; PCCOR_SIGNATURE signature = nullptr; ULONG size = 0;
                for (ULONG index = 0; index < count; ++index) {
                    if (SUCCEEDED(context.Metadata()->GetMethodSpecProps(specs[index], &parent, &signature, &size)) &&
                        size == 3 && signature[0] == IMAGE_CEE_CS_CALLCONV_GENERICINST && signature[1] == 1 && signature[2] == ELEMENT_TYPE_BOOLEAN) m_get = specs[index];
                }
            }
            m_forceShow = context.Resolve(L"PCL.PageSetupUI", L"get_HiddenForceShow", L"bool", {}, false);
            if (check_hidden) {
                const auto modMain = context.ResolveType(L"PCL.ModMain"); // 全局页面对象的声明模块。
                m_selectRight = context.ResolveField(modMain, L"FrmSelectRight", L"PCL.PageSelectRight");
                m_showHidden = context.ResolveField(context.ResolveType(L"PCL.PageSelectRight"), L"ShowHidden", L"bool");
            }
            if (!m_setting || !m_get) throw std::runtime_error("缺少下载入口显示设置，无法恢复原有的显示条件。");
        }

        // 在计算栈上留下可见/隐藏的结果，调用者负责赋给控件或状态变量。
        void Emit(IlAssembler& output, int visible_value = 0, int hidden_value = 2) const {
            const int checkHidden = output.Label(), visible = output.Label(), hidden = output.Label(), end = output.Label();
            output.Token(0x72, m_setting); output.Op(0x14); output.Token(0x28, m_get);
            output.Branch(0x39, checkHidden); output.Token(0x28, m_forceShow); output.Branch(0x39, hidden);
            output.Mark(checkHidden);
            if (m_checkHidden) {
                output.Token(0x7e, m_selectRight); output.Branch(0x39, visible);
                output.Token(0x7e, m_selectRight); output.Token(0x7b, m_showHidden); output.Branch(0x3a, hidden);
            }
            output.Mark(visible); output.Integer(visible_value); output.Branch(0x38, end);
            output.Mark(hidden); output.Integer(hidden_value); output.Mark(end);
        }

        // 生成的表达式有固定指令数，但变量、函数与字符串编号全部取自目标。
        size_t InstructionCount() const { return m_checkHidden ? 14 : 9; }
    };
}
