#pragma once
#include "IPatchFeature.hpp"
#include "../ManagedPatchContext.hpp"
#include <array>

namespace PclFix {

    // 负责恢复整合包格式识别后的安装器分发。
    class ModpackDispatcherFeature final : public IPatchFeature {
    private:
        // 一种整合包格式的识别文件及安装函数；空识别文件表示普通压缩包兜底。
        struct FormatHandler {
            const wchar_t* marker; // ZIP 内用于识别格式的文件名。
            const wchar_t* method; // 目标程序中已有的安装函数名。
            unsigned arguments; // 格式安装器额外需要的实例名、图标参数个数。
        };

        // 识别、重建与复检共用此表，最后一项固定为无格式标记的兜底安装器。
        inline static constexpr std::array<FormatHandler, 7> m_handlers{{
            {L"manifest.json", L"InstallPackCurseForge", 2},
            {L"modpack.json", L"InstallPackHMCL", 0},
            {L"mmc-pack.json", L"InstallPackMMC", 0},
            {L"mcbbs.packmeta", L"InstallPackMCBBS", 1},
            {L"modrinth.index.json", L"InstallPackModrinth", 2},
            {L"modpack.zip", L"InstallPackLauncherPack", 0},
            {L"", L"InstallPackCompress", 0}
        }};

        // 被删除安装分支的分发函数没有正常返回路径，格式判断后只会直接抛出异常。
        // 普通错误处理里的异常不能单独作为拦截依据，后续还会校验全部格式与安装函数。
        static size_t FindBlockedDispatch(ManagedPatchContext& context, const ManagedPatchContext::MethodBody& body) {
            const auto& items = body.instructions;
            if (std::any_of(items.begin(), items.end(), [](const auto& instruction) { return instruction.opcode == 0x2a; })) return items.size();
            size_t found = items.size(); // 未匹配时返回指令数量。
            for (size_t index = 0; index + 2 < items.size(); ++index) {
                if (!IsExceptionThrow(context, body, index)) continue;
                if (found != items.size()) throw std::runtime_error("找到多个直接报错的位置，无法确定整合包拦截位置。");
                found = index;
            }
            return found;
        }

        // 恢复格式分发时保留目标原有的 ZIP 识别、异常信息和 finally 释放逻辑，只重建被常量删除的尾部。
        static void RecoverModpackDispatch(ManagedPatchContext& context, mdMethodDef method, const ManagedPatchContext::MethodBody& body, const std::wstring& result_type, PatchAnalysis& analysis) {
            const auto locals = context.Locals(body); const auto& instructions = body.instructions;
            const size_t restriction = FindBlockedDispatch(context, body); int zipSlot = -1, prefixSlot = -1, resultSlot = -1, packSlot = -1;
            std::map<std::wstring, int> formats; // 格式编号从目标自己的识别分支提取。
            std::set<std::wstring> signatures; // 从描述表收集需要在 ZIP 内识别的文件。
            for (const auto& handler : m_handlers) if (handler.marker[0]) signatures.insert(handler.marker);
            for (size_t index = 0; index < locals.size(); ++index) if (locals[index] == result_type) { if (resultSlot >= 0) throw std::runtime_error("无法准确确定整合包安装结果的保存位置。"); resultSlot = static_cast<int>(index); }
            for (size_t index = 0; index < instructions.size(); ++index) {
                const auto& instruction = instructions[index]; mdToken owner = 0;
                if (instruction.opcode == 0x28 && context.MemberName(instruction.token, owner) == L"OpenZip" && context.TypeName(owner) == L"MeloongCore.FileUtils" && index + 1 < instructions.size()) zipSlot = context.LocalIndex(body, instructions[index + 1], true);
                if (instruction.opcode != 0x72 || index + 1 >= instructions.size()) continue;
                const auto text = context.UserString(instruction.token);
                if (text.empty() && prefixSlot < 0) prefixSlot = context.LocalIndex(body, instructions[index + 1], true);
                if (!signatures.contains(text) || formats.contains(text)) continue;
                // 常量必须紧跟 stloc，而且 GetEntry 的结果必须先经历分支判断。
                bool getEntry = false, condition = false;
                for (size_t next = index + 1; next + 1 < instructions.size() && next < index + 40; ++next) {
                    mdToken nextOwner = 0;
                    if (instructions[next].opcode == 0x6f && context.MemberName(instructions[next].token, nextOwner) == L"GetEntry" && context.TypeName(nextOwner) == L"System.IO.Compression.ZipArchive") getEntry = true;
                    if (!instructions[next].targets.empty()) condition = true;
                    const auto value = context.IntegerValue(body, instructions[next]); const int slot = context.LocalIndex(body, instructions[next + 1], true);
                    if (getEntry && condition && value && slot >= 0) {
                        if (packSlot >= 0 && packSlot != slot) throw std::runtime_error("整合包格式识别结果的保存方式与预期不同。");
                        packSlot = slot; formats.emplace(text, *value); break;
                    }
                }
            }
            auto validSlot = [&locals](int slot, const std::wstring& type) { return slot >= 0 && static_cast<size_t>(slot) < locals.size() && locals[slot] == type; };
            if (!body.extraSections || restriction == instructions.size() || restriction + 2 >= instructions.size() ||
                instructions[restriction + 1].opcode != 0x73 || instructions[restriction + 2].opcode != 0x7a ||
                !validSlot(zipSlot, L"System.IO.Compression.ZipArchive") || !validSlot(prefixSlot, L"string") ||
                !validSlot(packSlot, L"int32") || resultSlot < 0 || formats.size() != signatures.size()) throw std::runtime_error("整合包格式识别或下载拦截的实现方式暂不受支持。");
            const auto exceptionConstructor = context.Resolve(L"System.Exception", L".ctor", L"void", {L"string"}, true);
            if (instructions[restriction + 1].token != exceptionConstructor) throw std::runtime_error("下载拦截的报错方式与已支持版本不同，无法安全修补。");
            const size_t begin = instructions[restriction].offset, end = instructions[restriction + 2].offset + instructions[restriction + 2].size;
            std::set<int> values; // 不同格式的分类编号必须唯一，避免错误分发。
            IlAssembler output; const int finish = output.Label(); const int fallback = output.Label(); std::vector<int> labels;
            std::vector<mdToken> calls;
            for (const auto& handler : m_handlers) {
                std::vector<std::wstring> parameters{L"string", L"System.IO.Compression.ZipArchive"}; // 所有安装器的公共参数。
                if (handler.marker[0]) {
                    if (!values.insert(formats.at(handler.marker)).second) throw std::runtime_error("多个整合包格式使用了相同的编号，无法确定对应的安装方式。");
                    parameters.push_back(L"string"); // 具名格式需要归档内路径前缀，兜底安装器不需要。
                    labels.push_back(output.Label());
                }
                for (unsigned index = 0; index < handler.arguments; ++index) parameters.push_back(L"string");
                calls.push_back(context.Resolve(L"PCL.ModModpack", handler.method, result_type, parameters));
            }
            for (const auto& instruction : instructions) {
                if (instruction.offset == begin) {
                    output.Mark(static_cast<int>(begin));
                    for (size_t index = 0; index < labels.size(); ++index) { output.Local(false, packSlot); output.Integer(formats.at(m_handlers[index].marker)); output.Branch(0x3b, labels[index]); }
                    output.Branch(0x38, fallback);
                    for (size_t index = 0; index < labels.size(); ++index) {
                        output.Mark(labels[index]); output.Op(0x02); output.Local(false, zipSlot); output.Local(false, prefixSlot);
                        if (m_handlers[index].arguments >= 1) output.Op(0x03);
                        if (m_handlers[index].arguments >= 2) output.Op(0x04);
                        output.Token(0x28, calls[index]); output.Local(true, resultSlot); output.Branch(0xdd, finish);
                    }
                    output.Mark(fallback); output.Op(0x02); output.Local(false, zipSlot); output.Token(0x28, calls.back()); output.Local(true, resultSlot); output.Branch(0xdd, finish);
                }
                if (instruction.offset < begin || instruction.offset >= end) context.CopyInstruction(output, body, instruction);
            }
            output.Mark(static_cast<int>(body.length)); output.Mark(finish); output.Local(false, resultSlot); output.Op(0x2a);
            analysis.methods.push_back(context.Assemble(method, body, output, PatchPointKind::ModpackDownload));
        }

