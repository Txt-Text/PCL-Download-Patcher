#pragma once
#include <windows.h>
#include <cor.h>
#include <metahost.h>
#include <wrl/client.h>
#include <filesystem>
#include <fstream>
#include <vector>
#include <string>
#include <span>
#include <stdexcept>
#include <cstring>
#include <algorithm>
#include <cstdint>

#pragma comment(lib, "mscoree.lib")

namespace PclFix {
    // 一段等长 IL 替换，保存原始字节以防分析后目标文件改变。
    struct PatchRange {
        size_t offset = 0; // 文件内的实际偏移，每次从 RVA 动态计算。
        std::vector<unsigned char> before; // 替换前字节。
        std::vector<unsigned char> after; // 替换后字节。
    };

    // 解析结果同时用于界面显示、事务写入和修补后复检。
    struct PatchAnalysis {
        std::vector<PatchRange> ranges; // 所有确认匹配的下载页面。
        size_t restoredPages = 0; // 已直接绑定正常加载器的页面数。
        std::string error; // 结构不符时的具体原因。
    };

    // 只读解析 CLR 元数据与 CIL，不加载或执行目标程序集。
    class ManagedPatch {
    private:
        using Bytes = std::vector<unsigned char>;
        template<class T> using ComPtr = Microsoft::WRL::ComPtr<T>;
        Bytes m_bytes; // 目标 PE 文件的只读分析快照。
        ComPtr<IMetaDataImport2> m_metadata; // CLR 提供的元数据只读接口。
        std::vector<IMAGE_SECTION_HEADER> m_sections; // RVA 到文件偏移的映射。
        mdFieldDef m_loader = 0; // 正常原版版本列表加载器。
        Bytes m_loaderSignature; // 字段签名，用于确认替换对象的类型一致。

        // 元数据枚举句柄由作用域管理，解析中途抛出异常时也会释放。
        struct MetadataEnumeration {
            IMetaDataImport2* metadata; // 借用解析器的接口，不接管 COM 所有权。
            HCORENUM handle = nullptr; // CLR 枚举状态。
            ~MetadataEnumeration() { if (handle) metadata->CloseEnum(handle); }
        };

        struct Instruction {
            size_t offset = 0; // 方法 IL 内偏移。
            size_t size = 0; // 包含操作数的指令长度。
            unsigned opcode = 0; // 一字节或 FE 前缀操作码。
            unsigned token = 0; // 元数据令牌或四字节操作数。
            std::vector<size_t> targets; // 所有分支目标。
        };

        struct MethodBody {
            size_t offset = 0; // 方法 IL 在文件中的位置。
            size_t length = 0; // 原始代码长度。
            bool extraSections = false; // 是否有异常处理等附加节。
            std::vector<Instruction> instructions; // 完整解码后的指令。
        };

        // 在任何解引用前做边界检查，损坏文件直接返回错误。
        template<class T> T Read(size_t offset) const {
            if (offset > m_bytes.size() || sizeof(T) > m_bytes.size() - offset) throw std::runtime_error("PE/IL 数据越界。");
            T value{};
            std::memcpy(&value, m_bytes.data() + offset, sizeof(value));
            return value;
        }

        // 将当前文件自己的 RVA 转换成真实文件偏移，不使用固定地址。
        size_t FileOffset(ULONG rva) const {
            for (const auto& section : m_sections) {
                if (rva >= section.VirtualAddress && rva - section.VirtualAddress < section.SizeOfRawData) {
                    const size_t result = static_cast<size_t>(section.PointerToRawData) + rva - section.VirtualAddress;
                    if (result < m_bytes.size()) return result;
                }
            }
            throw std::runtime_error("方法 RVA 未映射到有效文件节。");
        }

