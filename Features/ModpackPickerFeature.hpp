#pragma once
#include "IPatchFeature.hpp"
#include "../ManagedPatchContext.hpp"

namespace PclFix {

    // 负责恢复整合包文件选择和后台安装入口。
    class ModpackPickerFeature final : public IPatchFeature {
    private:
        // 泛型调用按定义的语义和实参选择，不能复用另一程序集的 MethodSpec 令牌。
        static mdToken ResolveFirstString(ManagedPatchContext& context) {
            context.LoadSymbols();
            for (const auto& member : context.Symbols()) {
                if (member.owner != L"System.Linq.Enumerable" || member.name != L"FirstOrDefault" || member.signature.parameters.size() != 1) continue;
                ManagedPatchContext::MetadataEnumeration enumeration{context.Metadata()}; mdMethodSpec items[128]{}; ULONG count = 0;
                while (SUCCEEDED(context.Metadata()->EnumMethodSpecs(&enumeration.handle, member.token, items, 128, &count)) && count) {
                    for (ULONG index = 0; index < count; ++index) {
                        mdToken parent = 0, owner = 0; PCCOR_SIGNATURE signature = nullptr; ULONG size = 0;
                        if (FAILED(context.Metadata()->GetMethodSpecProps(items[index], &parent, &signature, &size)) || size != 3 ||
                            signature[0] != IMAGE_CEE_CS_CALLCONV_GENERICINST || signature[1] != 1 || signature[2] != ELEMENT_TYPE_STRING) continue;
                        if (context.MemberName(parent, owner) != L"FirstOrDefault" || context.TypeName(owner) != L"System.Linq.Enumerable") continue;
                        const auto shape = context.Signature(parent);
                        if (!shape.instance && shape.genericCount == 1 && shape.result == L"!!0" && shape.parameters == std::vector<std::wstring>{L"System.Collections.Generic.IEnumerable`1<!!0>"}) return items[index];
                    }
                }
            }
            throw std::runtime_error("无法读取整合包文件选择结果，无法修补。");
        }

        // 用目标自身的闭包、文件对话框和后台线程恢复无参数入口；示例文件不参与运行时匹配。
        static void RecoverModpackEntry(ManagedPatchContext& context, mdMethodDef method, mdMethodDef dispatch, const ManagedPatchContext::MethodBody& body, PatchAnalysis& analysis) {
            const auto locals = context.Locals(body);
            if (body.extraSections || locals.size() != 1 || body.instructions.empty() || body.instructions.front().opcode != 0x73) throw std::runtime_error("整合包文件选择功能的实现方式暂不受支持。");
            mdToken closureType = 0; const mdToken constructor = body.instructions.front().token;
            if (context.MemberName(constructor, closureType) != L".ctor" || TypeFromToken(closureType) != mdtTypeDef || locals.front() != context.TypeName(closureType) || !locals.front().starts_with(L"PCL.ModModpack/")) throw std::runtime_error("整合包文件选择与后台安装的连接方式暂不受支持。");
            mdToken callback = 0; // 根据真实调用关系选择后台入口，不依赖 VB 的闭包编号。
            mdFieldDef fileField = 0; // 保存文件路径的闭包字段也按实际调用关系识别。
            context.LoadSymbols();
            for (const auto& member : context.Symbols()) {
                if (member.owner != context.TypeName(closureType) || !member.signature.instance || member.signature.result != L"void" || !member.signature.parameters.empty() || member.name == L".ctor" || TypeFromToken(member.token) != mdtMethodDef) continue;
                const auto candidate = context.Body(member.token);
                if (std::any_of(candidate.instructions.begin(), candidate.instructions.end(), [dispatch](const auto& instruction) { return instruction.opcode == 0x28 && instruction.token == dispatch; })) {
                    if (callback) throw std::runtime_error("找到多个整合包后台安装函数，无法确定应使用哪一个。");
                    callback = member.token;
                    for (const auto& instruction : candidate.instructions) {
                        if (instruction.opcode != 0x7b || TypeFromToken(instruction.token) != mdtFieldDef || context.FieldType(instruction.token) != L"string") continue;
                        mdTypeDef owner = 0;
                        if (FAILED(context.Metadata()->GetFieldProps(instruction.token, &owner, nullptr, 0, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr)) || owner != closureType) continue;
                        if (fileField && fileField != instruction.token) throw std::runtime_error("无法确定后台安装应读取哪个整合包文件路径。");
                        fileField = instruction.token;
                    }
                }
            }
            if (!callback || !fileField) throw std::runtime_error("整合包后台安装函数或文件路径信息缺失，无法恢复安装功能。");
            const std::wstring tuple = L"System.ValueTuple`2<string[],string>";
            const std::wstring options = L"MeloongCore.OneOf`2<System.Collections.Generic.IEnumerable`1<" + tuple + L">,string>";
            const std::wstring nullable = L"System.Nullable`1<" + options + L">";
            const auto tupleSignature = context.GenericSignature(L"System.ValueTuple`2", true, {{ELEMENT_TYPE_SZARRAY, ELEMENT_TYPE_STRING}, {ELEMENT_TYPE_STRING}});
            const auto enumerableSignature = context.GenericSignature(L"System.Collections.Generic.IEnumerable`1", false, {tupleSignature});
            const auto optionsSignature = context.GenericSignature(L"MeloongCore.OneOf`2", true, {enumerableSignature, {ELEMENT_TYPE_STRING}});
            const auto nullableSignature = context.GenericSignature(L"System.Nullable`1", true, {optionsSignature});
            const auto tupleType = context.TypeSpec(tupleSignature);
            const auto tupleConstructor = context.BindGeneric(tupleType, L"System.ValueTuple`2", L".ctor", L"void", {L"!0", L"!1"}, true);
            const auto implicit = context.BindGeneric(context.TypeSpec(optionsSignature), L"MeloongCore.OneOf`2", L"op_Implicit", L"MeloongCore.OneOf`2<!0,!1>", {L"!0"}, false);
            const auto nullableConstructor = context.BindGeneric(context.TypeSpec(nullableSignature), L"System.Nullable`1", L".ctor", L"void", {L"!0"}, true);
            const auto select = context.Resolve(L"MeloongCore.Wpf.Dialogs", L"SelectFile", L"System.Collections.Generic.List`1<string>", {L"string", L"bool", L"string", nullable});
            const auto first = ResolveFirstString(context);
            const auto empty = context.Resolve(L"System.String", L"IsNullOrEmpty", L"bool", {L"string"});
            const auto action = context.Resolve(L"System.Action", L".ctor", L"void", {L"object", L"native int"}, true);
            const auto run = context.Resolve(L"PCL.ModBase", L"RunInThread", L"void", {L"System.Action"});
            IlAssembler output; const int done = output.Label();
            output.Token(0x73, constructor); output.Local(true, 0);
            output.Local(false, 0); output.Token(0x72, context.StringToken(L"选择整合包压缩文件")); output.Op(0x16); output.Op(0x14);
            output.Op(0x17); output.Token(0x8d, tupleType); output.Op(0x25); output.Op(0x16);
            output.Op(0x19); output.Token(0x8d, context.ResolveType(L"System.String"));
            const std::wstring extensions[] = {L"rar", L"zip", L"mrpack"}; // 保持原版对话框的格式提示与错误处理。
            for (int index = 0; index < 3; ++index) { output.Op(0x25); output.Integer(index); output.Token(0x72, context.StringToken(extensions[index])); output.Op(0xa2); }
            output.Token(0x72, context.StringToken(L"整合包文件")); output.Token(0x73, tupleConstructor); output.Token(0xa4, tupleType);
            output.Token(0x28, implicit); output.Token(0x73, nullableConstructor); output.Token(0x28, select); output.Token(0x28, first);
            output.Token(0x7d, fileField); output.Local(false, 0); output.Token(0x7b, fileField); output.Token(0x28, empty); output.Branch(0x3a, done);
            output.Local(false, 0); output.Token(0xfe06, callback); output.Token(0x73, action); output.Token(0x28, run);
            output.Mark(done); output.Op(0x2a);
            analysis.methods.push_back(context.Assemble(method, body, output, PatchPointKind::ModpackDownload));
        }

