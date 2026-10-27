#pragma once
#include "IPatchFeature.hpp"
#include "../ManagedPatchContext.hpp"

namespace PclFix {

    // 负责发现并恢复 Minecraft 下载页面的客户端列表加载器。
    class DownloadPageLoaderFeature final : public IPatchFeature {
    private:

        // 正常下载加载器的文件内身份，不会保存到注册对象中。
        struct LoaderBinding {
            mdFieldDef field = 0; // 当前程序集的静态字段令牌。
            ManagedPatchContext::Bytes signature; // 验证任务构造器所需的字段签名。
        };

        // 从实际元数据中查找正常下载加载器及其字段类型签名。
        static LoaderBinding FindLoader(ManagedPatchContext& context) {
            LoaderBinding binding; // 当前文件的正常加载器及签名。
            mdTypeDef type = 0;
            if (FAILED(context.Metadata()->FindTypeDefByName(L"PCL.ModDownload", 0, &type))) throw std::runtime_error("未找到 PCL 下载模块。");
            ManagedPatchContext::MetadataEnumeration enumeration{context.Metadata()};
            mdFieldDef fields[128]{}; ULONG count = 0;
            while (SUCCEEDED(context.Metadata()->EnumFields(&enumeration.handle, type, fields, 128, &count)) && count) {
                for (ULONG index = 0; index < count; ++index) {
                    wchar_t name[512]{}; PCCOR_SIGNATURE signature = nullptr; ULONG size = 0; DWORD attributes = 0;
                    if (SUCCEEDED(context.Metadata()->GetFieldProps(fields[index], nullptr, name, 512, nullptr, &attributes, &signature, &size, nullptr, nullptr, nullptr)) && std::wstring(name) == L"DlClientListLoader" && (attributes & fdStatic)) {
                        binding.field = fields[index]; binding.signature.assign(signature, signature + size);
                    }
                }
            }
            if (!binding.field || binding.signature.size() < 4 || binding.signature[0] != IMAGE_CEE_CS_CALLCONV_FIELD) throw std::runtime_error("无法找到“原版游戏”版本列表的加载功能，无法安全修补。");
            return binding;
        }

        // 确认构造对象与正常字段具有同一个泛型类型签名。
        static bool IsLoaderConstructor(ManagedPatchContext& context, mdToken token, const LoaderBinding& binding) {
            mdToken owner = 0;
            if (context.MemberName(token, owner) != L".ctor" || TypeFromToken(owner) != mdtTypeSpec) return false;
            PCCOR_SIGNATURE signature = nullptr; ULONG size = 0;
            if (FAILED(context.Metadata()->GetTypeSpecFromToken(owner, &signature, &size))) return false;
            return size + 1 == binding.signature.size() && std::equal(signature, signature + size, binding.signature.begin() + 1);
        }

