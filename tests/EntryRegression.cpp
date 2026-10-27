// 仅在输出目录写入合成样本，不启动真实 PCL。
#include "../ManagedPatch.h"
#include <iostream>

int wmain(int argument_count, wchar_t** arguments) {
    try {
        if (argument_count != 3) return 2;
        PclFix::PatchFeatureRegistry registry; // 单独测试七个新增入口，核心下载功能由原回归覆盖。
        registry.Register(std::make_unique<PclFix::DownloadNavigationFeature>());
        registry.Register(std::make_unique<PclFix::ModpackSearchFeature>());
        registry.Register(std::make_unique<PclFix::DisabledModDownloadFeature>());
        registry.Register(std::make_unique<PclFix::LaunchDownloadFeature>());
        registry.Register(std::make_unique<PclFix::EmptyListDownloadFeature>());
        registry.Register(std::make_unique<PclFix::UpdateDownloadFeature>());
        registry.Register(std::make_unique<PclFix::ModpackImportFeature>());
        const auto before = PclFix::ManagedPatch::Analyze(arguments[1], {}, registry);
        if (!before.error.empty()) throw std::runtime_error(before.error);
        if (before.TotalPoints() != 7 || before.image.empty()) throw std::runtime_error("expected seven entry patches");
        {
            std::ofstream output(std::filesystem::path(arguments[2]), std::ios::binary); // 只创建新的输出文件。
            output.write(reinterpret_cast<const char*>(before.image.data()), before.image.size());
            if (!output) throw std::runtime_error("cannot write fixture");
        }
        const auto after = PclFix::ManagedPatch::Analyze(arguments[2], {}, registry);
        if (!registry.Verify(before, after) || !after.image.empty()) throw std::runtime_error("entry verification failed");
        std::cout << "Entry structural verification PASS\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
