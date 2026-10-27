#pragma once
#include <windows.h>
#include <cor.h>
#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

namespace PclFix {

    // 可独立扫描、写入和回显的功能类别；新增限制点时只需增加类别和对应分析规则。
    enum class PatchPointKind {
        DownloadPage, // Minecraft 下载页面的正常加载器绑定。
        ModpackDownload, // 整合包安装入口。
        ModifyPage, // 修改已有实例页面的 Minecraft 卡片。
        ResourceInstall // 资源详情列表的安装入口。
    };

    // 将类别转换为用户可读名称，统一用于扫描摘要和修补进度。
    inline const char* PatchPointName(PatchPointKind kind) {
        switch (kind) {
        case PatchPointKind::DownloadPage: return "下载页面";
        case PatchPointKind::ModpackDownload: return "整合包下载";
        case PatchPointKind::ModifyPage: return "修改页面的 Minecraft 选项";
        case PatchPointKind::ResourceInstall: return "资源安装入口";
        default: return "未知修补点";
        }
    }

    // 一个注册特征的当前结构状态，用于区分未发现、受限、已恢复和不兼容。
    enum class FeatureState {
        NotFound,
        Restricted,
        Restored,
        Unsupported
    };

    // 修补点的静态描述；具体特征类通过该描述参与统一回显和分类统计。
    struct PatchFeatureInfo {
        std::string id; // 供补丁范围与验证结果关联的稳定名称。
        std::string name = "未知修补点"; // 面向用户的名称由描述结构拥有，支持运行时注册。
        PatchPointKind category = PatchPointKind::DownloadPage; // 所属功能类别。
        bool required = true; // 是否属于完整恢复所必须的功能。
    };

    // 一次扫描得到的具体特征结果；不会保存跨文件的可变状态。
    struct FeatureScanResult {
        PatchFeatureInfo info; // 本结果对应的特征描述。
        FeatureState state = FeatureState::NotFound; // 当前特征的结构状态。
        size_t detectedCount = 0; // 识别到的结构数量。
        size_t patchableCount = 0; // 可以生成补丁的结构数量。
        size_t restoredCount = 0; // 已经处于正常状态的结构数量。
        std::string reason; // 不支持或拒绝修补时的原因。
        std::vector<mdTypeDef> typeDefinitions; // 本特征扫描时匹配到的目标类型令牌。
    };

    // 一段等长 IL 替换，保存原始字节以防分析后目标文件改变。
    struct PatchRange {
        size_t offset = 0; // 文件内的实际偏移，每次从 RVA 动态计算。
        std::vector<unsigned char> before; // 替换前字节。
        std::vector<unsigned char> after; // 替换后字节。
        PatchPointKind kind = PatchPointKind::DownloadPage; // 该字节范围所属的功能类别。
        std::string featureId; // 产生该修改的具体注册特征。
    };

    // 不能等长恢复的方法使用新方法体，原 MethodDef 令牌保持不变。
    struct MethodReplacement {
        mdMethodDef method = 0; // 本次扫描动态解析到的方法定义。
        std::vector<unsigned char> body; // 包括 fat 方法头、IL 和异常处理表。
        PatchPointKind kind = PatchPointKind::ModpackDownload; // 分类回显和统计归属。
        std::string featureId; // 产生该修改的具体注册特征。
    };

    // 解析结果同时用于界面显示、事务写入和修补后验证。
    struct PatchAnalysis {
        std::vector<PatchRange> ranges; // 各功能类别中已经验证的等长修改。
        std::vector<MethodReplacement> methods; // 需要迁移并重建的方法。
        size_t restoredCount = 0; // 特征局部分析时记录已经正常的结构数。
        std::vector<unsigned char> image; // 完整修补计划生成的候选 PE；不会直接改写输入文件。
        std::string error; // 结构不符时的具体原因。
        std::string warning; // 可继续修补但存在未恢复功能时的提示。
        std::vector<FeatureScanResult> features; // 每个已注册修补点的独立扫描结果。

        // 按注册顺序列出本次出现的类别，控制台不再维护固定功能列表。
        std::vector<PatchPointKind> Categories() const {
            std::vector<PatchPointKind> categories; // 首次出现的类别。
            for (const auto& feature : features) {
                if (std::find(categories.begin(), categories.end(), feature.info.category) == categories.end()) {
                    categories.push_back(feature.info.category);
                }
            }
            return categories;
        }

        // 统计一个类别中实际可以写入的范围数量。
        size_t Count(PatchPointKind kind) const {
            return static_cast<size_t>(std::count_if(ranges.begin(), ranges.end(), [kind](const auto& range) { return range.kind == kind; })) +
                static_cast<size_t>(std::count_if(methods.begin(), methods.end(), [kind](const auto& method) { return method.kind == kind; }));
        }

        // 统计某个具体注册特征生成的修改点，区分同一类别中的多个功能。
        size_t CountFeature(std::string_view id) const {
            return static_cast<size_t>(std::count_if(ranges.begin(), ranges.end(), [id](const auto& range) { return range.featureId == id; })) +
                static_cast<size_t>(std::count_if(methods.begin(), methods.end(), [id](const auto& method) { return method.featureId == id; }));
        }

        // 汇总所有已注册特征中已正常的结构数量。
        size_t RestoredPoints() const {
            size_t count = 0; // 所有特征的正常结构总数。
            for (const auto& feature : features) count += feature.restoredCount;
            return count;
        }

        // 仅统计已生成且能够完整写入的修补点，不能恢复的结构会产生错误。
        size_t TotalPoints() const { return ranges.size() + methods.size(); }
    };

}