        // 提取类型名；TypeSpec 会递归解析其泛型类型定义。
        std::wstring TypeName(mdToken token) const {
            wchar_t name[2048]{};
            if (TypeFromToken(token) == mdtTypeDef) {
                if (FAILED(m_metadata->GetTypeDefProps(token, name, 2048, nullptr, nullptr, nullptr))) return {};
            } else if (TypeFromToken(token) == mdtTypeRef) {
                if (FAILED(m_metadata->GetTypeRefProps(token, nullptr, name, 2048, nullptr))) return {};
            } else if (TypeFromToken(token) == mdtTypeSpec) {
                PCCOR_SIGNATURE signature = nullptr; ULONG size = 0;
                if (FAILED(m_metadata->GetTypeSpecFromToken(token, &signature, &size)) || size < 3 || signature[0] != ELEMENT_TYPE_GENERICINST) return {};
                const auto* cursor = signature + 2;
                const size_t width = (*cursor & 0x80) == 0 ? 1 : ((*cursor & 0xc0) == 0x80 ? 2 : 4);
                if (size < 2 + width || (*cursor & 0xe0) == 0xe0 ||
                    (signature[1] != ELEMENT_TYPE_CLASS && signature[1] != ELEMENT_TYPE_VALUETYPE)) return {};
                mdToken type = 0;
                CorSigUncompressToken(cursor, &type);
                return TypeName(type);
            }
            return name;
        }

