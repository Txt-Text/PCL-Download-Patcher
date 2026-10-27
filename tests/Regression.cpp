// 复用生产入口所在的实现，回归直接覆盖真实修补和备份流程。
#define main PclFixInteractiveMain
#include "../PCL-Download-Patcher.cpp"
#undef main
#include "RegistryRegression.hpp"

namespace PclFix {
    // 所有写入只发生在指定输出目录的副本，输入样本保持只读。
    class Regression {
    private:
        // 条件不满足时立即终止，禁止把部分通过报告为整体成功。
        static void Require(bool condition, const char* message) {
            if (!condition) throw std::runtime_error(message);
        }

        // 以整块读取避免大文件逐字节迭代；测试只会写入输出目录。
        static std::vector<unsigned char> ReadBytes(const std::filesystem::path& path) {
            std::ifstream input(path, std::ios::binary | std::ios::ate);
            Require(input.good(), "test input unreadable");
            const auto size = input.tellg();
            Require(size > 0, "test input empty");
            std::vector<unsigned char> bytes(static_cast<size_t>(size));
            input.seekg(0);
            Require(static_cast<bool>(input.read(reinterpret_cast<char*>(bytes.data()), size)), "test input truncated");
            return bytes;
        }

        // 写出合成的旧补丁或不兼容副本，始终保留真实输入。
        static void WriteBytes(const std::filesystem::path& path, const std::vector<unsigned char>& bytes) {
            std::ofstream output(path, std::ios::binary);
            output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
            Require(output.good(), "test output failed");
        }

        // 元数据或 IL 被修改后不保留过期的可选 PE 校验和。
        static void ClearChecksum(std::vector<unsigned char>& bytes) {
            IMAGE_DOS_HEADER dos{}; std::memcpy(&dos, bytes.data(), sizeof(dos));
            const size_t offset = dos.e_lfanew + 4 + sizeof(IMAGE_FILE_HEADER) + offsetof(IMAGE_OPTIONAL_HEADER32, CheckSum);
            Require(offset + 4 <= bytes.size(), "test PE header invalid");
            std::fill_n(bytes.begin() + offset, 4, 0);
        }

        // 缺少一类安装器时必须拒绝整个修补，不能写入其他已经生成的功能点。
        static void CheckUnsupported(const std::filesystem::path& source, const std::filesystem::path& directory, const Console& console) {
            auto bytes = ReadBytes(source);
            constexpr std::string_view name = "InstallPackModrinth";
            const auto found = std::search(bytes.begin(), bytes.end(), name.begin(), name.end());
            Require(found != bytes.end(), "installer metadata not found");
            *(found + name.size() - 1) = 'X';
            ClearChecksum(bytes);
            const auto broken = directory / L"unsupported.exe";
            WriteBytes(broken, bytes);
            const auto info = PeScanner::Scan(broken);
            const auto plan = ManagedPatch::Analyze(broken);
            Require(!plan.error.empty() && !plan.TotalPoints() && plan.image.empty(), "incomplete recovery accepted");
            Require(!PatchEngine::Apply(info, console) && HashService::Sha256(broken) == info.hash, "unsupported input changed");
            Require(!std::filesystem::exists(broken.wstring() + L".pclfix.tmp"), "unsupported input created temporary output");
        }

        // 同类中的整合包入口必须分别验证；调换结果顺序也不应影响身份匹配。
        static void CheckFeatureVerification(const PatchAnalysis& before, const PatchAnalysis& after) {
            const auto& registry = PatchFeatureRegistry::BuiltIn(); // 生产启动时使用的规则集合。
            auto reordered = after; // 模拟汇总结果次序改变。
            std::reverse(reordered.features.begin(), reordered.features.end());
            Require(registry.Verify(before, reordered), "verification depends on result order");
            for (const auto& original : after.features) {
                auto damaged = after; // 每次只破坏一个具体特征的复检结果。
                const auto found = std::find_if(damaged.features.begin(), damaged.features.end(),
                    [&original](const auto& item) { return item.info.id == original.info.id; });
                found->state = FeatureState::NotFound;
                found->restoredCount = 0;
                Require(!registry.Verify(before, damaged), "missing feature hidden by category totals");
            }
        }

        // 删除必要结构时仍完成其他特征扫描，但必须拒绝生成任何候选写入。
        static void CheckPartialRecognition(const std::filesystem::path& source, const std::filesystem::path& directory) {
            auto bytes = ReadBytes(source); // 从输入复制，原样本保持只读。
            constexpr std::string_view name = "get_CardMinecraft"; // 破坏卡片访问器的语义身份。
            const auto found = std::search(bytes.begin(), bytes.end(), name.begin(), name.end());
            Require(found != bytes.end(), "card getter metadata not found");
            *(found + name.size() - 1) = 'X';
            ClearChecksum(bytes);
            const auto target = directory / L"partial-feature.exe"; // 隔离的缺失结构样本。
            WriteBytes(target, bytes);
            const auto hash = HashService::Sha256(target); // 检查拒绝操作前后的内容。
            const auto plan = ManagedPatch::Analyze(target); // 必须保留所有已注册项的扫描记录。
            Require(!plan.error.empty() && plan.TotalPoints() == 0 && plan.image.empty(), "partial features patched");
            Require(plan.features.size() == PatchFeatureRegistry::BuiltIn().Features().size(), "partial scan stopped early");
            Require(HashService::Sha256(target) == hash, "partial scan mutated input");
        }

