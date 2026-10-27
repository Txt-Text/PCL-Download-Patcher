// 只在输出目录生成副本，验证版本范围仅警告、实际结构决定是否修补。
#define main PatcherInteractiveMain
#include "../PCL-Download-Patcher.cpp"
#undef main

namespace PclFix {
    class VersionRegression {
    private:
        // 失败即终止，不把拒绝写入的检查降为警告。
        static void Require(bool condition, const char* message) {
            if (!condition) throw std::runtime_error(message);
        }

        // 用三套乱序登记的规则验证扩展能力，不修改生产规则表。
        static void CheckCatalog() {
            PatchRuleCatalog catalog; // 测试自己的版本范围和功能所有权。
            const auto makeRule = [](std::string id, TargetVersion first, TargetVersion last) {
                PatchRuleSet rule{std::move(id), "测试规则", "验证第三套规则及范围选择。", first, last, {}};
                rule.features.Register(std::make_unique<ModpackPickerFeature>());
                return rule;
            };
            const auto reject = [](auto operation) {
                bool rejected = false; // 确认失败来自登记检查，而非后续选择。
                try { operation(); } catch (const std::invalid_argument&) { rejected = true; }
                Require(rejected, "invalid catalog registration accepted");
            };
            bool emptyRejected = false; // 未注册任何规则时应明确报错。
            try { (void)catalog.Select({1, 0, 0, 0}); } catch (const std::runtime_error&) { emptyRejected = true; }
            Require(emptyRejected, "empty catalog selected a rule");
            catalog.Register(makeRule("third", {3, 0, 0, 0}, {3, 0, 0, 4}));
            catalog.Register(makeRule("first", {1, 0, 0, 0}, {1, 0, 0, 0}));
            catalog.Register(makeRule("second", {2, 0, 0, 0}, {2, 0, 0, 2}));
            Require(catalog.Select({1, 0, 0, 0}).id == "first", "first boundary selection failed");
            Require(catalog.Select({2, 0, 0, 0}).id == "second", "second boundary selection failed");
            const auto& third = catalog.Select({3, 0, 0, 0}); // 新登记的第三套规则应自动进入选择流程。
            Require(third.id == "third" && third.name == "测试规则" && !third.description.empty() &&
                third.features.Features().size() == 1, "third rule metadata or features lost");
            Require(third.GetWarning({3, 0, 0, 4}).empty(), "verified upper boundary warned");
            Require(catalog.Select({0, 9, 0, 0}).id == "first" &&
                !catalog.Select({0, 9, 0, 0}).GetWarning({0, 9, 0, 0}).empty(), "early version fallback failed");
            Require(catalog.Select({2, 5, 0, 0}).id == "second" &&
                !catalog.Select({2, 5, 0, 0}).GetWarning({2, 5, 0, 0}).empty(), "version gap fallback failed");
            Require(catalog.Select({4, 0, 0, 0}).id == "third" &&
                !catalog.Select({4, 0, 0, 0}).GetWarning({4, 0, 0, 0}).empty(), "future fallback ignored third rule");
            reject([&] { catalog.Register(makeRule("third", {4, 0, 0, 0}, {4, 0, 0, 0})); });
            reject([&] { catalog.Register(makeRule("overlap", {2, 0, 0, 1}, {2, 0, 0, 3})); });
            reject([&] { catalog.Register(makeRule("same-start", {3, 0, 0, 0}, {3, 0, 0, 0})); });
            reject([&] { catalog.Register(makeRule("enclosing", {1, 0, 0, 0}, {4, 0, 0, 0})); });
            reject([&] { catalog.Register(makeRule("reversed", {4, 0, 0, 1}, {4, 0, 0, 0})); });
            reject([&] { catalog.Register(makeRule("", {4, 0, 0, 0}, {4, 0, 0, 0})); });
            Require(catalog.Select({4, 0, 0, 0}).id == "third", "rejected registration changed catalog");
            const auto& builtIn = PatchRuleCatalog::BuiltIn(); // 现有两个版本的功能集合必须保持原样。
            Require(builtIn.Select({2, 99, 0, 0}).features.Features().size() == 5 &&
                builtIn.Select({2, 99, 0, 1}).features.Features().size() == 12, "built-in feature sets changed");
        }