        // 解析方法名与其声明类型，避免误把其他构造器当成加载器。
        std::wstring MemberName(mdToken token, mdToken& owner) const {
            wchar_t name[2048]{};
            if (TypeFromToken(token) == mdtMethodSpec) {
                mdToken parent = 0;
                if (FAILED(m_metadata->GetMethodSpecProps(token, &parent, nullptr, nullptr))) return {};
                return MemberName(parent, owner);
            }
            HRESULT result = E_FAIL;
            if (TypeFromToken(token) == mdtMethodDef) result = m_metadata->GetMethodProps(token, &owner, name, 2048, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
            else if (TypeFromToken(token) == mdtMemberRef) result = m_metadata->GetMemberRefProps(token, &owner, name, 2048, nullptr, nullptr, nullptr);
            return SUCCEEDED(result) ? std::wstring(name) : std::wstring();
        }

        // 解码操作数宽度，表直接来自 Windows CLR SDK 的标准 opcode.def。
        static int OperandSize(unsigned opcode) {
            constexpr int InlineNone = 0, ShortInlineI = 1, ShortInlineVar = 1, ShortInlineBrTarget = 1;
            constexpr int InlineVar = 2, InlineI = 4, InlineMethod = 4, InlineField = 4, InlineString = 4;
            constexpr int InlineType = 4, InlineTok = 4, InlineSig = 4, InlineBrTarget = 4, ShortInlineR = 4;
            constexpr int InlineI8 = 8, InlineR = 8, InlineSwitch = -2;
            switch (opcode) {
#define OPDEF(id, name, pop, push, operand, kind, length, first, second, flow) case ((length == 2 ? 0xfe00 : (length == 0 ? 0x10000 + __LINE__ : 0)) | second): return length == 0 ? -1 : operand;
#define OPALIAS(a, b, c)
#include <opcode.def>
#undef OPALIAS
#undef OPDEF
            default: return -1;
            }
        }

        // 解析 tiny/fat 方法头，并校验所有分支都落在指令边界上。
        MethodBody Body(mdMethodDef method) const {
            ULONG rva = 0;
            if (FAILED(m_metadata->GetMethodProps(method, nullptr, nullptr, 0, nullptr, nullptr, nullptr, nullptr, &rva, nullptr)) || !rva) throw std::runtime_error("目标方法没有托管 IL。");
            const size_t header = FileOffset(rva);
            MethodBody body;
            const unsigned char first = Read<unsigned char>(header);
            if ((first & 3) == 2) { body.offset = header + 1; body.length = first >> 2; }
            else if ((first & 3) == 3) {
                const WORD flags = Read<WORD>(header);
                const size_t headerSize = (flags >> 12) * 4;
                if (headerSize < 12) throw std::runtime_error("非法 fat IL 方法头。");
                body.offset = header + headerSize; body.length = Read<DWORD>(header + 4); body.extraSections = (flags & 8) != 0;
            } else throw std::runtime_error("未知 IL 方法头。");
            if (body.offset > m_bytes.size() || body.length > m_bytes.size() - body.offset) throw std::runtime_error("IL 方法体越界。");
            size_t cursor = 0;
            while (cursor < body.length) {
                Instruction instruction; instruction.offset = cursor;
                instruction.opcode = Read<unsigned char>(body.offset + cursor++);
                if (instruction.opcode == 0xfe) {
                    if (cursor == body.length) throw std::runtime_error("IL 双字节指令被截断。");
                    instruction.opcode = 0xfe00 | Read<unsigned char>(body.offset + cursor++);
                }
                const int operandSize = OperandSize(instruction.opcode);
                if (operandSize == -1) throw std::runtime_error("不支持的 IL 指令。");
                if (operandSize == -2) {
                    if (body.length - cursor < 4) throw std::runtime_error("IL switch 操作数被截断。");
                    const DWORD count = Read<DWORD>(body.offset + cursor);
                    if (cursor + 4 > body.length || count > (body.length - cursor - 4) / 4) throw std::runtime_error("非法 switch 指令。");
                    const size_t end = cursor + 4 + count * 4;
                    for (DWORD index = 0; index < count; ++index) instruction.targets.push_back(end + Read<int32_t>(body.offset + cursor + 4 + index * 4));
                    cursor = end;
                } else {
                    if (static_cast<size_t>(operandSize) > body.length - cursor) throw std::runtime_error("IL 操作数越界。");
                    if (operandSize == 4) instruction.token = Read<DWORD>(body.offset + cursor);
                    if ((instruction.opcode >= 0x2b && instruction.opcode <= 0x37) || instruction.opcode == 0xde) instruction.targets.push_back(cursor + 1 + Read<int8_t>(body.offset + cursor));
                    if ((instruction.opcode >= 0x38 && instruction.opcode <= 0x44) || instruction.opcode == 0xdd) instruction.targets.push_back(cursor + 4 + Read<int32_t>(body.offset + cursor));
                    cursor += operandSize;
                }
                instruction.size = cursor - instruction.offset;
                body.instructions.push_back(instruction);
            }
            for (const auto& instruction : body.instructions) for (const size_t target : instruction.targets) {
                if (std::none_of(body.instructions.begin(), body.instructions.end(), [target](const auto& item) { return item.offset == target; })) throw std::runtime_error("IL 跳转没有落在指令边界。");
            }
            return body;
        }

        // 从实际元数据中查找正常下载加载器及其字段类型签名。
        void FindLoader() {
            mdTypeDef type = 0;
            if (FAILED(m_metadata->FindTypeDefByName(L"PCL.ModDownload", 0, &type))) throw std::runtime_error("未找到 PCL 下载模块。");
            MetadataEnumeration enumeration{m_metadata.Get()};
            mdFieldDef fields[128]{}; ULONG count = 0;
            while (SUCCEEDED(m_metadata->EnumFields(&enumeration.handle, type, fields, 128, &count)) && count) {
                for (ULONG index = 0; index < count; ++index) {
                    wchar_t name[512]{}; PCCOR_SIGNATURE signature = nullptr; ULONG size = 0; DWORD attributes = 0;
                    if (SUCCEEDED(m_metadata->GetFieldProps(fields[index], nullptr, name, 512, nullptr, &attributes, &signature, &size, nullptr, nullptr, nullptr)) && std::wstring(name) == L"DlClientListLoader" && (attributes & fdStatic)) {
                        m_loader = fields[index]; m_loaderSignature.assign(signature, signature + size);
                    }
                }
            }
            if (!m_loader || m_loaderSignature.size() < 4 || m_loaderSignature[0] != IMAGE_CEE_CS_CALLCONV_FIELD) throw std::runtime_error("正常原版加载器字段缺失或签名不匹配。");
        }

        // 确认构造对象与正常字段具有同一个泛型类型签名。
        bool IsLoaderConstructor(mdToken token) const {
            mdToken owner = 0;
            if (MemberName(token, owner) != L".ctor" || TypeFromToken(owner) != mdtTypeSpec) return false;
            PCCOR_SIGNATURE signature = nullptr; ULONG size = 0;
            if (FAILED(m_metadata->GetTypeSpecFromToken(owner, &signature, &size))) return false;
            return size + 1 == m_loaderSignature.size() && std::equal(signature, signature + size, m_loaderSignature.begin() + 1);
        }

        // 检查被删除的表达式只构造一个任务，不读取表达式外的栈值。
        // 所有分支合流时栈深必须一致，防止仅凭字符串误改相似代码。
        void ValidateExpression(const MethodBody& body, size_t first_index, size_t end_offset) const {
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
                    if (MemberName(instruction.token, owner) != L".ctor") throw std::runtime_error("任务表达式存在未知构造调用。");
                    HRESULT result = E_FAIL;
                    if (TypeFromToken(instruction.token) == mdtMemberRef) result = m_metadata->GetMemberRefProps(instruction.token, nullptr, nullptr, 0, nullptr, &signature, &length);
                    else if (TypeFromToken(instruction.token) == mdtMethodDef) result = m_metadata->GetMethodProps(instruction.token, nullptr, nullptr, 0, nullptr, nullptr, &signature, &length, nullptr, nullptr);
                    if (FAILED(result) || length < 2 || (signature[0] & IMAGE_CEE_CS_CALLCONV_GENERIC)) throw std::runtime_error("构造器签名不受支持。");
                    const auto* cursor = signature + 1; // 构造器无泛型方法参数，下一项为参数个数。
                    const size_t width = (*cursor & 0x80) == 0 ? 1 : ((*cursor & 0xc0) == 0x80 ? 2 : 4);
                    if (length < 1 + width || (*cursor & 0xe0) == 0xe0) throw std::runtime_error("构造器签名被截断。");
                    pop = static_cast<int>(CorSigUncompressData(cursor)); push = 1;
                } else throw std::runtime_error("限制任务表达式存在不支持的指令，拒绝改写。");
                if (depths[index] < pop) throw std::runtime_error("限制任务依赖表达式外部的栈值。");
                const int depth = depths[index] - pop + push;
                auto visit = [&](size_t target) {
                    if (target == end_offset) {
                        if (depth != 1) throw std::runtime_error("限制任务表达式不是单值结果。");
                        reachesEnd = true; return;
                    }
                    if (target < body.instructions[first_index].offset || target >= end_offset) throw std::runtime_error("限制任务跳出了表达式范围。");
                    const auto found = std::find_if(body.instructions.begin(), body.instructions.end(), [target](const auto& item) { return item.offset == target; });
                    if (found == body.instructions.end()) throw std::runtime_error("限制任务分支目标无效。");
                    const size_t next = static_cast<size_t>(found - body.instructions.begin());
                    if (depths[next] == -1) { depths[next] = depth; pending.push_back(next); }
                    else if (depths[next] != depth) throw std::runtime_error("限制任务分支栈深不一致。");
                };
                for (const size_t target : instruction.targets) {
                    if (target <= instruction.offset) throw std::runtime_error("限制任务表达式含循环，拒绝改写。");
                    visit(target);
                }
                if (opcode != 0x2b && opcode != 0x38) visit(instruction.offset + instruction.size);
            }
            if (!reachesEnd) throw std::runtime_error("限制任务表达式没有正常出口。");
        }

