#pragma once
#include "IPatchFeature.hpp"
#include <functional>

namespace PclFix {
    // 共享单个函数的查找、扫描与重建流程；具体界面规则仍由各个子类负责。
    class MethodPatchFeature : public IPatchFeature {
    private:
        PatchFeatureInfo m_info; // 注册身份、名称和分类。
        std::wstring m_type; // 目标函数所属的完整类型名。
        std::wstring m_method; // 目标函数名，不依赖令牌和地址。

    protected:
        using Context = ManagedPatchContext;
        using Body = Context::MethodBody;
        using Instruction = Context::Instruction;

        MethodPatchFeature(PatchFeatureInfo info, std::wstring type, std::wstring method)
            : m_info(std::move(info)), m_type(std::move(type)), m_method(std::move(method)) {}

        // 精确检查类型和成员名，防止同名界面元素被误判。
        static bool Member(Context& context, const Instruction& instruction, const std::wstring& owner, const std::wstring& name) {
            mdToken type = 0; // 元数据中实际声明此成员的类型。
            if (TypeFromToken(instruction.token) == mdtFieldDef) {
                wchar_t fieldName[512]{}; // 字段名与方法名使用不同的元数据读取接口。
                return SUCCEEDED(context.Metadata()->GetFieldProps(instruction.token, &type, fieldName, 512, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr)) &&
                    fieldName == name && context.TypeName(type) == owner;
            }
            return context.MemberName(instruction.token, type) == name && context.TypeName(type) == owner;
        }

        // 扫描和写入均须满足同一结构要求，无法确定时停止整个修补。
        static void Require(bool condition) {
            if (!condition) throw std::runtime_error("此入口的代码与已支持的结构不同，无法安全修补。");
        }

        // 子类识别到正常结构后计数，识别到拦截后生成或预报一个方法修改。
        void Record(Context& context, mdMethodDef method, const Body& body, PatchAnalysis& analysis,
            bool build_patch, bool restricted, const std::function<void(IlAssembler&)>& emit) const {
            if (!restricted) { ++analysis.restoredCount; return; }
            if (!build_patch) { analysis.methods.push_back({}); return; }
            IlAssembler output; // 重建时统一重算跳转和异常处理位置。
            emit(output);
            analysis.methods.push_back(context.Assemble(method, body, output, m_info.category));
        }

        // 替换一个连续片段，其余原始指令及异常处理保持原有关系。
        static void Replace(Context& context, const Body& body, IlAssembler& output, size_t first, size_t last,
            const std::function<void(IlAssembler&)>& emit) {
            Require(first < last && last <= body.instructions.size());
            for (size_t index = 0; index < body.instructions.size(); ++index) {
                if (index == first) {
                    output.Mark(static_cast<int>(body.instructions[index].offset));
                    emit(output);
                    index = last - 1;
                } else context.CopyInstruction(output, body, body.instructions[index]);
            }
            output.Mark(static_cast<int>(body.length));
        }

        // 对比整段指令行为：整数和短跳转统一编码，忽略编译器的编码长度差异。
        static bool Matches(Context& context, const Body& body, size_t first, size_t last,
            const std::function<void(IlAssembler&)>& emit) {
            if (first >= last || last > body.instructions.size()) return false;
            try {
                IlAssembler actual, expected; // 分别生成目标现有代码与预期恢复代码。
                for (size_t index = first; index < last; ++index) {
                    const auto& instruction = body.instructions[index];
                    if (const auto value = context.IntegerValue(body, instruction)) {
                        actual.Mark(static_cast<int>(instruction.offset)); actual.Integer(*value);
                    } else context.CopyInstruction(actual, body, instruction);
                }
                actual.Mark(static_cast<int>(last == body.instructions.size() ? body.length : body.instructions[last].offset));
                emit(expected);
                std::map<int, uint32_t> actualOffsets, expectedOffsets; // 跳转位置只在各自代码片段内部比较。
                return actual.Build(actualOffsets) == expected.Build(expectedOffsets);
            } catch (const std::exception&) { return false; }
        }

        // 各子类只描述自身的拦截结构与恢复方法。
        virtual void AnalyzeMethod(Context& context, mdMethodDef method, const Body& body,
            PatchAnalysis& analysis, bool build_patch) const = 0;

    public:
        PatchFeatureInfo Describe() const override { return m_info; }

        // 同名函数有重载时不猜测；当前入口均只有一个目标函数。
        FeatureScanResult Scan(Context& context) const override {
            FeatureScanResult result; // 当前入口独立的扫描结果。
            result.info = Describe();
            mdTypeDef type = 0; // 目标页面类型。
            if (FAILED(context.Metadata()->FindTypeDefByName(m_type.c_str(), 0, &type))) return result;
            mdMethodDef method = 0; // 只允许唯一的方法定义，出现新重载时不猜测。
            for (const auto& symbol : context.Symbols()) {
                if (symbol.owner != m_type || symbol.name != m_method || TypeFromToken(symbol.token) != mdtMethodDef) continue;
                Require(!method); method = symbol.token;
            }
            if (!method) return result;
            result.typeDefinitions.push_back(type);
            PatchAnalysis probe; // 扫描不写入文件，也不注册新元数据。
            AnalyzeMethod(context, method, context.Body(method), probe, false);
            result.patchableCount = probe.TotalPoints();
            result.restoredCount = probe.restoredCount;
            result.detectedCount = result.patchableCount + result.restoredCount;
            result.state = result.patchableCount ? FeatureState::Restricted :
                (result.restoredCount ? FeatureState::Restored : FeatureState::NotFound);
            return result;
        }

        // 写入阶段重新检查目标函数，避免扫描和生成使用两套条件。
        void Patch(Context& context, const FeatureScanResult& result, PatchAnalysis& analysis) const override {
            Require(result.typeDefinitions.size() == 1);
            const auto method = context.FindNamedMethod(result.typeDefinitions.front(), m_method.c_str());
            Require(method != 0);
            AnalyzeMethod(context, method, context.Body(method), analysis, true);
        }

        // 必须从同一入口重新识别出正常代码，不能靠拦截文字消失判定成功。
        bool Verify(const PatchAnalysis& analysis, const FeatureScanResult& result) const override {
            const auto found = std::find_if(analysis.features.begin(), analysis.features.end(),
                [&result](const auto& item) { return item.info.id == result.info.id; });
            return found != analysis.features.end() && found->state == FeatureState::Restored &&
                found->patchableCount == 0 && found->restoredCount >= result.detectedCount;
        }
    };
}