        // 检查被删除的表达式只构造一个任务，不读取表达式外的栈值。
        // 所有分支合流时栈深必须一致，防止仅凭字符串误改相似代码。
        static void ValidateExpression(ManagedPatchContext& context, const ManagedPatchContext::MethodBody& body, size_t first_index, size_t end_offset) {
            std::vector<int> depths(body.instructions.size(), -1); // 各指令入口的相对栈深。
            std::vector<size_t> pending{first_index}; // 尚待验证的控制流节点。
            depths[first_index] = 0;
            bool reachesEnd = false; // 至少存在一条正常到达表达式末尾的路径。
            while (!pending.empty()) {
                const size_t index = pending.back(); pending.pop_back();
                const auto& instruction = body.instructions[index];
                int pop = 0, push = 0; // 当前指令的相对栈效果。
                const unsigned opcode = instruction.opcode;
                if (opcode == 0x00 || opcode == 0x2b || opcode == 0x38) {}
                else if ((opcode >= 0x02 && opcode <= 0x05) || opcode == 0x0e || opcode == 0xfe09 ||
                    opcode == 0x14 || (opcode >= 0x15 && opcode <= 0x20) || opcode == 0x72 || opcode == 0x7e || opcode == 0xfe06) push = 1;
                else if (opcode == 0x25) { pop = 1; push = 2; }
                else if (opcode == 0x80 || opcode == 0x2c || opcode == 0x2d || opcode == 0x39 || opcode == 0x3a) pop = 1;
                else if (opcode == 0x73) {
                    PCCOR_SIGNATURE signature = nullptr; ULONG length = 0; mdToken owner = 0;
                    if (context.MemberName(instruction.token, owner) != L".ctor") throw std::runtime_error("下载拦截的实现方式与已支持版本不同，无法安全修补。");
                    HRESULT result = E_FAIL;
                    if (TypeFromToken(instruction.token) == mdtMemberRef) result = context.Metadata()->GetMemberRefProps(instruction.token, nullptr, nullptr, 0, nullptr, &signature, &length);
                    else if (TypeFromToken(instruction.token) == mdtMethodDef) result = context.Metadata()->GetMethodProps(instruction.token, nullptr, nullptr, 0, nullptr, nullptr, &signature, &length, nullptr, nullptr);
                    if (FAILED(result) || length < 2 || (signature[0] & IMAGE_CEE_CS_CALLCONV_GENERIC)) throw std::runtime_error("创建下载任务的方式暂不受支持。");
                    const auto* cursor = signature + 1; // 构造器无泛型方法参数，下一项为参数个数。
                    const size_t width = (*cursor & 0x80) == 0 ? 1 : ((*cursor & 0xc0) == 0x80 ? 2 : 4);
                    if (length < 1 + width || (*cursor & 0xe0) == 0xe0) throw std::runtime_error("创建下载任务所需的信息不完整，文件可能已损坏。");
                    pop = static_cast<int>(CorSigUncompressData(cursor)); push = 1;
                } else throw std::runtime_error("下载拦截使用了暂不支持的指令，已停止修补。");
                if (depths[index] < pop) throw std::runtime_error("下载拦截与其他代码存在依赖，无法单独修改。");
                const int depth = depths[index] - pop + push;
                auto visit = [&](size_t target) {
                    if (target == end_offset) {
                        if (depth != 1) throw std::runtime_error("下载拦截的计算方式与预期不同，无法安全修补。");
                        reachesEnd = true; return;
                    }
                    if (target < body.instructions[first_index].offset || target >= end_offset) throw std::runtime_error("下载拦截会跳转到其他代码，无法安全修补。");
                    const auto found = std::find_if(body.instructions.begin(), body.instructions.end(), [target](const auto& item) { return item.offset == target; });
                    if (found == body.instructions.end()) throw std::runtime_error("下载拦截包含无效的跳转位置，文件可能已损坏。");
                    const size_t next = static_cast<size_t>(found - body.instructions.begin());
                    if (depths[next] == -1) { depths[next] = depth; pending.push_back(next); }
                    else if (depths[next] != depth) throw std::runtime_error("下载拦截各个分支的数据处理方式不一致，无法安全修补。");
                };
                for (const size_t target : instruction.targets) {
                    if (target <= instruction.offset) throw std::runtime_error("下载拦截中存在循环，当前版本暂不支持修补。");
                    visit(target);
                }
                if (opcode != 0x2b && opcode != 0x38) visit(instruction.offset + instruction.size);
            }
            if (!reachesEnd) throw std::runtime_error("无法确定下载拦截执行后的返回位置，已停止修补。");
        }

        // 确认任务回调只会抛错，或通过编译器生成的缓存委托调用同样只抛错的回调。
        // 不检查异常文字、任务标题和编译器生成的函数编号。
        static bool IsThrowingCallback(ManagedPatchContext& context, mdToken method, unsigned depth = 0) {
            if (depth > 2 || TypeFromToken(method) != mdtMethodDef) return false;
            const auto body = context.Body(method);
            const auto& items = body.instructions;
            if (body.extraSections) return false;
            if (items.size() == 3) return IsExceptionThrow(context, body, 0);
            if (items.size() != 11 || items[0].opcode != 0x7e || items[1].opcode != 0x2c ||
                items[2].opcode != 0x7e || items[3].opcode != 0x2b || items[4].opcode != 0x7e ||
                items[5].opcode != 0xfe06 || items[6].opcode != 0x73 || items[7].opcode != 0x25 ||
                items[8].opcode != 0x80 || items[9].opcode != 0x6f || items[10].opcode != 0x2a ||
                items[0].token != items[2].token || items[0].token != items[8].token ||
                items[1].targets != std::vector<size_t>{items[4].offset} || items[3].targets != std::vector<size_t>{items[9].offset}) return false;
            mdToken constructorOwner = 0, invokeOwner = 0; // 委托创建和调用必须属于同一个类型。
            if (context.MemberName(items[6].token, constructorOwner) != L".ctor" ||
                context.MemberName(items[9].token, invokeOwner) != L"Invoke" || constructorOwner != invokeOwner) return false;
            const auto constructor = context.Signature(items[6].token), invoke = context.Signature(items[9].token);
            return constructor.instance && constructor.parameters == std::vector<std::wstring>{L"object", L"native int"} &&
                invoke.instance && invoke.result == L"void" && invoke.parameters.empty() && IsThrowingCallback(context, items[5].token, depth + 1);
        }

