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
        }
    };

}