        // 只有同时命中关闭下载字符串、正确构造器和 PageLoaderInit 才生成改写。
        void AnalyzePage(mdTypeDef type, PatchAnalysis& analysis) const {
            mdMethodDef method = 0;
            if (FAILED(m_metadata->FindMethod(type, L"LoaderInit", nullptr, 0, &method))) return;
            const MethodBody body = Body(method);
            if (body.extraSections) throw std::runtime_error("加载器方法含异常处理区域，需更新结构规则。");
            bool callsPageLoader = false;
            for (const auto& instruction : body.instructions) {
                if (instruction.opcode == 0x28 || instruction.opcode == 0x6f) {
                    mdToken owner = 0;
                    if (MemberName(instruction.token, owner) == L"PageLoaderInit" && TypeName(owner) == L"PCL.MyPageRight") callsPageLoader = true;
                }
            }
            if (!callsPageLoader) return;
            size_t restrictionCount = 0;
            for (size_t index = 0; index < body.instructions.size(); ++index) {
                const auto& instruction = body.instructions[index];
                if (instruction.opcode == 0x7e && instruction.token == m_loader) ++analysis.restoredPages;
                if (instruction.opcode != 0x72) continue;
                wchar_t text[1024]{}; ULONG count = 0;
                if (FAILED(m_metadata->GetUserString(instruction.token, text, 1024, &count)) || std::wstring(text, count) != L"关闭下载") continue;
                size_t end = 0;
                for (size_t next = index + 1; next < body.instructions.size(); ++next) {
                    const auto& candidate = body.instructions[next];
                    if (candidate.opcode == 0x73 && IsLoaderConstructor(candidate.token)) { end = candidate.offset + candidate.size; break; }
                }
                if (!end || end - instruction.offset < 5) throw std::runtime_error("关闭下载任务的构造器类型不匹配。");
                for (const auto& branch : body.instructions) for (size_t target : branch.targets) {
                    const bool insideSource = branch.offset >= instruction.offset && branch.offset < end;
                    const bool insideTarget = target >= instruction.offset && target < end;
                    if ((!insideSource && insideTarget && target != instruction.offset) || (insideSource && !insideTarget && target != end)) throw std::runtime_error("限制任务边界存在外部跳转，拒绝改写。");
                }
                ValidateExpression(body, index, end);
                PatchRange range; range.offset = body.offset + instruction.offset;
                range.before.assign(m_bytes.begin() + range.offset, m_bytes.begin() + body.offset + end);
                range.after.assign(range.before.size(), 0); // 剩余字节使用 nop 保持原方法长度。
                range.after[0] = 0x7e; // ldsfld：压入程序已有的正常加载器。
                std::memcpy(range.after.data() + 1, &m_loader, 4);
                analysis.ranges.push_back(std::move(range)); ++restrictionCount;
            }
            if (restrictionCount > 1) throw std::runtime_error("同一页面出现多个限制任务，规则存在歧义。");
        }

