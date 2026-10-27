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

    enum class Color : WORD {
        White = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE,
        Green = FOREGROUND_GREEN | FOREGROUND_INTENSITY,
        Red = FOREGROUND_RED | FOREGROUND_INTENSITY,
        Yellow = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_INTENSITY,
        Orange = FOREGROUND_RED | FOREGROUND_GREEN
    };

    class Console {
    private:
        HANDLE m_handle; // 借用标准输出句柄，不负责关闭。
        WORD m_defaultColor; // 控制台原来的文字颜色。
        DWORD m_originalMode = 0; // 退出时恢复的原控制台模式。
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

        void Write(const std::string& text, Color color = Color::White) const {
            SetConsoleTextAttribute(m_handle, static_cast<WORD>(color));
            const int length = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
            if (length > 0) {
                std::wstring wide(static_cast<size_t>(length), L'\0');
                MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(), length);
                DWORD mode = 0;
                if (GetConsoleMode(m_handle, &mode)) {
                    // Win32 的 16 色调色板没有橙色，支持 VT 时使用 RGB。
                    if (color == Color::Orange && m_virtualTerminal) wide = L"\x1b[38;2;255;165;0m" + wide + L"\x1b[0m";
                    DWORD written = 0;
                    WriteConsoleW(m_handle, wide.data(), static_cast<DWORD>(wide.size()), &written, nullptr);
                } else {
                    DWORD written = 0;
                    WriteFile(m_handle, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
                }
            }
            SetConsoleTextAttribute(m_handle, m_defaultColor);
        }

        void Line(const std::string& text, Color color = Color::White) const { Write(text + "\n", color); }

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
        bool authFeature = false; // 是否命中本工具当前的鉴权特征集合。
    };

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
    private:
        static bool Contains(const std::vector<char>& bytes, const std::string& text) {
            return std::search(bytes.begin(), bytes.end(), text.begin(), text.end()) != bytes.end();
        }

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
            info.authFeature = Contains(bytes, "RestrictMcDownload") && Contains(bytes, "SecretCdnSign");
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
    public:
        static bool CanPatch(const FileInfo& info, const Console& console) {
            const auto analysis = PclFix::ManagedPatch::Analyze(info.path);
            if (!analysis.error.empty()) { console.Line("[-] " + analysis.error, Color::Red); return false; }
            if (analysis.ranges.empty()) {
                if (analysis.unavailableModpack) console.Line("[-] 整合包安装逻辑已被编译器删除，无法仅凭当前文件恢复。", Color::Red);
                else console.Line("[+] 下载页面正常，无需重复修补。", Color::Green);
                return false;
            }
            console.Line("[=] 扫描完毕。总共有 " + std::to_string(analysis.TotalPoints()) + " 个注入点需要修补：", Color::White);
            for (const auto kind : {PclFix::PatchPointKind::DownloadPage, PclFix::PatchPointKind::ModpackDownload,
                PclFix::PatchPointKind::ModifyPage, PclFix::PatchPointKind::ResourceInstall}) {
                const size_t count = analysis.Count(kind) + (kind == PclFix::PatchPointKind::ModpackDownload ? analysis.unavailableModpack : 0);
                if (count) console.Line("[=] " + std::string(PclFix::PatchPointName(kind)) + "：" + std::to_string(count) + " 个", Color::White);
            }
            if (analysis.restoredPages) console.Line("[=] 已存在正常下载页面：" + std::to_string(analysis.restoredPages) + " 个", Color::White);
            if (!analysis.warning.empty()) console.Line("[-] " + analysis.warning, Color::Red);
            return true;
        }

        // 在内存快照中等长修改，临时文件通过结构复检后才替换目标。
        static bool Apply(const FileInfo& info, const Console& console) {
            const auto analysis = PclFix::ManagedPatch::Analyze(info.path);
            if (!analysis.error.empty() || analysis.ranges.empty()) {
                console.Line(analysis.error.empty() ? "[=] 无需重复修补。" : "[-] " + analysis.error, analysis.error.empty() ? Color::White : Color::Red);
                return false;
            }
            std::ifstream input(info.path, std::ios::binary);
            std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(input)), {});
            input.close();
            const auto categories = {PclFix::PatchPointKind::DownloadPage, PclFix::PatchPointKind::ModpackDownload,
                PclFix::PatchPointKind::ModifyPage, PclFix::PatchPointKind::ResourceInstall};
            for (const auto kind : categories) {
                if (!analysis.Count(kind)) continue;
                console.Line("[=] 正在修补 " + std::string(PclFix::PatchPointName(kind)), Color::White);
                for (const auto& range : analysis.ranges) {
                    if (range.kind != kind) continue;
                    if (range.offset > bytes.size() || range.before.size() > bytes.size() - range.offset ||
                        !std::equal(range.before.begin(), range.before.end(), bytes.begin() + range.offset)) return false;
                    std::copy(range.after.begin(), range.after.end(), bytes.begin() + range.offset);
                }
            }
            // PE 校验和为零表示未提供；CLR 程序允许此值。
            IMAGE_DOS_HEADER dos{}; std::memcpy(&dos, bytes.data(), sizeof(dos));
            const size_t checksum = dos.e_lfanew + 4 + sizeof(IMAGE_FILE_HEADER) + offsetof(IMAGE_OPTIONAL_HEADER32, CheckSum);
            if (checksum + 4 > bytes.size()) return false;
            std::fill_n(bytes.begin() + checksum, 4, 0);
            const Fs::path temporary = info.path.wstring() + L".pclfix.tmp";
            // CREATE_NEW 防止覆盖已有临时文件。
            const HANDLE output = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (output == INVALID_HANDLE_VALUE) return false;
            DWORD written = 0;
            const bool saved = WriteFile(output, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) && written == bytes.size() && FlushFileBuffers(output);
            CloseHandle(output);
            const auto verified = PclFix::ManagedPatch::Analyze(temporary);
            const size_t downloadPagePatchCount = analysis.Count(PclFix::PatchPointKind::DownloadPage);
            bool success = saved && verified.error.empty() && verified.ranges.empty() && verified.restoredPages >= analysis.restoredPages + downloadPagePatchCount &&
                PeScanner::Scan(temporary).validPe && HashService::Sha256(info.path) == info.hash;
            if (success) success = ReplaceFileW(info.path.c_str(), temporary.c_str(), nullptr, 0, nullptr, nullptr) != FALSE;
            if (!success) { std::error_code error; Fs::remove(temporary, error); console.Line("[-] 修补或临时文件复检失败，目标未替换。", Color::Red); return false; }
            const auto finalCheck = PclFix::ManagedPatch::Analyze(info.path);
            // ranges 为空表示本次识别到的页面、修改页和资源入口都已消失；下载页面额外检查正常加载器计数。
            success = finalCheck.error.empty() && finalCheck.ranges.empty() && finalCheck.restoredPages >= verified.restoredPages;
            if (success) {
                console.Line("[+] 修补与复检完成。", Color::Green);
                if (!finalCheck.warning.empty()) console.Line("[-] " + finalCheck.warning, Color::Red);
            } else console.Line("[-] 替换后复检失败，请使用备份还原。", Color::Red);
            return success;
        }
    };

    class BackupManager {
    public:
        static bool Create(const Fs::path& target, const Console& console) {
            Fs::path backup = target;
            backup += ".pclfix.bak";
            std::error_code error;
            if (Fs::exists(backup, error)) {
                const auto existingHash = HashService::Sha256(backup);
                if (existingHash.size() == 64 && existingHash == HashService::Sha256(target)) {
                    console.Line("[+] 已有备份与当前文件一致，复用该备份。", Color::Green);
                    return true;
                }
                console.Line("[-] 备份文件已存在，为避免覆盖拒绝继续：" + PathToUtf8(backup.filename()), Color::Red);
                return false;
            }
            Fs::copy_file(target, backup, Fs::copy_options::none, error);
            if (error) {
                console.Line("[-] 创建备份失败：" + error.message(), Color::Red);
                return false;
            }
            const auto backupHash = HashService::Sha256(backup);
            if (backupHash.size() != 64 || backupHash != HashService::Sha256(target)) {
                console.Line("[-] 备份内容校验失败，已停止修补。", Color::Red);
                return false;
            }
            console.Line("[+] 已创建备份：" + PathToUtf8(backup.filename()), Color::Green);
            return true;
        }

        static bool Restore(const Fs::path& target, const Console& console) {
            Fs::path backup = target;
            backup += ".pclfix.bak";
            if (!Fs::exists(backup)) {
                console.Line("[-] 未找到对应的 .pclfix.bak 备份。", Color::Red);
                return false;
            }
            const FileInfo backupInfo = PeScanner::Scan(backup);
            if (!backupInfo.validPe || backupInfo.checksumError) {
                console.Line("[-] 备份文件的 PE 头或校验码无效，拒绝还原。", Color::Red);
                return false;
            }
            const Fs::path temporary = target.wstring() + L".pclfix.restore.tmp";
            std::error_code error;
            Fs::copy_file(backup, temporary, Fs::copy_options::none, error);
            if (error) {
                console.Line("[-] 无法创建还原临时文件：" + error.message(), Color::Red);
                return false;
            }
            if (!PeScanner::Scan(temporary).validPe || HashService::Sha256(temporary) != backupInfo.hash ||
                !MoveFileExW(temporary.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
                Fs::remove(temporary, error);
                console.Line("[-] 还原失败，原文件未替换。", Color::Red);
                return false;
            }
            if (HashService::Sha256(target) != backupInfo.hash) {
                console.Line("[-] 还原后的内容校验失败，请保留备份文件。", Color::Red);
                return false;
            }
            console.Line("[+] 已从备份还原 PCL。", Color::Green);
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
            m_console.Line("[=] 按任意键返回。", Color::Orange);
            m_console.ReadKey();
        }

        void Header() const {
            m_console.Line("==========================="); 
            m_console.Line("");
            m_console.Line("PCL Download Patcher v1.0"); 
            m_console.Line("Author: Txt_Text");
            m_console.Line("");
            m_console.Line("本工具完全免费开源！项目 GitHub 链接：https://github.com/Txt-Text/PCL-Download-Patcher"); 
            m_console.Line("");
            m_console.Line("===========================");
        }

        int Menu(const std::vector<std::string>& items) const {
            size_t selected = 0;
            while (true) {
                system("cls"); Header(); m_console.Line("请选择需要进行的操作：");
                for (size_t index = 0; index < items.size(); ++index) {
                    m_console.Write(index == selected ? "> " : "  ", index == selected ? Color::Yellow : Color::White);
                    m_console.Line(std::to_string(index + 1) + "." + items[index], index == selected ? Color::Yellow : Color::White);
                }
                m_console.Line(""); m_console.Line("[↑] 上一个选项 | [↓] 下一个选项 | [Enter] 确认 | [ESC] 退出", Color::Orange);
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
            dialog.lpstrTitle = L"请选择 PCL 可执行文件";
            dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
            return GetOpenFileNameW(&dialog) ? std::optional<Fs::path>(filename) : std::nullopt;
        }

        void PatchPcl() {
            system("cls"); Header();
            const auto selected = PickFile(); // 直接由用户选择目标，不枚举程序目录。
            if (!selected) return;
            const auto info = PeScanner::Scan(*selected);
            if (!info.validPe) { m_console.Line("[-] PE头校验错误，这不是一个有效的EXE文件。", Color::Red); Pause(); return; }
            if (info.checksumError) { m_console.Line("[-] 校验码校验错误，文件可能已损坏或被修改。", Color::Red); Pause(); return; }
            m_console.Line(info.authFeature ? "[+] 识别到新版鉴权特征。" : "[-] 未识别到新版鉴权特征，可能是旧版、社区版和其他改版。", info.authFeature ? Color::Green : Color::Red);
            m_console.Line("[=] SHA-256: " + info.hash);
            // 字符串特征仅供提示；最终兼容性以元数据和 IL 结构校验为准。
            if (!PatchEngine::CanPatch(info, m_console)) { Pause(); return; }
            m_console.Line("[=] 按 Enter 创建备份并开始修补，按 ESC 取消。", Color::Orange);
            const int confirm = m_console.ReadKey();
            if (confirm != 13) { m_console.Line("[=] 已取消。", Color::White); Pause(); return; }
            const auto processRunning = ProcessGuard::IsRunning(info.path);
            if (!processRunning) {
                m_console.Line("[-] 无法检查 PCL 进程状态，已停止修补。", Color::Red);
                Pause(); return;
            }
            if (*processRunning) {
                m_console.Line("[-] 检测到 PCL 正在运行，请先完全退出 PCL 后重试。", Color::Red);
                Pause(); return;
            }
            const auto beforeWrite = PeScanner::Scan(info.path);
            if (!beforeWrite.validPe || beforeWrite.checksumError || beforeWrite.hash != info.hash) {
                m_console.Line("[-] 文件在确认后发生变化，已取消修补。", Color::Red);
                Pause(); return;
            }
            if (!BackupManager::Create(info.path, m_console)) { Pause(); return; }
            PatchEngine::Apply(info, m_console);
            Pause();
        }

        void BackupRestore() {
            const int operation = Menu({"备份PCL", "还原PCL", "返回"});
            if (operation < 0 || operation == 2) return;
            const auto selected = PickFile(); // 备份与还原同样只使用文件选择窗口。
            if (!selected) return;
            const auto info = PeScanner::Scan(*selected);
            // 还原时允许当前 EXE 损坏；需要校验的是待恢复的备份。
            if (operation == 0 && (!info.validPe || info.checksumError)) { m_console.Line("[-] PE头或校验码校验错误。", Color::Red); Pause(); return; }
            const auto running = ProcessGuard::IsRunning(info.path);
            if (!running || *running) { m_console.Line("[-] PCL 正在运行或进程状态不可确认，已停止操作。", Color::Red); Pause(); return; }
            if (operation == 0) BackupManager::Create(info.path, m_console);
            else {
                m_console.Line("[=] 按 Enter 确认覆盖当前文件，按 ESC 取消。", Color::Orange);
                if (m_console.ReadKey() == 13) BackupManager::Restore(info.path, m_console);
                else m_console.Line("[=] 已取消还原。", Color::White);
            }
            Pause();
        }

    public:
        int Run() {
            SetConsoleOutputCP(CP_UTF8);
            while (true) { const int choice = Menu({"修补PCL", "备份与还原PCL"}); if (choice < 0) return 0; if (choice == 0) PatchPcl(); else BackupRestore(); }
        }
    };
}

int main() { return Application().Run(); }
