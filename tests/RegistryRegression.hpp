#pragma once
#include "../ManagedPatch.h"

namespace PclFix {

    // 验证调用者可注册全新功能，中央流程不需要认识具体子类。
    class RegistryRegression {
    private:
        // 每次测试独立记录虚函数调用，不使用全局可变状态。
        struct Calls {
            size_t scan = 0; // 扫描入口调用数。
            size_t patch = 0; // 计划生成入口调用数。
            size_t verify = 0; // 子类验证入口调用数。
        };

        // 写入相同字节的测试特征，仅测试注册分派，不执行输出文件。
        class ProbeFeature final : public IPatchFeature {
        private:
            std::string m_id; // 本实例注册身份。
            size_t m_offset; // 本实例生成修改的位置。
            Calls& m_calls; // 借用当前测试作用域中的计数器。

        public:
            ProbeFeature(std::string id, size_t offset, Calls& calls)
                : m_id(std::move(id)), m_offset(offset), m_calls(calls) {}

            // 身份和名称都在运行时提供，不依赖新增枚举项。
            PatchFeatureInfo Describe() const override {
                return {m_id, m_id, PatchPointKind::DownloadPage, true};
            }

            // 无需 PCL 特定类型，也不依赖内置特征的扫描结果。
            FeatureScanResult Scan(ManagedPatchContext& context) const override {
                ++m_calls.scan;
                if (m_offset >= context.BytesView().size()) throw std::runtime_error("probe outside input");
                FeatureScanResult result; // 测试特征声明一个受限结构。
                result.info = Describe();
                result.state = FeatureState::Restricted;
                result.detectedCount = result.patchableCount = 1;
                return result;
            }

            // 计划由注册流程接收、归属并合并，输入文件保持只读。
            void Patch(ManagedPatchContext& context, const FeatureScanResult&, PatchAnalysis& analysis) const override {
                ++m_calls.patch;
                PatchRange range; // 同值修改用于确认事务计划分派。
                range.offset = m_offset;
                range.before = range.after = {context.BytesView()[m_offset]};
                analysis.ranges.push_back(std::move(range));
            }

            // 验证入口确实由注册表调用，而非中央功能分支代替。
            bool Verify(const PatchAnalysis& after, const FeatureScanResult&) const override {
                ++m_calls.verify;
                const auto found = std::find_if(after.features.begin(), after.features.end(),
                    [this](const auto& item) { return item.info.id == m_id; });
                return found != after.features.end() && found->state == FeatureState::Restored;
            }
        };

        // 模拟扫描失败，以及写入了部分元数据后才失败的功能。
        class FailingFeature final : public IPatchFeature {
        private:
            bool m_scanFails; // 区分扫描失败与计划生成失败。

        public:
            explicit FailingFeature(bool scan_fails) : m_scanFails(scan_fails) {}

            PatchFeatureInfo Describe() const override {
                return {m_scanFails ? "scan-failure" : "patch-failure", "失败隔离测试", PatchPointKind::DownloadPage, true};
            }

            FeatureScanResult Scan(ManagedPatchContext&) const override {
                if (m_scanFails) throw std::runtime_error("expected scan failure");
                FeatureScanResult result;
                result.info = Describe(); result.state = FeatureState::Restricted;
                result.detectedCount = result.patchableCount = 1;
                return result;
            }

            void Patch(ManagedPatchContext& context, const FeatureScanResult&, PatchAnalysis& analysis) const override {
                context.StringToken(L"FAILED_FEATURE_METADATA_MUST_NOT_LEAK");
                analysis.ranges.push_back({0, {0x4d}, {0x00}}); // 在失败前还加入一个必须丢弃的文件头修改。
                throw std::runtime_error("expected patch generation failure");
            }

            bool Verify(const PatchAnalysis&, const FeatureScanResult&) const override {
                throw std::runtime_error("skipped feature must not be verified");
            }
        };

        // 重建一个原样函数，让测试候选确实保存元数据，避免漏掉失败功能留下的字符串。
        class MetadataFeature final : public IPatchFeature {
        public:
            PatchFeatureInfo Describe() const override { return {"metadata-success", "元数据隔离测试", PatchPointKind::DownloadPage, true}; }

            FeatureScanResult Scan(ManagedPatchContext&) const override {
                FeatureScanResult result;
                result.info = Describe(); result.state = FeatureState::Restricted;
                result.detectedCount = result.patchableCount = 1;
                return result;
            }

            void Patch(ManagedPatchContext& context, const FeatureScanResult&, PatchAnalysis& analysis) const override {
                const auto type = context.ResolveType(L"PCL.PageDownloadInstall");
                const auto method = context.FindNamedMethod(type, L"LoaderInit");
                const auto body = context.Body(method);
                IlAssembler output;
                for (const auto& instruction : body.instructions) context.CopyInstruction(output, body, instruction);
                output.Mark(static_cast<int>(body.length));
                context.StringToken(L"SUCCESS_FEATURE_METADATA");
                analysis.methods.push_back(context.Assemble(method, body, output, PatchPointKind::DownloadPage));
            }

