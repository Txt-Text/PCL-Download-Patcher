// 复用生产入口所在的实现，回归直接覆盖真实修补和备份流程。
#define main PclFixInteractiveMain
#include "../PCL-Download-Patcher.cpp"
#undef main

namespace PclFix {
    // 所有写入只发生在指定输出目录的副本，输入样本保持只读。
    class Regression {
    private:
        // 条件不满足时立即终止，禁止把部分通过报告为整体成功。
        static void Require(bool condition, const char* message) {
            if (!condition) throw std::runtime_error(message);
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
            Require(before.error.empty() && !before.ranges.empty(), "restriction not found");
            const auto info = PeScanner::Scan(target);
            Require(info.validPe && !info.checksumError, "source PE invalid");
            Require(BackupManager::Create(target, console), "backup failed");
            Require(HashService::Sha256(target.wstring() + L".pclfix.bak") == originalHash, "backup differs");
            Require(PatchEngine::Apply(info, console), "patch failed");
            const auto after = ManagedPatch::Analyze(target);
            Require(after.error.empty() && after.ranges.empty() && after.restoredPages >= before.restoredPages + before.ranges.size(), "patched structure invalid");
            const auto patchedHash = HashService::Sha256(target);
            Require(patchedHash != originalHash, "patch had no effect");
            Require(!PatchEngine::Apply(PeScanner::Scan(target), console), "repeat patch accepted");
            Require(HashService::Sha256(target) == patchedHash, "repeat patch mutated target");
            Require(BackupManager::Restore(target, console), "restore failed");
            Require(HashService::Sha256(target) == originalHash, "restore not byte exact");
            Require(BackupManager::Create(target, console), "identical backup reuse failed");
            Require(PatchEngine::Apply(PeScanner::Scan(target), console), "repatch failed");
            Require(HashService::Sha256(target) == patchedHash, "patch not deterministic");
            const auto damaged = output_path / L"PCL truncated.exe"; // PE 头尚不完整的损坏输入。
            {
                std::ofstream output(damaged, std::ios::binary);
                output.write("MZ", 2);
            }
            Require(!PeScanner::Scan(damaged).validPe, "truncated PE accepted");
            Require(!ManagedPatch::Analyze(damaged).error.empty(), "truncated metadata accepted");
            Require(HashService::Sha256(source_path) == originalHash, "source was modified");
            console.Line("[+] Regression PASS: " + PathToUtf8(source_path.filename()), Color::Green);
        }
    };
}

// 参数依次为输出根目录和一个或多个只读输入样本。
int wmain(int argument_count, wchar_t** arguments) {
    try {
        if (argument_count < 3) return 2;
        for (int index = 2; index < argument_count; ++index) {
            PclFix::Regression::Run(arguments[index], std::filesystem::path(arguments[1]) / std::to_wstring(index - 1));
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Regression FAIL: " << error.what() << '\n';
        return 1;
    }
}
