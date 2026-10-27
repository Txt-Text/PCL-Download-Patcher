#pragma once
#include "IPatchFeature.hpp"
#include "../ManagedPatchContext.hpp"

namespace PclFix {

    // 负责恢复资源详情和整合包详情中的条件安装入口。
    class ResourceInstallFeature final : public IPatchFeature {
    private:
        // 直接读取目标或目标嵌入依赖中的 ModPack 命名常量，不硬编码资源类型编号。
        static int ResourceModpackValue(ManagedPatchContext& context, mdFieldDef field) {
            const auto typeName = context.FieldType(field);
            if (const auto value = context.EnumLiteral(context.Metadata(), typeName, L"ModPack")) return *value;
            const auto dependency = context.EnumDependencyName(field);
            if (!dependency.empty()) if (const auto value = context.EmbeddedEnum(dependency, typeName, L"ModPack")) return *value;
            throw std::runtime_error("无法区分整合包和其他资源，已停止修补以免错改。");
        }

        // 查找编译器生成的按钮点击处理函数：参数和返回类型必须匹配，且只转换 sender 的类型再调用 Install_Click。
        static mdMethodDef ResourceInstallAdapter(ManagedPatchContext& context, mdTypeDef page, mdMethodDef install, const std::vector<std::wstring>& parameters) {
            context.LoadSymbols(); mdMethodDef result = 0;
            for (const auto& member : context.Symbols()) {
                if (member.owner != context.TypeName(page) || !member.signature.instance || member.signature.result != L"void" ||
                    member.signature.parameters != parameters || TypeFromToken(member.token) != mdtMethodDef) continue;
                const auto body = context.Body(member.token); const auto& items = body.instructions;
                if (items.size() != 6 || items[0].opcode != 0x02 || items[1].opcode != 0x03 || items[2].opcode != 0x74 ||
                    context.TypeName(items[2].token) != L"PCL.MyListItem" || items[3].opcode != 0x04 || items[4].opcode != 0x28 ||
                    items[4].token != install || items[5].opcode != 0x2a) continue;
                // 如果编译器生成了多个作用完全相同的按钮点击处理函数，可以使用其中任意一个，不依赖自动生成的编号。
                if (!result) result = member.token;
            }
            if (!result) throw std::runtime_error("无法找到可用的整合包安装按钮处理函数，无法修补。");
            return result;
        }