        // 模拟只修补下载页面的旧版本，并验证原始备份保留、附加备份和整体升级。
        static void CheckUpgrade(const std::filesystem::path& source, const std::filesystem::path& directory, const PatchAnalysis& plan, const Console& console) {
            if (!plan.Count(PatchPointKind::DownloadPage)) return;
            auto bytes = ReadBytes(source);
            for (const auto& range : plan.ranges) if (range.kind == PatchPointKind::DownloadPage) std::copy(range.after.begin(), range.after.end(), bytes.begin() + range.offset);
            ClearChecksum(bytes);
            const auto target = directory / L"upgrade.exe";
            WriteBytes(target, bytes);
            const auto originalHash = HashService::Sha256(source);
            const auto currentHash = HashService::Sha256(target);
            const auto originalBackup = target.wstring() + L".pclfix.bak";
            const auto extraBackup = target.wstring() + L".pclfix." + std::wstring(currentHash.begin(), currentHash.begin() + 16) + L".bak";
            std::filesystem::copy_file(source, originalBackup);
            Require(BackupManager::Create(target, console) && BackupManager::Create(target, console), "upgrade backup failed");
            Require(HashService::Sha256(originalBackup) == originalHash && HashService::Sha256(extraBackup) == currentHash, "upgrade overwrote a backup");
            Require(PatchEngine::Apply(PeScanner::Scan(target), console), "partial patch upgrade failed");
            Require(ManagedPatch::Analyze(target).TotalPoints() == 0, "partial patch upgrade incomplete");
            Require(BackupManager::Restore(target, console) && HashService::Sha256(target) == originalHash, "upgrade did not preserve original restore");
        }

    public:
        // 对每个样本执行真实修补、重复调用、还原和损坏文件拒绝测试。
        static void Run(const std::filesystem::path& source_path, const std::filesystem::path& output_path) {
            const Console console; // 生产日志通道。
            const auto originalHash = HashService::Sha256(source_path); // 原始输入的内容标识。
            Require(originalHash.size() == 64, "source hash failed");
            std::filesystem::create_directories(output_path);
            const auto target = output_path / L"PCL test.exe"; // 每个样本的隔离工作副本。
            std::filesystem::copy_file(source_path, target);
            const auto before = ManagedPatch::Analyze(target); // 从未修改副本得到动态补丁范围。
            if (!before.error.empty()) throw std::runtime_error(before.error);
            if (!before.TotalPoints()) {
                Require(before.RestoredPoints() > 0 && before.image.empty(), "unrecognized unrestricted input");
                Require(!PatchEngine::Apply(PeScanner::Scan(target), console), "unrestricted input patched");
                Require(HashService::Sha256(target) == originalHash && HashService::Sha256(source_path) == originalHash, "unrestricted input changed");
                console.Line("Unrestricted PASS: " + PathToUtf8(source_path.filename()), Console::Level::Success);
                return;
            }
            Require(!before.image.empty(), "candidate image missing");
            const auto info = PeScanner::Scan(target);
            Require(info.validPe && !info.checksumError, "source PE invalid");
            Require(BackupManager::Create(target, console), "backup failed");
            Require(HashService::Sha256(target.wstring() + L".pclfix.bak") == originalHash, "backup differs");
            Require(PatchEngine::Apply(info, console), "patch failed");
            const auto after = ManagedPatch::Analyze(target);
            Require(PatchFeatureRegistry::BuiltIn().Verify(before, after), "patched structure invalid");
            CheckFeatureVerification(before, after);
            const auto patchedHash = HashService::Sha256(target);
            Require(patchedHash != originalHash, "patch had no effect");
            Require(!PatchEngine::Apply(PeScanner::Scan(target), console), "repeat patch accepted");
            Require(HashService::Sha256(target) == patchedHash, "repeat patch mutated target");
            Require(BackupManager::Restore(target, console), "restore failed");
            Require(HashService::Sha256(target) == originalHash, "restore not byte exact");
            Require(BackupManager::Create(target, console), "identical backup reuse failed");
            Require(PatchEngine::Apply(PeScanner::Scan(target), console), "repatch failed");
            Require(HashService::Sha256(target) == patchedHash, "patch not deterministic");
            if (before.Count(PatchPointKind::ModpackDownload)) {
                CheckUnsupported(source_path, output_path, console);
                CheckPartialRecognition(source_path, output_path);
                CheckUpgrade(source_path, output_path, before, console);
            }
            const auto damaged = output_path / L"PCL truncated.exe"; // PE 头尚不完整的损坏输入。
            {
                std::ofstream output(damaged, std::ios::binary);
                output.write("MZ", 2);
            }
            Require(!PeScanner::Scan(damaged).validPe, "truncated PE accepted");
            Require(!ManagedPatch::Analyze(damaged).error.empty(), "truncated metadata accepted");
            Require(HashService::Sha256(source_path) == originalHash, "source was modified");
            console.Line("Regression PASS: " + PathToUtf8(source_path.filename()), Console::Level::Success);
        }
    };
}

// 参数依次为输出根目录和一个或多个只读输入样本。
int wmain(int argument_count, wchar_t** arguments) {
    try {
        if (argument_count < 3) return 2;
        PclFix::RegistryRegression::Run(arguments[2]);
        for (int index = 2; index < argument_count; ++index) {
            PclFix::Regression::Run(arguments[index], std::filesystem::path(arguments[1]) / std::to_wstring(index - 1));
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Regression FAIL: " << error.what() << '\n';
        return 1;
    }
}
