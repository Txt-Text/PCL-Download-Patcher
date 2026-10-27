#define NOMINMAX
#include <windows.h>
#include <tlhelp32.h>
#include <commdlg.h>
#include <conio.h>
#include <bcrypt.h>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>
#include <algorithm>
#include <iterator>
#include <cstring>
#include "ManagedPatch.h"

#pragma comment(lib, "Comdlg32.lib")
#pragma comment(lib, "Bcrypt.lib")

#define PCL_DOWNLOAD_PATCHER_VERSION "v1.1.0"
#define PCL_DOWNLOAD_PATCHER_AUTHOR "Txt_Text"

namespace {
    namespace Fs = std::filesystem;

    // 将 Windows 宽字符路径转换为 UTF-8，供控制台消息显示。
    std::string PathToUtf8(const Fs::path& path) {
        const std::wstring value = path.wstring();
        const int length = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
        std::string result(static_cast<size_t>(length), '\0');
        if (length > 0) WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), length, nullptr, nullptr);
        return result;
    }

    class Console {
    private:
        HANDLE m_handle;                // 借用标准输出句柄，不负责关闭。
        WORD m_defaultColor;            // 控制台原来的文字颜色。
        DWORD m_originalMode = 0;       // 退出时恢复的原控制台模式。
        bool m_virtualTerminal = false; // 是否支持 RGB 橙色转义序列。

    public:
        Console() : m_handle(GetStdHandle(STD_OUTPUT_HANDLE)), m_defaultColor(7) {
            CONSOLE_SCREEN_BUFFER_INFO info{};
            if (GetConsoleScreenBufferInfo(m_handle, &info)) m_defaultColor = info.wAttributes;
            if (GetConsoleMode(m_handle, &m_originalMode)) m_virtualTerminal = SetConsoleMode(m_handle, m_originalMode | ENABLE_VIRTUAL_TERMINAL_PROCESSING) != FALSE;
        }

        // 只恢复本对象改变的控制台模式，标准输出句柄仍由进程持有。
        ~Console() { if (m_virtualTerminal) SetConsoleMode(m_handle, m_originalMode); }
        Console(const Console&) = delete;
        Console& operator=(const Console&) = delete;

        // 颜色类型
        enum class Color : WORD {
            None = 0xffff,   // 无颜色，用于控制台打印函数判断是否要使用默认颜色
            Gray = FOREGROUND_INTENSITY, // 调试消息使用灰色。
            White = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE,
            Green = FOREGROUND_GREEN | FOREGROUND_INTENSITY,
            Red = FOREGROUND_RED | FOREGROUND_INTENSITY,
            Yellow = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_INTENSITY,
            Orange = FOREGROUND_RED | FOREGROUND_GREEN
        };

        // 日志等级
        enum class Level {
            None,    // 纯消息，无前缀
            Info,    // 普通消息，白色 [=]
            Warn,    // 警告信息，黄色 [!]
            Error,   // 错误信息，红色 [-]
            Success, // 成功信息，绿色 [+]
            Debug    // 调试信息，灰色 [#]
        };

        // 获取日志等级对应的颜色
        static Color GetLevelColor(Level level) {
            switch (level) {
            case Level::None:
                return Color::White;
            case Level::Info:
                return Color::White;
            case Level::Warn:
                return Color::Yellow;
            case Level::Error:
                return Color::Red;
            case Level::Success:
                return Color::Green;
            case Level::Debug:
                return Color::Gray;
            }
            return Color::White; // 未知等级按普通消息显示。
        }

        // 获取日志等级对应的前缀字符串
        static std::string GetLevelPrefix(Level level) {
            switch (level) {
            case Level::None:
                return "";
            case Level::Info:
                return "[=]";
            case Level::Warn:
                return "[!]";
            case Level::Error:
                return "[-]";
            case Level::Success:
                return "[+]";
            case Level::Debug:
                return "[#]";
            }
            return ""; // 未知等级不添加前缀。
        }

        // 不换行的打印函数
        void Write(const std::string& text, Level level = Level::None, Color color = Color::None) const {

            // 显式颜色优先；未指定颜色时使用日志等级的默认颜色。
            const Color effectiveColor = color == Color::None ? GetLevelColor(level) : color;
            const std::string prefix = GetLevelPrefix(level); // 无等级消息不加前缀及空格。
            const std::string message = prefix.empty() ? text : prefix + " " + text;
            SetConsoleTextAttribute(m_handle, static_cast<WORD>(effectiveColor));
            // 控制台使用 UTF-8 代码页；重定向时同样原样写入 UTF-8 字节。
            // 仅真实控制台启用橙色转义序列，避免污染重定向文件。
            const std::string output = effectiveColor == Color::Orange && m_virtualTerminal
                ? "\x1b[38;2;255;165;0m" + message + "\x1b[0m" : message;
            DWORD written = 0; // 本次写入的字节数。
            WriteFile(m_handle, output.data(), static_cast<DWORD>(output.size()), &written, nullptr);
            SetConsoleTextAttribute(m_handle, m_defaultColor);
        }

        // 换行的打印函数
        void Line(const std::string& text, Level level = Level::Info, Color color = Color::None) const {
            Write(text + "\n", level, color);
        }

        // 读取按键
        int ReadKey() const {
            const int key = _getch();
            return key == 0 || key == 224 ? 256 + _getch() : key;
        }
    };

    struct FileInfo {
        Fs::path path; // 被检查的目标文件路径。
        std::string hash; // 检查时计算的 SHA-256。
        bool validPe = false; // DOS 与 PE 头结构是否有效。
        bool checksumError = false; // 可选 PE 校验和是否不匹配。
    };

    // 哈希工具
    class HashService {
    public:
        static std::string Sha256(const Fs::path& path) {
            std::ifstream input(path, std::ios::binary);
            BCRYPT_ALG_HANDLE algorithm = nullptr;
            BCRYPT_HASH_HANDLE hash = nullptr;
            DWORD objectLength = 0, resultLength = 0;
            if (!input || BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) return "无法计算";
            if (BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&objectLength), sizeof(objectLength), &resultLength, 0) < 0) {
                BCryptCloseAlgorithmProvider(algorithm, 0);
                return "无法计算";
            }
            std::vector<UCHAR> object(objectLength), digest(32), buffer(65536);
            if (BCryptCreateHash(algorithm, &hash, object.data(), objectLength, nullptr, 0, 0) < 0) {
                BCryptCloseAlgorithmProvider(algorithm, 0);
                return "无法计算";
            }
            while (input) {
                input.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
                if (input.gcount() > 0 && BCryptHashData(hash, buffer.data(), static_cast<ULONG>(input.gcount()), 0) < 0) {
                    BCryptDestroyHash(hash); BCryptCloseAlgorithmProvider(algorithm, 0);
                    return "无法计算";
                }
            }
            if (BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) < 0) {
                BCryptDestroyHash(hash); BCryptCloseAlgorithmProvider(algorithm, 0);
                return "无法计算";
            }
            BCryptDestroyHash(hash); BCryptCloseAlgorithmProvider(algorithm, 0);
            std::ostringstream output; output << std::hex << std::setfill('0');
            for (const auto byte : digest) output << std::setw(2) << static_cast<int>(byte);
            return output.str();
        }
    };

    class PeScanner {
    public:
        static FileInfo Scan(const Fs::path& path) {
            std::ifstream input(path, std::ios::binary);
            FileInfo info;
            info.path = path;
            info.hash = HashService::Sha256(path);
            if (!input) return info;
            const std::vector<char> bytes((std::istreambuf_iterator<char>(input)), {});
            if (bytes.size() < sizeof(IMAGE_DOS_HEADER) || bytes[0] != 'M' || bytes[1] != 'Z') return info;
            IMAGE_DOS_HEADER dos{};
            std::memcpy(&dos, bytes.data(), sizeof(dos));
            const size_t peOffset = static_cast<size_t>(dos.e_lfanew);
            if (dos.e_lfanew <= 0 || peOffset > bytes.size() || bytes.size() - peOffset < sizeof(DWORD) + sizeof(IMAGE_FILE_HEADER)) return info;
            DWORD signature = 0;
            std::memcpy(&signature, bytes.data() + peOffset, sizeof(signature));
            if (signature != IMAGE_NT_SIGNATURE) return info;
            IMAGE_FILE_HEADER fileHeader{};
            std::memcpy(&fileHeader, bytes.data() + peOffset + sizeof(DWORD), sizeof(fileHeader));
            const size_t optionalOffset = peOffset + sizeof(DWORD) + sizeof(fileHeader);
            if (fileHeader.SizeOfOptionalHeader < sizeof(WORD) || optionalOffset + fileHeader.SizeOfOptionalHeader > bytes.size()) return info;
            WORD magic = 0;
            std::memcpy(&magic, bytes.data() + optionalOffset, sizeof(magic));
            if (magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC && magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) return info;
            constexpr size_t checksumOffsetInOptionalHeader = offsetof(IMAGE_OPTIONAL_HEADER32, CheckSum);
            if (fileHeader.SizeOfOptionalHeader < checksumOffsetInOptionalHeader + sizeof(DWORD)) return info;
            DWORD storedChecksum = 0;
            std::memcpy(&storedChecksum, bytes.data() + optionalOffset + checksumOffsetInOptionalHeader, sizeof(storedChecksum));
            info.validPe = true;
            if (storedChecksum != 0) {
                std::uint64_t sum = 0;
                const size_t checksumFileOffset = optionalOffset + checksumOffsetInOptionalHeader;
                for (size_t index = 0; index < bytes.size(); index += 2) {
                    WORD word = 0;
                    if (index != checksumFileOffset && index != checksumFileOffset + 2) {
                        word = static_cast<unsigned char>(bytes[index]);
                        if (index + 1 < bytes.size()) word |= static_cast<WORD>(static_cast<unsigned char>(bytes[index + 1])) << 8;
                    }
                    sum = (sum & 0xffff) + (sum >> 16) + word;
                }
                sum = (sum & 0xffff) + (sum >> 16);
                sum += bytes.size();
                info.checksumError = storedChecksum != static_cast<DWORD>(sum);
            }
            return info;
        }
    };

    class PatchEngine {
    private:

        // 每类功能均需从待修补转为可验证的正常结构，不能用“没有字节补丁”代替功能完成。
        static bool Verified(const PclFix::PatchAnalysis& before, const PclFix::PatchAnalysis& after) {
            return before.version && before.version == after.version && PclFix::PatchRuleCatalog::BuiltIn().Select(*before.version).features.Verify(before, after);
        }

    public:

        // 是否支持修补
        static bool CanPatch(const FileInfo& info, const Console& console) {

            // 合并同类别下连续注册的具体特征进度。
            std::optional<PclFix::PatchPointKind> scanning; 
            const auto analysis = PclFix::ManagedPatch::Analyze(info.path, [&console, &scanning](PclFix::PatchPointKind kind) {
                if (scanning != kind) console.Line("扫描 " + std::string(PclFix::PatchPointName(kind)) + "...", Console::Level::Info);
                scanning = kind;
            });
            if (analysis.version) console.Line("PCL 程序集版本：" + analysis.version->ToString() + "，检查规则：" + analysis.ruleName, Console::Level::Info);
            if (!analysis.warning.empty()) console.Line(analysis.warning, Console::Level::Warn);
            console.Line("", Console::Level::None); // 扫描进度与分类结果之间留一行。
            size_t recognized = 0; // 识别到结构或明确的不兼容点时，不能声称完全未发现。
            for (const auto category : analysis.Categories()) {
                console.Line(std::string(PclFix::PatchPointName(category)) + "：", Console::Level::Info, Console::Color::Orange);
                for (const auto& feature : analysis.features) {
                    if (feature.info.category != category) continue;
                    const char* state = "已跳过（未识别）";  // 未匹配。
                    Console::Level level = Console::Level::Warn; // 跳过属于警告，不阻止其他功能修补。
                    Console::Color stateColor = Console::Color::Yellow; // 状态文字独立着色，功能名称保持等级默认颜色。
                    if (feature.state != PclFix::FeatureState::NotFound) ++recognized;
                    if (feature.state == PclFix::FeatureState::Restricted) {
                        state = "需要修补"; level = Console::Level::Info; stateColor = Console::Color::Yellow;
                    } else if (feature.state == PclFix::FeatureState::Restored) {
                        state = "无需修补"; level = Console::Level::Info; stateColor = Console::Color::Green;
                    } else if (feature.state == PclFix::FeatureState::Unsupported) state = "已跳过（结构不支持）";
                    console.Write("    ", Console::Level::None); // 缩进只用于区分类别和具体功能。
                    console.Write(feature.info.name + "：", level);
                    console.Line(state, Console::Level::None, stateColor);
                    if (!feature.reason.empty()) {
                        console.Write("        ", Console::Level::None);
                        console.Line(feature.reason, Console::Level::Warn);
                    }
                }
            }
            console.Line("", Console::Level::None); // 分类结果与扫描总结或报错之间留一行。
            if (!recognized) {
                console.Line("未识别到受支持的特征，可能目标是已修补版本、社区版或其他改版，也可能是尚未支持的版本。", Console::Level::Error);
                if (!analysis.error.empty() && analysis.error != "未识别到受支持的特征。") console.Line(analysis.error, Console::Level::Error);
                return false;
            }
            if (!analysis.error.empty()) { console.Line(analysis.error, Console::Level::Error); return false; }
            if (!analysis.TotalPoints()) {
                if (analysis.SkippedFeatures()) {
                    console.Line("没有可确定需要修补的功能；未识别或不支持的功能已跳过。", Console::Level::Info);
                    return false;
                }
                console.Line("目标下载功能全部正常，无需重复修补。", Console::Level::Success);
                return false;
            }
            console.Line("扫描完毕。总共有 " + std::to_string(analysis.TotalPoints()) + " 处需要修补：", Console::Level::Info);
            for (const auto kind : analysis.Categories()) {
                const size_t count = analysis.Count(kind);
                if (count) console.Line(std::string(PclFix::PatchPointName(kind)) + "：" + std::to_string(count) + " 个", Console::Level::Info);
            }

            return true;
        }

        // 分类报告已生成的计划，本次实际修改的功能验证成功后才替换目标。
        static bool Apply(const FileInfo& info, const Console& console) {
            const auto analysis = PclFix::ManagedPatch::Analyze(info.path);
            if (!analysis.error.empty() || !analysis.TotalPoints()) {
                console.Line(!analysis.error.empty() ? analysis.error : (analysis.SkippedFeatures() ? "没有可确定修补的功能，未修改文件。" : "无需重复修补。"), analysis.error.empty() ? Console::Level::Info : Console::Level::Error);
                return false;
            }
            const auto& bytes = analysis.image; // 分析器同时生成等长修改和完整方法恢复的候选镜像。
            const auto categories = analysis.Categories(); // 类别来自实际注册特征。
            for (const auto kind : categories) {
                if (!analysis.Count(kind)) continue;
                console.Line("正在修补 " + std::string(PclFix::PatchPointName(kind)), Console::Level::Info);
                console.Line("已生成 " + std::to_string(analysis.Count(kind)) + " 处修改，等待应用修改与校验。", Console::Level::Info);
            }
            if (bytes.empty() || HashService::Sha256(info.path) != info.hash) { 
                console.Line("发现源文件发生变化，停止修补。", Console::Level::Error); return false; 
            }

            const Fs::path temporary = info.path.wstring() + L".pclfix.tmp";
            // CREATE_NEW 防止覆盖已有临时文件。
            const HANDLE output = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (output == INVALID_HANDLE_VALUE) return false;
            DWORD written = 0;
            const bool saved = WriteFile(output, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) && written == bytes.size() && FlushFileBuffers(output);
            CloseHandle(output);
            const auto verified = PclFix::ManagedPatch::Analyze(temporary);
            bool success = saved && Verified(analysis, verified) &&
                PeScanner::Scan(temporary).validPe && HashService::Sha256(info.path) == info.hash;
            if (success) success = ReplaceFileW(info.path.c_str(), temporary.c_str(), nullptr, 0, nullptr, nullptr) != FALSE;
            if (!success) { std::error_code error; Fs::remove(temporary, error); console.Line("修补或临时文件验证失败，目标未替换。", Console::Level::Error); if (!verified.error.empty()) console.Line(verified.error, Console::Level::Error); return false; }
            const auto finalCheck = PclFix::ManagedPatch::Analyze(info.path);
            success = Verified(analysis, finalCheck);
            if (success) {
                for (const auto kind : categories) if (analysis.Count(kind)) console.Line(std::string(PclFix::PatchPointName(kind)) + "：本次修补完成，验证通过。", Console::Level::Success);
                if (analysis.SkippedFeatures()) console.Line("本次可修补的功能已完成，另有 " + std::to_string(analysis.SkippedFeatures()) + " 项功能跳过，无法确认全部下载功能均已恢复。", Console::Level::Warn);
                else console.Line("修补完成。", Console::Level::Success);
                if (!finalCheck.warning.empty()) console.Line(finalCheck.warning, Console::Level::Warn);
            } else console.Line("替换后验证失败，请使用备份还原。", Console::Level::Error);
            return success;
        }
    };

    // 备份
    class BackupManager {
    public:
        static bool Create(const Fs::path& target, const Console& console) {
            Fs::path backup = target;
            backup += ".pclfix.bak";
            std::error_code error;
            if (Fs::exists(backup, error)) {
                const auto existingHash = HashService::Sha256(backup);
                if (existingHash.size() == 64 && existingHash == HashService::Sha256(target)) {
                    console.Line("已有相同文件哈希的备份，无需重复创建。", Console::Level::Success);
                    return true;
                }
                const auto currentHash = HashService::Sha256(target); // 升级旧补丁时保留最初备份，并另存本次修改前的状态。
                if (currentHash.size() != 64) { console.Line("无法校验当前文件，停止备份。", Console::Level::Error); return false; }
                backup = target.wstring() + L".pclfix." + std::wstring(currentHash.begin(), currentHash.begin() + 16) + L".bak";
                if (Fs::exists(backup, error)) {
                    if (HashService::Sha256(backup) == currentHash) { console.Line("已保留最初备份，也已有当前文件的备份，无需重复创建。", Console::Level::Success); return true; }
                    console.Line("同名备份已存在，但内容不同，已停止以免覆盖。", Console::Level::Error); return false;
                }
                console.Line("保留最初的 .pclfix.bak，另存当前状态备份。", Console::Level::Info);
            }
            Fs::copy_file(target, backup, Fs::copy_options::none, error);
            if (error) {
                console.Line("创建备份失败：" + error.message(), Console::Level::Error);
                return false;
            }
            const auto backupHash = HashService::Sha256(backup);
            if (backupHash.size() != 64 || backupHash != HashService::Sha256(target)) {
                console.Line("备份内容校验失败，已停止修补。", Console::Level::Error);
                return false;
            }
            console.Line("已创建备份：" + PathToUtf8(backup.filename()), Console::Level::Success);
            return true;
        }

        static bool Restore(const Fs::path& target, const Console& console) {
            Fs::path backup = target;
            backup += ".pclfix.bak";
            if (!Fs::exists(backup)) {
                console.Line("未找到对应的 .pclfix.bak 备份。", Console::Level::Error);
                return false;
            }
            const FileInfo backupInfo = PeScanner::Scan(backup);
            if (!backupInfo.validPe || backupInfo.checksumError) {
                console.Line("备份文件的 PE 头或校验码无效，无法还原。", Console::Level::Error);
                return false;
            }
            const Fs::path temporary = target.wstring() + L".pclfix.restore.tmp";
            std::error_code error;
            Fs::copy_file(backup, temporary, Fs::copy_options::none, error);
            if (error) {
                console.Line("无法创建还原临时文件：" + error.message(), Console::Level::Error);
                return false;
            }
            if (!PeScanner::Scan(temporary).validPe || HashService::Sha256(temporary) != backupInfo.hash ||
                !MoveFileExW(temporary.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
                Fs::remove(temporary, error);
                console.Line("还原失败，原文件未替换。", Console::Level::Error);
                return false;
            }
            if (HashService::Sha256(target) != backupInfo.hash) {
                console.Line("还原后的内容校验失败，请保留备份文件。", Console::Level::Error);
                return false;
            }
            console.Line("已从备份还原 PCL。", Console::Level::Success);
            return true;
        }
    };

    class ProcessGuard {
    public:
        static std::optional<bool> IsRunning(const Fs::path& executable) {
            const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
            if (snapshot == INVALID_HANDLE_VALUE) return std::nullopt;
            PROCESSENTRY32W entry{};
            entry.dwSize = sizeof(entry);
            bool running = false;
            if (Process32FirstW(snapshot, &entry)) {
                do {
                    const HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID);
                    if (!process) continue;
                    wchar_t processPath[32768]{};
                    DWORD pathLength = static_cast<DWORD>(std::size(processPath));
                    if (QueryFullProcessImageNameW(process, 0, processPath, &pathLength)) {
                        wchar_t targetPath[32768]{};
                        const DWORD targetLength = GetFullPathNameW(executable.c_str(), static_cast<DWORD>(std::size(targetPath)), targetPath, nullptr);
                        if (targetLength > 0 && targetLength < std::size(targetPath) && _wcsicmp(processPath, targetPath) == 0) running = true;
                    }
                    CloseHandle(process);
                    if (running) break;
                } while (Process32NextW(snapshot, &entry));
            }
            CloseHandle(snapshot);
            return running;
        }
    };

    class Application {
    private:
        Console m_console; // 负责控制台颜色、UTF-8 输出和无回显按键读取。

        void Pause() const {
            m_console.Line("按任意键返回。", Console::Level::Info, Console::Color::Orange);
            m_console.ReadKey();
        }

        void Header() const {
            m_console.Line("======================================================", Console::Level::None, Console::Color::Orange); 
            m_console.Line("", Console::Level::None);
            m_console.Line("PCL Download Patcher", Console::Level::None, Console::Color::Orange); 
            m_console.Line("Version: " PCL_DOWNLOAD_PATCHER_VERSION, Console::Level::None, Console::Color::Orange); 
            m_console.Line("Author: " PCL_DOWNLOAD_PATCHER_AUTHOR, Console::Level::None, Console::Color::Orange);
            m_console.Line("GitHub 链接：https://github.com/Txt-Text/PCL-Download-Patcher", Console::Level::None, Console::Color::Orange); 
            m_console.Line("", Console::Level::None);
            m_console.Line("若修补失败请尝试最新版，依旧失败就在 Github 发 Issue 告诉一下 Ta 啦。", Console::Level::None, Console::Color::Orange); 
            m_console.Line("本工具完全免费开源，有帮到就求个 Star 了喵...", Console::Level::None, Console::Color::Orange); 
            m_console.Line("", Console::Level::None);
            m_console.Line("======================================================", Console::Level::None, Console::Color::Orange); 
            m_console.Line("当前版本暂不支持 CE 版，但很快会的。\n", Console::Level::Info, Console::Color::Gray);
        }

        int Menu(const std::vector<std::string>& items) const {
            size_t selected = 0;
            while (true) {
                system("cls"); Header(); m_console.Line("请选择需要进行的操作：", Console::Level::None);
                for (size_t index = 0; index < items.size(); ++index) {
                    m_console.Write(index == selected ? "> " : "  ", Console::Level::None, index == selected ? Console::Color::Yellow : Console::Color::White);
                    m_console.Line(std::to_string(index + 1) + "." + items[index], Console::Level::None, index == selected ? Console::Color::Yellow : Console::Color::White);
                }
                m_console.Line("", Console::Level::None); m_console.Line("[↑] 上一个选项 | [↓] 下一个选项 | [Enter] 确认 | [ESC] 退出", Console::Level::None, Console::Color::Orange);
                const int key = m_console.ReadKey();
                if (key == 256 + 72 || key == 256 + 75) selected = selected == 0 ? items.size() - 1 : selected - 1;
                else if (key == 256 + 80 || key == 256 + 77) selected = (selected + 1) % items.size();
                else if (key == 13) return static_cast<int>(selected);
                else if (key == 27) return -1;
            }
        }

        // 打开系统文件选择窗口；取消时直接返回上级菜单，不改动任何文件。
        std::optional<Fs::path> PickFile() const {
            wchar_t filename[32768]{}; // 接收用户选中的完整文件路径。
            OPENFILENAMEW dialog{}; // 系统文件选择窗口的配置与返回状态。
            dialog.lStructSize = sizeof(dialog);
            dialog.lpstrFilter = L"可执行文件 (*.exe)\0*.exe\0所有文件 (*.*)\0*.*\0"; dialog.lpstrFile = filename; dialog.nMaxFile = static_cast<DWORD>(std::size(filename));
            dialog.hwndOwner = GetConsoleWindow(); // 文件选择窗口归属于当前控制台。
            dialog.lpstrTitle = L"请选择 PCL 可执行文件（.exe）";
            dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
            return GetOpenFileNameW(&dialog) ? std::optional<Fs::path>(filename) : std::nullopt;
        }

        void PatchPcl() {
            system("cls"); Header();
            const auto selected = PickFile(); // 直接由用户选择目标，不枚举程序目录。
            if (!selected) return;
            const auto info = PeScanner::Scan(*selected);
            if (!info.validPe) { m_console.Line("PE 头校验失败，这不是一个有效的EXE文件。", Console::Level::Error); Pause(); return; }
            if (info.checksumError) { m_console.Line("校验码校验失败，可能是文件已损坏或被修改。", Console::Level::Error); Pause(); return; }
            m_console.Line("SHA-256: " + info.hash, Console::Level::Info);

            // 具体鉴权特征由注册的修补点扫描，不依赖程序集字符串标记。
            if (!PatchEngine::CanPatch(info, m_console)) { 
                Pause(); return; 
            }

            m_console.Line("按 [Enter] 创建备份并开始修补，按 [ESC] 取消。", Console::Level::Info, Console::Color::Orange);
            const int confirm = m_console.ReadKey();
            if (confirm != 13) { m_console.Line("已取消。", Console::Level::Info); Pause(); return; }
            const auto processRunning = ProcessGuard::IsRunning(info.path);
            if (!processRunning) {
                m_console.Line("无法检查 PCL 进程状态，停止修补。", Console::Level::Error);
                Pause(); return;
            }
            if (*processRunning) {
                m_console.Line("检测到 PCL 正在运行，请退出 PCL 后重试。", Console::Level::Error);
                Pause(); return;
            }
            const auto beforeWrite = PeScanner::Scan(info.path);
            if (!beforeWrite.validPe || beforeWrite.checksumError || beforeWrite.hash != info.hash) {
                m_console.Line("文件在确认后发生变化，取消修补。", Console::Level::Error);
                Pause(); return;
            }
            if (!BackupManager::Create(info.path, m_console)) { Pause(); return; }
            PatchEngine::Apply(info, m_console);
            Pause();
        }

        // 从主菜单直接选择文件并创建备份。
        void BackupPcl() {
            const auto selected = PickFile(); // 备份目标由用户选择。
            if (!selected) return;
            const auto info = PeScanner::Scan(*selected); // 备份前检查当前 EXE 是否有效。
            if (!info.validPe || info.checksumError) { m_console.Line("PE 头或校验码校验失败。", Console::Level::Error); Pause(); return; }
            const auto running = ProcessGuard::IsRunning(info.path); // 运行中的文件不进行备份。
            if (!running || *running) { m_console.Line("PCL 正在运行或进程状态不可确认，已停止操作。", Console::Level::Error); Pause(); return; }
            BackupManager::Create(info.path, m_console);
            Pause();
        }

        // 从主菜单直接选择待还原文件，覆盖前仍需要确认。
        void RestorePcl() {
            const auto selected = PickFile(); // 还原时允许当前 EXE 损坏；由还原流程校验备份。
            if (!selected) return;
            const auto running = ProcessGuard::IsRunning(*selected); // 避免覆盖正在运行的 PCL。
            if (!running || *running) { m_console.Line("PCL 正在运行或进程状态不可确认，已停止操作。", Console::Level::Error); Pause(); return; }
            m_console.Line("按 [Enter] 确认覆盖当前文件，按 [ESC] 取消。", Console::Level::Info, Console::Color::Orange);
            if (m_console.ReadKey() == 13) BackupManager::Restore(*selected, m_console);
            else m_console.Line("已取消还原。", Console::Level::Info);
            Pause();
        }

    public:
        int Run() {
            (void)PclFix::PatchRuleCatalog::BuiltIn(); // 启动时注册版本规则及各自的修补功能。
            SetConsoleOutputCP(CP_UTF8);
            while (true) {
                const int choice = Menu({"修补PCL", "还原PCL", "备份PCL"}); // 主菜单直接进入对应操作。
                if (choice < 0) return 0;
                switch (choice) {
                case 0: PatchPcl(); break;
                case 1: RestorePcl(); break;
                case 2: BackupPcl(); break;
                }
            }
        }
    };
}

int main() { return Application().Run(); }