        // 识别并生成下载页面加载器的等长 IL 修改。
        static void AnalyzeType(ManagedPatchContext& context, mdTypeDef type, PatchAnalysis& analysis, bool build_patch) {
            const auto binding = FindLoader(context); // 每次分析使用目标自身的字段身份。
            mdMethodDef method = 0;
            if (FAILED(context.Metadata()->FindMethod(type, L"LoaderInit", nullptr, 0, &method))) return;
            const auto body = context.Body(method);
            if (body.extraSections) throw std::runtime_error("下载页面包含暂不支持的异常处理代码，请更新修补器后重试。");
            bool callsPageLoader = false;
            for (const auto& instruction : body.instructions) {
                if (instruction.opcode == 0x28 || instruction.opcode == 0x6f) {
                    mdToken owner = 0;
                    if (context.MemberName(instruction.token, owner) == L"PageLoaderInit" && context.TypeName(owner) == L"PCL.MyPageRight") callsPageLoader = true;
                }
            }
            if (!callsPageLoader) return;
            size_t restrictionCount = 0;
            for (size_t index = 0; index < body.instructions.size(); ++index) {
                const auto& instruction = body.instructions[index];
                if (instruction.opcode == 0x7e && instruction.token == binding.field) ++analysis.restoredCount;
                if (instruction.opcode != 0x72) continue;
                size_t end = 0; // 从任务名参数开始定位同类型加载器构造，不读取任务名内容。
                mdToken callback = 0; // 此任务唯一的回调方法。
                bool multipleCallbacks = false; // 多回调任务不能按只报错任务处理。
                for (size_t next = index + 1; next < body.instructions.size(); ++next) {
                    const auto& candidate = body.instructions[next];
                    if (candidate.opcode == 0xfe06) { if (callback) multipleCallbacks = true; callback = candidate.token; }
                    if (candidate.opcode == 0x73 && IsLoaderConstructor(context, candidate.token, binding)) { end = candidate.offset + candidate.size; break; }
                }
                if (!end) continue; // 与加载器构造无关的普通字符串不是修补点。
                if (end - instruction.offset < 5 || !callback || multipleCallbacks || !IsThrowingCallback(context, callback)) {
                    throw std::runtime_error("无法确认下载任务是否只用于报错，已停止修补。");
                }
                for (const auto& branch : body.instructions) for (const size_t target : branch.targets) {
                    const bool insideSource = branch.offset >= instruction.offset && branch.offset < end;
                    const bool insideTarget = target >= instruction.offset && target < end;
                    if ((!insideSource && insideTarget && target != instruction.offset) || (insideSource && !insideTarget && target != end)) throw std::runtime_error("其他代码会跳入下载拦截内部，无法安全修改。");
                }
                ValidateExpression(context, body, index, end);
                if (!build_patch) { analysis.ranges.push_back({}); ++restrictionCount; continue; }
                PatchRange range;
                range.offset = body.offset + instruction.offset;
                range.kind = PatchPointKind::DownloadPage;
                range.before.assign(context.BytesView().begin() + range.offset, context.BytesView().begin() + body.offset + end);
                range.after.assign(range.before.size(), 0);
                range.after[0] = 0x7e;
                std::memcpy(range.after.data() + 1, &binding.field, 4);
                analysis.ranges.push_back(std::move(range));
                ++restrictionCount;
            }
            if (restrictionCount > 1) throw std::runtime_error("同一页面发现多处下载拦截，无法确定正确的修改位置。");
        }

    public:
        // 返回该具体功能的稳定身份、显示名称和分类。
        PatchFeatureInfo Describe() const override {
            return {"download-page-loader", "原版游戏版本列表", PatchPointKind::DownloadPage, true};
        }

        // 只读检查自身结构，未匹配不能当作已经恢复。
        FeatureScanResult Scan(ManagedPatchContext& context) const override {
            FeatureScanResult result; // 本文件的特征状态与目标类型集合。
            result.info = Describe();
            for (const auto type : context.TypeDefinitions()) {
                if (context.TypeName(type).starts_with(L"PCL.PageDownload")) result.typeDefinitions.push_back(type);
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