        // 只在副本的元数据里修改程序集版本，原始历史文件始终只读。
        static void WriteVersion(const std::filesystem::path& source, const std::filesystem::path& target, const TargetVersion& version) {
            ManagedPatchContext context; // 当前测试副本的可编辑元数据。
            context.Open(source);
            ManagedPatchContext::ComPtr<IMetaDataAssemblyImport> reader;
            ManagedPatchContext::ComPtr<IMetaDataAssemblyEmit> writer;
            Require(SUCCEEDED(context.Metadata()->QueryInterface(IID_IMetaDataAssemblyImport, reinterpret_cast<void**>(reader.GetAddressOf()))) &&
                SUCCEEDED(context.Metadata()->QueryInterface(IID_IMetaDataAssemblyEmit, reinterpret_cast<void**>(writer.GetAddressOf()))), "assembly interfaces missing");
            mdAssembly assembly = 0; // 当前程序集清单。
            ASSEMBLYMETADATA metadata{}; // 原程序集信息，只替换四段版本。
            wchar_t name[512]{}, locale[512]{}; // 保留程序集名与区域信息。
            metadata.szLocale = locale; metadata.cbLocale = 512;
            const void* key = nullptr; ULONG keySize = 0, hashAlgorithm = 0; DWORD flags = 0; // 保留签名相关属性。
            Require(SUCCEEDED(reader->GetAssemblyFromScope(&assembly)) &&
                SUCCEEDED(reader->GetAssemblyProps(assembly, &key, &keySize, &hashAlgorithm, name, 512, nullptr, &metadata, &flags)), "version read failed");
            metadata.usMajorVersion = version.major; metadata.usMinorVersion = version.minor;
            metadata.usBuildNumber = version.build; metadata.usRevisionNumber = version.revision;
            Require(SUCCEEDED(writer->SetAssemblyProps(assembly, key, keySize, hashAlgorithm, name, &metadata, flags)), "version write failed");
            const auto type = context.ResolveType(L"PCL.PageDownloadInstall"); // 复制原方法以触发现有的元数据输出流程。
            const auto method = context.FindNamedMethod(type, L"LoaderInit");
            const auto body = context.Body(method);
            IlAssembler output; // 指令行为保持原样，所有引用继续取自当前文件。
            for (const auto& instruction : body.instructions) context.CopyInstruction(output, body, instruction);
            output.Mark(static_cast<int>(body.length));
            PatchAnalysis plan; // 测试生成过程不会调用实际下载修补规则。
            plan.methods.push_back(context.Assemble(method, body, output, PatchPointKind::DownloadPage));
            const auto bytes = context.BuildCandidate(plan);
            std::ofstream file(target, std::ios::binary);
            file.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
            Require(static_cast<bool>(file), "cannot write version fixture");
        }

        // 同一文件中的不兼容入口应跳过，其余已确认的功能仍能修补。
        static void CheckPartial(const std::filesystem::path& source, const std::filesystem::path& target, const TargetVersion& version) {
            WriteVersion(source, target, version);
            const auto analysis = ManagedPatch::Analyze(target);
            Require(analysis.version == version, "assembly version not used");
            Require(analysis.error.empty() && analysis.TotalPoints() > 0 && analysis.SkippedFeatures() > 0, "partial compatibility blocked");
            Require(PatchEngine::Apply(PeScanner::Scan(target), Console()), "compatible features not patched");
            Require(PatchRuleCatalog::BuiltIn().Select(version).features.Verify(analysis, ManagedPatch::Analyze(target)), "partial version verification failed");
        }

        // 版本超出验证范围时应继续修补，通过结构校验及最终复检才算成功。
        static void CheckAccepted(const std::filesystem::path& source, const std::filesystem::path& target,
            const TargetVersion& version, size_t points) {
            WriteVersion(source, target, version);
            const auto before = ManagedPatch::Analyze(target);
            Require(before.version == version && before.error.empty() && !before.warning.empty(), "version warning missing or blocked patch");
            Require(before.TotalPoints() == points && !before.image.empty(), "warning prevented candidate generation");
            Require(PatchEngine::Apply(PeScanner::Scan(target), Console()), "compatible structure blocked by version");
            const auto after = ManagedPatch::Analyze(target);
            Require(PatchRuleCatalog::BuiltIn().Select(version).features.Verify(before, after), "warning version verification failed");
        }