        // 只处理无参数的整合包文件选择入口。
        static void AnalyzeType(ManagedPatchContext& context, mdTypeDef type, PatchAnalysis& analysis, bool build_patch) {
            context.LoadSymbols();
            std::vector<ManagedPatchContext::MemberSymbol> entries;
            for (const auto& member : context.Symbols()) if (member.owner == context.TypeName(type) && member.name == L"ModpackInstall" && TypeFromToken(member.token) == mdtMethodDef) entries.push_back(member);
            const auto dispatch = std::find_if(entries.begin(), entries.end(), [](const auto& member) {
                return !member.signature.instance && member.signature.parameters == std::vector<std::wstring>{L"string", L"string", L"string"};
            });
            if (dispatch == entries.end()) throw std::runtime_error("未找到根据整合包格式选择安装方式的功能。");
            for (const auto& entry : entries) {
                if (entry.signature.instance || !entry.signature.parameters.empty() || entry.signature.result != L"void") continue;
                const auto body = context.Body(entry.token);
                const bool restricted = std::any_of(body.instructions.begin(), body.instructions.end(), [&context](const auto& instruction) {
                    return IsRestriction(context, instruction);
                });
                if (!restricted) {
                    bool picker = false;
                    bool background = false; // 文件选择和后台安装调用都必须存在。
                    for (const auto& instruction : body.instructions) {
                        if (instruction.opcode != 0x28 && instruction.opcode != 0x6f) continue;
                        mdToken owner = 0;
                        const auto name = context.MemberName(instruction.token, owner);
                        if (name == L"SelectFile" && context.TypeName(owner) == L"MeloongCore.Wpf.Dialogs") picker = true;
                        if (name == L"RunInThread" && context.TypeName(owner) == L"PCL.ModBase") background = true;
                    }
                    if (!picker || !background) throw std::runtime_error("整合包文件选择功能既未匹配到下载限制，也未匹配到完整的正常实现。");
                    ++analysis.restoredCount;
                    continue;
                }
                if (!build_patch) { analysis.methods.push_back({}); continue; }
                RecoverModpackEntry(context, entry.token, dispatch->token, body, analysis);
            }
        }

    public:
        // 返回该具体功能的稳定身份、显示名称和分类。
        PatchFeatureInfo Describe() const override { return {"modpack-picker", "整合包文件选择入口", PatchPointKind::ModpackDownload, true}; }

        // 只读检查自身结构，未匹配不能当作已经恢复。
        FeatureScanResult Scan(ManagedPatchContext& context) const override {
            FeatureScanResult result; // 本文件的特征状态与目标类型集合。
            result.info = Describe();
            for (const auto type : context.TypeDefinitions()) if (context.TypeName(type) == L"PCL.ModModpack") result.typeDefinitions.push_back(type);
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