        // 创建只读元数据作用域；不把目标程序集加载到 CLR 执行上下文。
        void Open(const std::filesystem::path& path) {
            std::ifstream input(path, std::ios::binary);
            if (!input) throw std::runtime_error("无法读取目标文件。");
            // 一次性读取，避免 Debug 下逐字节迭代导致界面长时间无响应。
            input.seekg(0, std::ios::end);
            const auto length = input.tellg(); // 元数据 API 的文件大小参数为 ULONG。
            if (length <= 0 || static_cast<uint64_t>(length) > MAXDWORD) throw std::runtime_error("文件大小不受支持。");
            m_bytes.resize(static_cast<size_t>(length));
            input.seekg(0);
            if (!input.read(reinterpret_cast<char*>(m_bytes.data()), length)) throw std::runtime_error("目标文件读取不完整。");
            const auto dos = Read<IMAGE_DOS_HEADER>(0);
            if (dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew <= 0) throw std::runtime_error("无效 DOS 头。");
            const size_t pe = dos.e_lfanew;
            if (Read<DWORD>(pe) != IMAGE_NT_SIGNATURE) throw std::runtime_error("无效 PE 头。");
            const auto file = Read<IMAGE_FILE_HEADER>(pe + 4);
            const size_t sections = pe + 4 + sizeof(file) + file.SizeOfOptionalHeader;
            for (unsigned index = 0; index < file.NumberOfSections; ++index) m_sections.push_back(Read<IMAGE_SECTION_HEADER>(sections + index * sizeof(IMAGE_SECTION_HEADER)));
            ComPtr<ICLRMetaHost> host; ComPtr<ICLRRuntimeInfo> runtime; ComPtr<IMetaDataDispenser> dispenser;
            if (FAILED(CLRCreateInstance(CLSID_CLRMetaHost, IID_PPV_ARGS(&host))) || FAILED(host->GetRuntime(L"v4.0.30319", IID_PPV_ARGS(&runtime))) ||
                FAILED(runtime->GetInterface(CLSID_CorMetaDataDispenser, IID_IMetaDataDispenser, reinterpret_cast<void**>(dispenser.GetAddressOf()))) ||
                FAILED(dispenser->OpenScopeOnMemory(m_bytes.data(), static_cast<ULONG>(m_bytes.size()), ofRead, IID_IMetaDataImport2, reinterpret_cast<IUnknown**>(m_metadata.GetAddressOf())))) throw std::runtime_error("无法解析 .NET 元数据。");
            FindLoader();
        }

    public:
        // 分析所有下载页面；规则与文件版本号、哈希和固定地址无关。
        static PatchAnalysis Analyze(const std::filesystem::path& path) {
            PatchAnalysis analysis;
            try {
                ManagedPatch parser; parser.Open(path);
                MetadataEnumeration enumeration{parser.m_metadata.Get()};
                mdTypeDef types[128]{}; ULONG count = 0;
                while (SUCCEEDED(parser.m_metadata->EnumTypeDefs(&enumeration.handle, types, 128, &count)) && count) {
                    for (ULONG index = 0; index < count; ++index) {
                        const std::wstring name = parser.TypeName(types[index]);
                        if (name.starts_with(L"PCL.PageDownload")) parser.AnalyzePage(types[index], analysis);
                    }
                }
                if (analysis.ranges.empty() && !analysis.restoredPages) analysis.error = "没有匹配到支持的下载页面绑定结构。";
            } catch (const std::exception& error) { analysis.error = error.what(); analysis.ranges.clear(); }
            return analysis;
        }
    };
}