            bool Verify(const PatchAnalysis& after, const FeatureScanResult& result) const override {
                return std::any_of(after.features.begin(), after.features.end(), [&result](const auto& feature) {
                    return feature.info.id == result.info.id && feature.state == FeatureState::Restored;
                });
            }
        };

        // 所有断言失败都明确终止测试，不把部分成功报告为通过。
        static void Require(bool condition, const char* message) {
            if (!condition) throw std::runtime_error(message);
        }

    public:
        // 覆盖动态注册、重复拒绝、独立状态、验证分派与冲突计划拒绝。
        static void Run(const std::filesystem::path& source_path) {
            Calls calls; // 生命周期覆盖本轮全部注册对象。
            PatchFeatureRegistry registry; // 空注册表不自动混入内置功能。
            registry.Register(std::make_unique<ProbeFeature>("runtime-feature-a", 0, calls));
            registry.Register(std::make_unique<ProbeFeature>("runtime-feature-b", 1, calls));
            bool rejected = false; // 重复身份必须明确拒绝。
            try { registry.Register(std::make_unique<ProbeFeature>("runtime-feature-a", 2, calls)); }
            catch (const std::invalid_argument&) { rejected = true; }
            Require(rejected, "duplicate feature identity accepted");
            rejected = false;
            try { registry.Register(nullptr); }
            catch (const std::invalid_argument&) { rejected = true; }
            Require(rejected, "null feature accepted");

            const auto before = ManagedPatch::Analyze(source_path, {}, registry); // 使用自定义规则驱动真实协调器。
            Require(before.error.empty() && before.TotalPoints() == 2 && !before.image.empty(), "runtime registration not used");
            Require(calls.scan == 2 && calls.patch == 2, "runtime lifecycle not dispatched");
            Require(before.CountFeature("runtime-feature-a") == 1 && before.CountFeature("runtime-feature-b") == 1, "feature plans mixed");
            auto after = before; // 生成两个独立的正常状态，用于测试验证分派。
            after.ranges.clear();
            after.image.clear();
            for (auto& item : after.features) {
                item.state = FeatureState::Restored;
                item.patchableCount = 0;
                item.restoredCount = 1;
            }
            Require(registry.Verify(before, after) && calls.verify == 2, "runtime verification not dispatched");

            PatchFeatureRegistry conflict; // 两个合法身份的功能仍不能覆盖同一字节。
            conflict.Register(std::make_unique<ProbeFeature>("conflict-a", 0, calls));
            conflict.Register(std::make_unique<ProbeFeature>("conflict-b", 0, calls));
            const auto invalid = ManagedPatch::Analyze(source_path, {}, conflict); // 冲突必须在文件写入前发现。
            Require(!invalid.error.empty() && invalid.image.empty() && !invalid.TotalPoints(), "overlapping plans accepted");

            PatchFeatureRegistry isolated; // 故意让正常功能先生成，再触发失败，验证重建与重放。
            isolated.Register(std::make_unique<MetadataFeature>());
            isolated.Register(std::make_unique<FailingFeature>(true));
            isolated.Register(std::make_unique<FailingFeature>(false));
            const auto partial = ManagedPatch::Analyze(source_path, {}, isolated);
            Require(partial.error.empty() && partial.TotalPoints() == 1 && partial.SkippedFeatures() == 2 && !partial.image.empty(), "feature failures blocked valid plan");
            const std::wstring sentinel = L"FAILED_FEATURE_METADATA_MUST_NOT_LEAK";
            const auto* sentinelBytes = reinterpret_cast<const unsigned char*>(sentinel.data());
            Require(std::search(partial.image.begin(), partial.image.end(), sentinelBytes, sentinelBytes + sentinel.size() * sizeof(wchar_t)) == partial.image.end(), "failed metadata leaked into output");
            Require(partial.image[0] == 0x4d && partial.image[1] == 0x5a, "failed byte patch leaked into output");
            auto partialAfter = partial;
            partialAfter.methods.clear(); partialAfter.image.clear();
            for (auto& feature : partialAfter.features) {
                if (feature.info.id != "metadata-success") continue;
                feature.state = FeatureState::Restored; feature.restoredCount = 1; feature.patchableCount = 0;
            }
            Require(isolated.Verify(partial, partialAfter), "skipped feature blocked verification");
            partialAfter.features.front().state = FeatureState::Unsupported;
            Require(!isolated.Verify(partial, partialAfter), "modified feature verification was skipped");

            PatchFeatureRegistry allSkipped; // 全部失败时不能生成空操作候选，也不能把跳过当成恢复。
            allSkipped.Register(std::make_unique<FailingFeature>(true));
            allSkipped.Register(std::make_unique<FailingFeature>(false));
            const auto noChanges = ManagedPatch::Analyze(source_path, {}, allSkipped);
            Require(noChanges.error.empty() && noChanges.SkippedFeatures() == 2 && noChanges.TotalPoints() == 0 && noChanges.image.empty(), "all-skipped scan generated output");
        }
    };

}