        // 恢复资源详情列表中仅整合包显示安装按钮、其他资源仅保存的条件表达式。
        static void AnalyzeType(ManagedPatchContext& context, mdTypeDef page, PatchAnalysis& analysis, bool build_patch) {
            const auto pageName = context.TypeName(page);
            context.LoadSymbols();
            bool hasListBuilder = false;
            for (const auto& member : context.Symbols()) {
                if (member.owner != pageName || TypeFromToken(member.token) != mdtMethodDef) continue;
                ManagedPatchContext::MethodBody body;
                try { body = context.Body(member.token); } catch (const std::exception&) { continue; }
                for (const auto& instruction : body.instructions) {
                    mdToken owner = 0;
                    if (instruction.opcode == 0x6f && context.MemberName(instruction.token, owner) == L"ToListItem" && context.TypeName(owner) == L"PCL.ResourceVersion") hasListBuilder = true;
                }
            }
            if (!hasListBuilder) return;
            const auto save = context.Resolve(pageName, L"Save_Click", L"void", {L"object", L"System.EventArgs"}, true);
            const auto install = context.Resolve(pageName, L"Install_Click", L"void", {L"PCL.MyListItem", L"System.EventArgs"}, true);
            const auto listConstructor = context.Resolve(L"PCL.MyListItem/ClickEventHandler", L".ctor", L"void", {L"object", L"native int"}, true);
            const auto iconConstructor = context.Resolve(L"PCL.MyIconButton/ClickEventHandler", L".ctor", L"void", {L"object", L"native int"}, true);
            const auto invoke = context.FindNamedMethod(static_cast<mdTypeDef>(context.ResolveType(L"PCL.MyListItem/ClickEventHandler")), L"Invoke");
            if (!invoke) throw std::runtime_error("无法读取资源列表按钮的点击处理方式，此版本暂不支持修补。");
            const auto adapter = ResourceInstallAdapter(context, page, install, context.Signature(invoke).parameters);
            const auto field = context.ResolveField(page, L"TargetResourceType", context.FieldTypeByName(page, L"TargetResourceType"));
            const int modpackValue = ResourceModpackValue(context, field);
            for (const auto& member : context.Symbols()) {
                if (member.owner != pageName || TypeFromToken(member.token) != mdtMethodDef) continue;
                ManagedPatchContext::MethodBody body;
                try { body = context.Body(member.token); } catch (const std::exception&) { continue; }
                const auto& items = body.instructions;
                size_t callCount = 0, callIndex = 0;
                for (size_t index = 0; index < items.size(); ++index) {
                    mdToken owner = 0;
                    if (items[index].opcode == 0x6f && context.MemberName(items[index].token, owner) == L"ToListItem" && context.TypeName(owner) == L"PCL.ResourceVersion") { ++callCount; callIndex = index; }
                }
                if (!callCount) continue;
                if (callCount != 1 || callIndex < 5) throw std::runtime_error("无法准确确定资源列表中需要修改的位置，已停止修补。");
                bool restored = false;
                for (size_t index = 0; index + 16 < items.size(); ++index) {
                    if (items[index].opcode != 0x02 || items[index + 1].opcode != 0x7b || items[index + 1].token != field ||
                        context.IntegerValue(body, items[index + 2]) != modpackValue || items[index + 3].opcode != 0x40 ||
                        items[index + 3].targets != std::vector<size_t>{items[index + 11].offset}) continue;
                    restored = items[index + 4].opcode == 0x02 && items[index + 5].opcode == 0xfe06 && items[index + 5].token == adapter &&
                        items[index + 6].opcode == 0x73 && items[index + 6].token == listConstructor && items[index + 7].opcode == 0x02 &&
                        items[index + 8].opcode == 0xfe06 && items[index + 8].token == save && items[index + 9].opcode == 0x73 && items[index + 9].token == iconConstructor &&
                        items[index + 10].opcode == 0x38 && items[index + 10].targets == std::vector<size_t>{items[index + 15].offset} &&
                        items[index + 11].opcode == 0x02 && items[index + 12].opcode == 0xfe06 && items[index + 12].token == save &&
                        items[index + 13].opcode == 0x73 && items[index + 13].token == listConstructor && items[index + 14].opcode == 0x14 && index + 16 == callIndex;
                    if (restored) break;
                }
                if (restored) { ++analysis.restoredCount; continue; }
                const auto shape = context.Signature(items[callIndex].token);
                if (shape.parameters != std::vector<std::wstring>{L"PCL.MyListItem/ClickEventHandler", L"PCL.MyIconButton/ClickEventHandler", L"bool"} ||
                    items[callIndex - 5].opcode != 0x02 || items[callIndex - 4].opcode != 0xfe06 ||
                    (items[callIndex - 4].token != save && items[callIndex - 4].token != install) ||
                    items[callIndex - 3].opcode != 0x73 || items[callIndex - 3].token != listConstructor ||
                    items[callIndex - 2].opcode != 0x14 || context.LocalIndex(body, items[callIndex - 1], false) < 0) throw std::runtime_error("资源列表的安装方式与已支持版本不同，无法安全修补。");
                if (!build_patch) { analysis.methods.push_back({}); continue; }
                const size_t begin = items[callIndex - 5].offset, end = items[callIndex - 1].offset;
                IlAssembler output; const int saveOnly = output.Label(); const int done = output.Label();
                for (const auto& instruction : items) {
                    if (instruction.offset == begin) {
                        output.Mark(static_cast<int>(begin)); output.Op(0x02); output.Token(0x7b, field); output.Integer(modpackValue); output.Branch(0x40, saveOnly);
                        output.Op(0x02); output.Token(0xfe06, adapter); output.Token(0x73, listConstructor);
                        output.Op(0x02); output.Token(0xfe06, save); output.Token(0x73, iconConstructor); output.Branch(0x38, done);
                        output.Mark(saveOnly); output.Op(0x02); output.Token(0xfe06, save); output.Token(0x73, listConstructor); output.Op(0x14);
                        output.Mark(done);
                    }
                    if (instruction.offset < begin || instruction.offset >= end) context.CopyInstruction(output, body, instruction);
                }
                output.Mark(static_cast<int>(body.length));
                analysis.methods.push_back(context.Assemble(member.token, body, output, PatchPointKind::ResourceInstall));
            }
        }

    public:
        // 返回该具体功能的稳定身份、显示名称和分类。
        PatchFeatureInfo Describe() const override { return {"resource-install", "资源列表的安装按钮", PatchPointKind::ResourceInstall, true}; }

        // 只读检查自身结构，未匹配不能当作已经恢复。
        FeatureScanResult Scan(ManagedPatchContext& context) const override {
            FeatureScanResult result; // 本文件的特征状态与目标类型集合。
            result.info = Describe();
            for (const auto type : context.TypeDefinitions()) {
                const auto name = context.TypeName(type);
                if (name == L"PCL.PageDownloadResourceDetail" || name == L"PCL.PageDownloadCompDetail") result.typeDefinitions.push_back(type);
            }
            PatchAnalysis probe; // 仅用于记录扫描命中的结构，不生成候选 PE。
            for (const auto type : result.typeDefinitions) AnalyzeType(context, type, probe, false);
            result.patchableCount = probe.TotalPoints();
            result.restoredCount = probe.restoredCount;
            result.detectedCount = result.patchableCount + result.restoredCount;
            result.state = result.patchableCount ? FeatureState::Restricted :
                (result.restoredCount ? FeatureState::Restored : FeatureState::NotFound);
            return result;
        }

        // 只有受限结构才生成修改；注册表负责标记身份并合并完整计划。
        void Patch(ManagedPatchContext& context, const FeatureScanResult& result, PatchAnalysis& analysis) const override {
            for (const auto type : result.typeDefinitions) AnalyzeType(context, type, analysis, true);
        }

        // 根据具体特征身份复查恢复数量，不借用同类别其他功能的结果。
        bool Verify(const PatchAnalysis& analysis, const FeatureScanResult& result) const override {
            const auto after = std::find_if(analysis.features.begin(), analysis.features.end(), [&result](const auto& item) { return item.info.id == result.info.id; });
            return analysis.error.empty() && after != analysis.features.end() && after->patchableCount == 0 &&
                after->restoredCount >= result.restoredCount + result.patchableCount;
        }
    };

}