        // 仅改拦截提示和任务标题，验证识别不再依赖这些显示文字。
        static void ChangeMessages(const std::filesystem::path& source, const std::filesystem::path& target) {
            ManagedPatchContext context;
            context.Open(source);
            PatchAnalysis plan; // 只重写涉及提示的函数，其余内容保持原样。
            size_t changed = 0; // 必须覆盖任务标题和三个拦截提示，防止测试无实际修改。
            for (const auto& member : context.Symbols()) {
                if (TypeFromToken(member.token) != mdtMethodDef ||
                    (!member.owner.starts_with(L"PCL.ModModpack") && !member.owner.starts_with(L"PCL.PageDownloadInstall"))) continue;
                const auto body = context.Body(member.token);
                bool modified = false; // 每个函数只加入一份重建计划。
                IlAssembler output;
                for (const auto& instruction : body.instructions) {
                    const auto text = instruction.opcode == 0x72 ? context.UserString(instruction.token) : std::wstring{};
                    if (text == L"关闭下载" || text.find(L"MC 下载功能") != std::wstring::npos) {
                        output.Mark(static_cast<int>(instruction.offset));
                        output.Token(0x72, context.StringToken(L"服务暂不可用：完全不同的测试提示"));
                        modified = true; ++changed;
                    } else context.CopyInstruction(output, body, instruction);
                }
                if (!modified) continue;
                output.Mark(static_cast<int>(body.length));
                plan.methods.push_back(context.Assemble(member.token, body, output, PatchPointKind::DownloadPage));
            }
            Require(changed >= 4, "test did not change all restriction messages");
            const auto bytes = context.BuildCandidate(plan);
            std::ofstream file(target, std::ios::binary);
            file.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
            Require(static_cast<bool>(file), "cannot write changed-message fixture");
        }

    public:
        // 两个输入分别是真实 2.99.0.0 和 2.99.0.1，只修改新输出目录内的副本。
        static void Run(const std::filesystem::path& old_source, const std::filesystem::path& new_source, const std::filesystem::path& directory) {
            CheckCatalog();
            Require(!std::filesystem::exists(directory), "output directory already exists");
            std::filesystem::create_directories(directory);
            const auto oldHash = HashService::Sha256(old_source), newHash = HashService::Sha256(new_source);
            CheckPartial(old_source, directory / L"old-as-new.exe", {2, 99, 0, 1});
            CheckAccepted(old_source, directory / L"legacy-compatible.exe", {2, 98, 0, 0}, 5);
            CheckAccepted(new_source, directory / L"future-compatible.exe", {2, 99, 0, 2}, 12);
            const auto messages = directory / L"changed-messages.exe";
            ChangeMessages(new_source, messages);
            const auto messagePlan = ManagedPatch::Analyze(messages);
            Require(messagePlan.error.empty() && messagePlan.TotalPoints() == 12, "restriction wording affected scan");
            Require(PatchEngine::Apply(PeScanner::Scan(messages), Console()), "changed restriction wording blocked patch");
            Require(PatchRuleCatalog::BuiltIn().Select(*messagePlan.version).features.Verify(messagePlan, ManagedPatch::Analyze(messages)), "changed wording verification failed");
            CheckPartial(old_source, directory / L"future-incompatible.exe", {2, 99, 0, 2});
            const auto renamed = directory / L"PCL 9.9.9.9.exe"; // 文件名中的版本不得覆盖程序集版本。
            std::filesystem::copy_file(new_source, renamed);
            const auto renamedPlan = ManagedPatch::Analyze(renamed);
            Require(renamedPlan.version == TargetVersion{2, 99, 0, 1} && renamedPlan.error.empty() && renamedPlan.TotalPoints() == 12, "filename version affected scan");
            auto after = renamedPlan; // 即使功能结果相同，跨版本验证也必须失败。
            after.version = TargetVersion{2, 99, 0, 0};
            Require(!PatchRuleCatalog::BuiltIn().Select(*renamedPlan.version).features.Verify(renamedPlan, after), "verification ignored version change");
            after = renamedPlan;
            after.ruleId = "different-rule";
            Require(!PatchRuleCatalog::BuiltIn().Select(*renamedPlan.version).features.Verify(renamedPlan, after), "verification ignored rule change");
            Require(HashService::Sha256(old_source) == oldHash && HashService::Sha256(new_source) == newHash, "history source changed");
        }
    };
}

int wmain(int argument_count, wchar_t** arguments) {
    try {
        if (argument_count != 4) return 2;
        PclFix::VersionRegression::Run(arguments[1], arguments[2], arguments[3]);
        std::cout << "Version warnings, structural checks, and changed restriction messages PASS\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