        // 只处理带 ZIP 识别和格式安装器分发的静态入口。
        static void AnalyzeType(ManagedPatchContext& context, mdTypeDef type, PatchAnalysis& analysis, bool build_patch) {
            context.LoadSymbols();
            std::vector<ManagedPatchContext::MemberSymbol> entries;
            for (const auto& member : context.Symbols()) if (member.owner == context.TypeName(type) && member.name == L"ModpackInstall" && TypeFromToken(member.token) == mdtMethodDef) entries.push_back(member);
            const auto dispatch = std::find_if(entries.begin(), entries.end(), [](const auto& member) {
                return !member.signature.instance && member.signature.parameters == std::vector<std::wstring>{L"string", L"string", L"string"};
            });
            if (dispatch == entries.end()) throw std::runtime_error("未找到根据整合包格式选择安装方式的功能。");
            const auto& entry = *dispatch;
            const auto body = context.Body(entry.token);
            const bool restricted = FindBlockedDispatch(context, body) != body.instructions.size();
            if (!restricted) {
                if (!body.extraSections) throw std::runtime_error("整合包安装功能缺少预期的异常处理代码，无法确认其是否完整。");
                std::set<std::wstring> calls;
                for (const auto& instruction : body.instructions) {
                    if (instruction.opcode != 0x28 && instruction.opcode != 0x6f) continue;
                    mdToken owner = 0;
                    const auto name = context.MemberName(instruction.token, owner);
                    if (context.TypeName(owner) == L"PCL.ModModpack") calls.insert(name);
                }
                for (const auto& handler : m_handlers) {
                    if (!calls.contains(handler.method)) throw std::runtime_error("整合包安装功能不完整或已被改动，无法确认是否正常。");
                }
                ++analysis.restoredCount;
                return;
            }
            if (entry.signature.result != L"PCL.ModLoader/LoaderCombo`1<string>") throw std::runtime_error("整合包安装函数的参数或返回类型已改变，无法安全修补。");
            if (!build_patch) { analysis.methods.push_back({}); return; }
            RecoverModpackDispatch(context, entry.token, body, entry.signature.result, analysis);
        }

    public:
        // 返回该具体功能的稳定身份、显示名称和分类。
        PatchFeatureInfo Describe() const override { return {"modpack-dispatcher", "整合包格式安装", PatchPointKind::ModpackDownload, true}; }

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
