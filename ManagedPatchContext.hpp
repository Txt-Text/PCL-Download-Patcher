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
#include <functional>
#include <map>
#include <set>
#include <optional>
#include <memory>
#include <limits>
#include <string_view>
#include "ManagedRewrite.h"
#include "ManagedResources.h"
#include "PatchTypes.hpp"

#pragma comment(lib, "mscoree.lib")

namespace PclFix {

    // 只读解析 CLR 元数据与 CIL，不加载或执行目标程序集。
    class ManagedPatchContext {
    public:
        using Bytes = std::vector<unsigned char>;
        template<class T> using ComPtr = Microsoft::WRL::ComPtr<T>;
    private:
        Bytes m_bytes; // 目标 PE 文件的只读分析快照。
        ComPtr<IMetaDataImport2> m_metadata; // CLR 提供的元数据只读接口。
        ComPtr<IMetaDataEmit2> m_emitter; // 仅用于候选镜像的字符串、签名与方法 RVA 更新。
        std::vector<IMAGE_SECTION_HEADER> m_sections; // RVA 到文件偏移的映射。

    public:
        // 返回借用的只读元数据接口；作用域由当前上下文持有。
        IMetaDataImport2* Metadata() const { return m_metadata.Get(); }

        // 返回输入文件的只读字节视图，修补只写入候选镜像。
        const Bytes& BytesView() const { return m_bytes; }

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
            size_t header = 0; // 方法头的文件偏移。
            size_t offset = 0; // 方法 IL 在文件中的位置。
            size_t length = 0; // 原始代码长度。
            WORD maxStack = 8; // tiny 格式缺省为 8，重建时不会降低此值。
            mdSignature locals = 0; // 现有局部变量签名，不在版本间复制令牌。
            bool extraSections = false; // 是否有异常处理等附加节。
            std::vector<Instruction> instructions; // 完整解码后的指令。
        };

        // 枚举当前程序集的类型定义，供各个注册特征独立筛选自身结构。
        std::vector<mdTypeDef> TypeDefinitions() const {
            MetadataEnumeration enumeration{m_metadata.Get()};
            mdTypeDef types[128]{};
            ULONG count = 0;
            std::vector<mdTypeDef> result;
            while (SUCCEEDED(m_metadata->EnumTypeDefs(&enumeration.handle, types, 128, &count)) && count) {
                result.insert(result.end(), types, types + count);
            }
            return result;
        }

        // 在任何解引用前做边界检查，损坏文件直接返回错误。
        template<class T> T Read(size_t offset) const {
            if (offset > m_bytes.size() || sizeof(T) > m_bytes.size() - offset) throw std::runtime_error("程序代码的读取位置超出文件范围，文件可能已损坏。");
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
            throw std::runtime_error("无法在 EXE 文件中找到对应代码的位置，文件可能已损坏。");
        }

        // 提取类型名；TypeSpec 会递归解析其泛型类型定义。
        std::wstring TypeName(mdToken token) const {
            wchar_t name[2048]{};
            if (TypeFromToken(token) == mdtTypeDef) {
                if (FAILED(m_metadata->GetTypeDefProps(token, name, 2048, nullptr, nullptr, nullptr))) return {};
                mdTypeDef parent = 0;
                if (SUCCEEDED(m_metadata->GetNestedClassProps(token, &parent))) return TypeName(parent) + L"/" + name;
            } else if (TypeFromToken(token) == mdtTypeRef) {
                mdToken scope = 0;
                if (FAILED(m_metadata->GetTypeRefProps(token, &scope, name, 2048, nullptr))) return {};
                if (TypeFromToken(scope) == mdtTypeRef) return TypeName(scope) + L"/" + name;
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
            if (FAILED(m_metadata->GetMethodProps(method, nullptr, nullptr, 0, nullptr, nullptr, nullptr, nullptr, &rva, nullptr)) || !rva) throw std::runtime_error("目标函数不包含可修补的 .NET 代码。");
            const size_t header = FileOffset(rva);
            MethodBody body;
            body.header = header;
            const unsigned char first = Read<unsigned char>(header);
            if ((first & 3) == 2) { body.offset = header + 1; body.length = first >> 2; }
            else if ((first & 3) == 3) {
                const WORD flags = Read<WORD>(header);
                const size_t headerSize = (flags >> 12) * 4;
                if (headerSize < 12) throw std::runtime_error("函数的 IL 代码头无效，文件可能已损坏。");
                body.offset = header + headerSize; body.length = Read<DWORD>(header + 4); body.extraSections = (flags & 8) != 0;
                body.maxStack = Read<WORD>(header + 2); body.locals = Read<DWORD>(header + 8);
            } else throw std::runtime_error("无法识别函数的 IL 代码格式。");
            if (body.offset > m_bytes.size() || body.length > m_bytes.size() - body.offset || body.length > static_cast<size_t>((std::numeric_limits<int>::max)())) throw std::runtime_error("函数的 IL 代码不完整或过大，无法读取。");
            size_t cursor = 0;
            while (cursor < body.length) {
                Instruction instruction; instruction.offset = cursor;
                instruction.opcode = Read<unsigned char>(body.offset + cursor++);
                if (instruction.opcode == 0xfe) {
                    if (cursor == body.length) throw std::runtime_error("IL 指令不完整，文件可能已损坏。");
                    instruction.opcode = 0xfe00 | Read<unsigned char>(body.offset + cursor++);
                }
                const int operandSize = OperandSize(instruction.opcode);
                if (operandSize == -1) throw std::runtime_error("不支持的 IL 指令。");
                if (operandSize == -2) {
                    if (body.length - cursor < 4) throw std::runtime_error("IL 分支指令的数据不完整，文件可能已损坏。");
                    const DWORD count = Read<DWORD>(body.offset + cursor);
                    if (cursor + 4 > body.length || count > (body.length - cursor - 4) / 4) throw std::runtime_error("IL 分支指令的数据无效，文件可能已损坏。");
                    const size_t end = cursor + 4 + count * 4;
                    for (DWORD index = 0; index < count; ++index) instruction.targets.push_back(end + Read<int32_t>(body.offset + cursor + 4 + index * 4));
                    cursor = end;
                } else {
                    if (static_cast<size_t>(operandSize) > body.length - cursor) throw std::runtime_error("IL 指令的数据超出代码范围，文件可能已损坏。");
                    if (operandSize == 4) instruction.token = Read<DWORD>(body.offset + cursor);
                    if ((instruction.opcode >= 0x2b && instruction.opcode <= 0x37) || instruction.opcode == 0xde) instruction.targets.push_back(cursor + 1 + Read<int8_t>(body.offset + cursor));
                    if ((instruction.opcode >= 0x38 && instruction.opcode <= 0x44) || instruction.opcode == 0xdd) instruction.targets.push_back(cursor + 4 + Read<int32_t>(body.offset + cursor));
                    cursor += operandSize;
                }
                instruction.size = cursor - instruction.offset;
                body.instructions.push_back(instruction);
            }
            for (const auto& instruction : body.instructions) for (const size_t target : instruction.targets) {
                if (std::none_of(body.instructions.begin(), body.instructions.end(), [target](const auto& item) { return item.offset == target; })) throw std::runtime_error("IL 跳转位置无效，文件可能已损坏或被修改。");
            }
            return body;
        }

        // 读取压缩整数前先验证边界，不能让损坏签名使 CLR 的内联读取越界。
        static ULONG SigNumber(const unsigned char*& cursor, const unsigned char* end) {
            if (cursor >= end) throw std::runtime_error("程序元数据中的类型信息不完整，文件可能已损坏。");
            const size_t size = (*cursor & 0x80) == 0 ? 1 : ((*cursor & 0xc0) == 0x80 ? 2 : 4);
            if ((*cursor & 0xe0) == 0xe0 || static_cast<size_t>(end - cursor) < size) throw std::runtime_error("程序元数据中的类型编码无效。");
            return CorSigUncompressData(cursor);
        }

        // 解码语义类型，包括泛型参数、数组和嵌套类型；令牌只在当前文件内有效。
        std::wstring SigType(const unsigned char*& cursor, const unsigned char* end) const {
            if (cursor >= end) throw std::runtime_error("程序的类型信息不完整。");
            const unsigned kind = *cursor++;
            switch (kind) {
            case ELEMENT_TYPE_VOID: return L"void";
            case ELEMENT_TYPE_BOOLEAN: return L"bool";
            case ELEMENT_TYPE_CHAR: return L"char";
            case ELEMENT_TYPE_I1: return L"int8";
            case ELEMENT_TYPE_U1: return L"uint8";
            case ELEMENT_TYPE_I2: return L"int16";
            case ELEMENT_TYPE_U2: return L"uint16";
            case ELEMENT_TYPE_I4: return L"int32";
            case ELEMENT_TYPE_U4: return L"uint32";
            case ELEMENT_TYPE_I8: return L"int64";
            case ELEMENT_TYPE_U8: return L"uint64";
            case ELEMENT_TYPE_R4: return L"float32";
            case ELEMENT_TYPE_R8: return L"float64";
            case ELEMENT_TYPE_STRING: return L"string";
            case ELEMENT_TYPE_OBJECT: return L"object";
            case ELEMENT_TYPE_I: return L"native int";
            case ELEMENT_TYPE_U: return L"native uint";
            case ELEMENT_TYPE_CLASS: case ELEMENT_TYPE_VALUETYPE: {
                const ULONG coded = SigNumber(cursor, end); // TypeDefOrRefEncoded 的低两位为表标记。
                constexpr mdToken tables[] = {mdtTypeDef, mdtTypeRef, mdtTypeSpec};
                if ((coded & 3) == 3) throw std::runtime_error("程序元数据中的类型编号无效。");
                return TypeName(TokenFromRid(coded >> 2, tables[coded & 3]));
            }
            case ELEMENT_TYPE_GENERICINST: {
                std::wstring result = SigType(cursor, end); // 后续首先是 class 或 valuetype。
                const ULONG count = SigNumber(cursor, end);
                if (count > 128) throw std::runtime_error("程序的泛型类型参数过多，超出当前支持范围。");
                result += L"<";
                for (ULONG index = 0; index < count; ++index) { if (index) result += L","; result += SigType(cursor, end); }
                return result + L">";
            }
            case ELEMENT_TYPE_VAR: return L"!" + std::to_wstring(SigNumber(cursor, end));
            case ELEMENT_TYPE_MVAR: return L"!!" + std::to_wstring(SigNumber(cursor, end));
            case ELEMENT_TYPE_SZARRAY: return SigType(cursor, end) + L"[]";
            case ELEMENT_TYPE_BYREF: return SigType(cursor, end) + L"&";
            case ELEMENT_TYPE_PTR: return SigType(cursor, end) + L"*";
            case ELEMENT_TYPE_PINNED: return SigType(cursor, end);
            case ELEMENT_TYPE_CMOD_REQD: case ELEMENT_TYPE_CMOD_OPT: SigNumber(cursor, end); return SigType(cursor, end);
            default: throw std::runtime_error("程序使用了暂不支持的类型编码：" + std::to_string(kind));
            }
        }

        // 泛型成员的声明类型必须包含实参，不能把不同实例化上的构造器混为一谈。
        std::wstring OwnerIdentity(mdToken token) const {
            if (TypeFromToken(token) != mdtTypeSpec) return TypeName(token);
            PCCOR_SIGNATURE signature = nullptr; ULONG size = 0;
            if (FAILED(m_metadata->GetTypeSpecFromToken(token, &signature, &size))) return {};
            const auto* cursor = signature;
            return SigType(cursor, signature + size);
        }

        struct MemberSignature {
            bool instance = false; // 签名中是否有隐含 this。
            unsigned genericCount = 0; // 方法自己的泛型参数个数。
            std::wstring result; // 返回类型的语义名称。
            std::vector<std::wstring> parameters; // 按调用顺序排列的参数类型。
        };

        // 对候选成员进行完整参数签名校验，重载新增不会静默匹配到错误方法。
        MemberSignature Signature(mdToken token) const {
            PCCOR_SIGNATURE signature = nullptr; ULONG size = 0; HRESULT status = E_FAIL;
            if (TypeFromToken(token) == mdtMethodDef) status = m_metadata->GetMethodProps(token, nullptr, nullptr, 0, nullptr, nullptr, &signature, &size, nullptr, nullptr);
            if (TypeFromToken(token) == mdtMemberRef) status = m_metadata->GetMemberRefProps(token, nullptr, nullptr, 0, nullptr, &signature, &size);
            if (FAILED(status) || size < 2 || (signature[0] & 0x0f) != IMAGE_CEE_CS_CALLCONV_DEFAULT) throw std::runtime_error("函数的参数或返回类型与预期不同。");
            MemberSignature result; result.instance = (signature[0] & IMAGE_CEE_CS_CALLCONV_HASTHIS) != 0;
            const auto* cursor = signature + 1; const auto* end = signature + size;
            if (signature[0] & IMAGE_CEE_CS_CALLCONV_GENERIC) result.genericCount = SigNumber(cursor, end);
            const ULONG count = SigNumber(cursor, end);
            if (count > 128) throw std::runtime_error("函数参数过多，超出当前支持范围。");
            result.result = SigType(cursor, end);
            for (ULONG index = 0; index < count; ++index) result.parameters.push_back(SigType(cursor, end));
            if (cursor != end) throw std::runtime_error("函数的类型信息包含无法识别的内容。");
            return result;
        }

        struct MemberSymbol {
            mdToken token = 0; // 当前目标文件中的实际成员令牌。
            std::wstring owner; // 包括嵌套类型和泛型实参的声明类型。
            std::wstring name; // 成员原名，编译器生成名称不作为主要特征。
            MemberSignature signature; // 经过解析的完整方法签名。
        };
    private:
        mutable std::vector<MemberSymbol> m_members; // 一次构建，在多条恢复规则中复用。
        mutable bool m_symbolsLoaded = false; // 空表与尚未构建的表需要区分。

    public:
        // 枚举并缓存完整成员语义，特征类只借用该集合。
        const std::vector<MemberSymbol>& Symbols() const { LoadSymbols(); return m_members; }

        // 只读索引所有方法定义和成员引用；无法解析的无关签名不影响候选查找。
        void LoadSymbols() const {
            if (m_symbolsLoaded) return;
            auto add = [this](mdToken token) {
                try { mdToken owner = 0; const auto name = MemberName(token, owner); if (!name.empty()) m_members.push_back({token, OwnerIdentity(owner), name, Signature(token)}); }
                catch (const std::exception&) {} // 字段引用和未使用的复杂签名不属于方法候选。
            };
            MetadataEnumeration types{m_metadata.Get()}; mdTypeDef batch[128]{}; ULONG count = 0;
            while (SUCCEEDED(m_metadata->EnumTypeDefs(&types.handle, batch, 128, &count)) && count) {
                for (ULONG index = 0; index < count; ++index) {
                    MetadataEnumeration methods{m_metadata.Get()}; mdMethodDef items[128]{}; ULONG amount = 0;
                    while (SUCCEEDED(m_metadata->EnumMethods(&methods.handle, batch[index], items, 128, &amount)) && amount) for (ULONG item = 0; item < amount; ++item) add(items[item]);
                }
            }
            auto references = [this, &add](mdToken parent) {
                MetadataEnumeration enumeration{m_metadata.Get()}; mdMemberRef items[128]{}; ULONG amount = 0;
                while (SUCCEEDED(m_metadata->EnumMemberRefs(&enumeration.handle, parent, items, 128, &amount)) && amount) for (ULONG index = 0; index < amount; ++index) add(items[index]);
            };
            // CLR 的父成员枚举按声明类型请求；不依赖未指定父类型时的实现差异。
            MetadataEnumeration refTypes{m_metadata.Get()}, specTypes{m_metadata.Get()}; mdToken parents[128]{};
            while (SUCCEEDED(m_metadata->EnumTypeRefs(&refTypes.handle, parents, 128, &count)) && count) for (ULONG index = 0; index < count; ++index) references(parents[index]);
            while (SUCCEEDED(m_metadata->EnumTypeSpecs(&specTypes.handle, parents, 128, &count)) && count) for (ULONG index = 0; index < count; ++index) references(parents[index]);
            m_symbolsLoaded = true;
        }

        // 根据声明类型、名称、返回值、参数和 this 约定寻找已有成员，不引入版本令牌。
        mdToken Resolve(const std::wstring& owner, const std::wstring& name, const std::wstring& result,
            const std::vector<std::wstring>& parameters, bool instance = false) const {
            LoadSymbols();
            for (const auto& member : m_members) if (member.owner == owner && member.name == name && member.signature.result == result &&
                member.signature.parameters == parameters && member.signature.instance == instance && !member.signature.genericCount) return member.token;
            throw std::runtime_error("缺少修补所需的函数，或函数的参数和返回类型已改变：" + Utf8(owner + L"::" + name));
        }

        // 根据目标文件已有的泛型类型定义构造 TypeSpec；仅写入类型语义，不携带示例文件的令牌。
        Bytes GenericSignature(const std::wstring& definition, bool value_type, const std::vector<Bytes>& arguments) const {
            Bytes result{ELEMENT_TYPE_GENERICINST, static_cast<BYTE>(value_type ? ELEMENT_TYPE_VALUETYPE : ELEMENT_TYPE_CLASS)};
            const mdToken token = ResolveType(definition); BYTE encoded[4]{};
            const ULONG width = CorSigCompressToken(token, encoded); result.insert(result.end(), encoded, encoded + width);
            const ULONG countWidth = CorSigCompressData(static_cast<ULONG>(arguments.size()), encoded); result.insert(result.end(), encoded, encoded + countWidth);
            for (const auto& argument : arguments) result.insert(result.end(), argument.begin(), argument.end());
            return result;
        }

        // 注册一个构造类型签名，元数据发射器会复用已存在的等价 TypeSpec。
        mdTypeSpec TypeSpec(const Bytes& signature) const {
            mdTypeSpec token = 0;
            if (FAILED(m_emitter->GetTokenFromTypeSpec(signature.data(), static_cast<ULONG>(signature.size()), &token))) throw std::runtime_error("无法为补丁添加所需的泛型类型信息。");
            return token;
        }

        // 删除分支时，编译器也可能删去唯一的 MemberRef。复用同一定义其他实例化的开放签名，绑定到当前构造类型。
        mdMemberRef BindGeneric(mdTypeSpec owner_token, const std::wstring& definition, const std::wstring& name,
            const std::wstring& result, const std::vector<std::wstring>& parameters, bool instance) const {
            LoadSymbols();
            for (const auto& member : m_members) {
                if (!member.owner.starts_with(definition + L"<") || member.name != name || member.signature.result != result ||
                    member.signature.parameters != parameters || member.signature.instance != instance || TypeFromToken(member.token) != mdtMemberRef) continue;
                PCCOR_SIGNATURE signature = nullptr; ULONG size = 0; mdMemberRef token = 0;
                if (FAILED(m_metadata->GetMemberRefProps(member.token, nullptr, nullptr, 0, nullptr, &signature, &size)) ||
                    FAILED(m_emitter->DefineMemberRef(owner_token, name.c_str(), signature, size, &token))) throw std::runtime_error("无法连接补丁与所需的泛型函数。");
                return token;
            }
            throw std::runtime_error("无法确认补丁所需泛型函数的参数和返回类型：" + Utf8(definition + L"::" + name));
        }

        // 诊断中可能带有中文或编译器生成名称，统一转换成 UTF-8，避免窄化和控制台乱码。
        static std::string Utf8(std::wstring_view value) {
            const int length = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
            if (length <= 0) return "?";
            std::string result(length, '\0');
            WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), length, nullptr, nullptr); return result;
        }

        // 新字符串进入当前目标的 #US 堆，旧字符串令牌保持不变。
        mdString StringToken(const std::wstring& value) const {
            mdString token = 0;
            if (FAILED(m_emitter->DefineUserString(value.c_str(), static_cast<ULONG>(value.size()), &token))) throw std::runtime_error("无法向补丁中添加所需文字。");
            return token;
        }

        // 读取用户字符串用于语义特征识别。
        std::wstring UserString(mdToken token) const {
            ULONG size = 0;
            if (FAILED(m_metadata->GetUserString(token, nullptr, 0, &size)) || size > 32768) return {};
            std::wstring result(size, L'\0'); ULONG actual = 0;
            if (FAILED(m_metadata->GetUserString(token, result.data(), size, &actual))) return {};
            result.resize(actual); return result;
        }

        // 读取当前函数中各个局部变量的类型，结果按变量编号排列。
        std::vector<std::wstring> Locals(const MethodBody& body) const {
            if (!body.locals) return {};
            PCCOR_SIGNATURE signature = nullptr; ULONG size = 0;
            if (FAILED(m_metadata->GetSigFromToken(body.locals, &signature, &size)) || !size || signature[0] != IMAGE_CEE_CS_CALLCONV_LOCAL_SIG) throw std::runtime_error("函数中的临时变量信息无效。");
            const auto* cursor = signature + 1; const auto* end = signature + size;
            const ULONG count = SigNumber(cursor, end); std::vector<std::wstring> result;
            if (count > 65535) throw std::runtime_error("函数中的临时变量过多，超出当前支持范围。");
            for (ULONG index = 0; index < count; ++index) result.push_back(SigType(cursor, end));
            if (cursor != end) throw std::runtime_error("函数的临时变量信息包含无法识别的内容。");
            return result;
        }

        // 从不同形式的 ldloc/stloc 指令中读取统一的局部变量编号。
        int LocalIndex(const MethodBody& body, const Instruction& instruction, bool store) const {
            const unsigned opcode = instruction.opcode;
            if (store && opcode >= 0x0a && opcode <= 0x0d) return opcode - 0x0a;
            if (!store && opcode >= 0x06 && opcode <= 0x09) return opcode - 0x06;
            if (opcode == (store ? 0x13u : 0x11u)) return Read<unsigned char>(body.offset + instruction.offset + 1);
            if (opcode == (store ? 0xfe0eu : 0xfe0cu)) return Read<WORD>(body.offset + instruction.offset + 2);
            return -1;
        }

        // 从指令读取整数语义，允许编译器选用不同的常量编码。
        std::optional<int> IntegerValue(const MethodBody& body, const Instruction& instruction) const {
            if (instruction.opcode >= 0x15 && instruction.opcode <= 0x1e) return static_cast<int>(instruction.opcode) - 0x16;
            if (instruction.opcode == 0x1f) return Read<int8_t>(body.offset + instruction.offset + 1);
            if (instruction.opcode == 0x20) return static_cast<int32_t>(instruction.token);
            return std::nullopt;
        }

        // 按原始偏移复制指令，通过统一标签重定位所有分支。
        void CopyInstruction(IlAssembler& output, const MethodBody& body, const Instruction& instruction) const {
            output.Mark(static_cast<int>(instruction.offset));
            if (instruction.opcode == 0x45) {
                std::vector<int> targets; for (const auto target : instruction.targets) targets.push_back(static_cast<int>(target)); output.Switch(std::move(targets));
            } else if (!instruction.targets.empty()) output.Branch(instruction.opcode, static_cast<int>(instruction.targets.front()));
            else {
                const size_t width = instruction.opcode > 0xff ? 2 : 1;
                output.Raw(instruction.opcode, {m_bytes.data() + body.offset + instruction.offset + width, instruction.size - width});
            }
        }

        // 重建方法头和异常表。原有 try/finally 的边界与分支使用同一标签表，禁止落入被删的内部指令。
        MethodReplacement Assemble(mdMethodDef method, const MethodBody& body, const IlAssembler& assembler, PatchPointKind kind) const {
            std::map<int, uint32_t> offsets; const auto code = assembler.Build(offsets);
            Bytes result(12, 0); // 统一输出 fat 格式，局部变量仍使用当前程序集的签名。
            const WORD flags = static_cast<WORD>(0x3013 | (body.extraSections ? 8 : 0));
            const WORD stack = (std::max)(body.maxStack, static_cast<WORD>(16));
            const DWORD length = static_cast<DWORD>(code.size());
            std::memcpy(result.data(), &flags, 2); std::memcpy(result.data() + 2, &stack, 2);
            std::memcpy(result.data() + 4, &length, 4); std::memcpy(result.data() + 8, &body.locals, 4);
            result.insert(result.end(), code.begin(), code.end());
            if (body.extraSections) {
                size_t section = (body.offset + body.length + 3) & ~size_t(3); // 源异常节总是四字节对齐。
                std::vector<DWORD> clauses; bool more = true;
                auto relocate = [&offsets](DWORD offset) {
                    const auto found = offsets.find(static_cast<int>(offset));
                    if (found == offsets.end()) throw std::runtime_error("无法为修改后的代码确定异常处理位置。");
                    return found->second;
                };
                while (more) {
                    const BYTE tag = Read<BYTE>(section); more = (tag & 0x80) != 0;
                    if ((tag & 0x3f) != 1) throw std::runtime_error("函数包含暂不支持的附加数据。");
                    const bool fat = (tag & 0x40) != 0;
                    const DWORD size = fat ? (Read<DWORD>(section) >> 8) : Read<BYTE>(section + 1);
                    const size_t width = fat ? 24 : 12;
                    if (size < 4 || (size - 4) % width || section + size > m_bytes.size()) throw std::runtime_error("程序的异常处理信息已损坏，无法安全修补。");
                    for (size_t item = section + 4; item < section + size; item += width) {
                        const DWORD attributes = fat ? Read<DWORD>(item) : Read<WORD>(item);
                        const DWORD first = fat ? Read<DWORD>(item + 4) : Read<WORD>(item + 2);
                        const DWORD firstLength = fat ? Read<DWORD>(item + 8) : Read<BYTE>(item + 4);
                        const DWORD handler = fat ? Read<DWORD>(item + 12) : Read<WORD>(item + 5);
                        const DWORD handlerLength = fat ? Read<DWORD>(item + 16) : Read<BYTE>(item + 7);
                        const DWORD classOrFilter = Read<DWORD>(item + (fat ? 20 : 8));
                        if (first + firstLength > body.length || handler + handlerLength > body.length || !firstLength || !handlerLength) throw std::runtime_error("异常处理位置超出函数代码范围，文件可能已损坏。");
                        clauses.insert(clauses.end(), {attributes, relocate(first), relocate(first + firstLength) - relocate(first),
                            relocate(handler), relocate(handler + handlerLength) - relocate(handler), (attributes & 1) ? relocate(classOrFilter) : classOrFilter});
                    }
                    section = (section + size + 3) & ~size_t(3);
                }
                while (result.size() % 4) result.push_back(0);
                const DWORD header = static_cast<DWORD>((4 + clauses.size() * 4) << 8) | 0x41; // 输出统一 fat EH 表。
                const size_t start = result.size(); result.resize(start + 4 + clauses.size() * 4);
                std::memcpy(result.data() + start, &header, 4); std::memcpy(result.data() + start + 4, clauses.data(), clauses.size() * 4);
            }
            return {method, std::move(result), kind};
        }

        // 在一个类型中按名称查找方法定义，令后续规则与方法令牌无关。
        mdMethodDef FindNamedMethod(mdTypeDef type, const wchar_t* name) const {
            MetadataEnumeration enumeration{m_metadata.Get()};
            mdMethodDef methods[128]{}; ULONG count = 0;
            while (SUCCEEDED(m_metadata->EnumMethods(&enumeration.handle, type, methods, 128, &count)) && count) {
                for (ULONG index = 0; index < count; ++index) {
                    wchar_t methodName[512]{};
                    if (SUCCEEDED(m_metadata->GetMethodProps(methods[index], nullptr, methodName, 512, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr)) &&
                        std::wstring(methodName) == name) return methods[index];
                }
            }
            return 0;
        }

        // 将一字节常量替换为另一个同长度常量，并保存前后字节用于事务校验。
        void AddBytePatch(const MethodBody& body, const Instruction& instruction, unsigned char before, unsigned char after,
            PatchPointKind kind, PatchAnalysis& analysis) const {
            const size_t offset = body.offset + instruction.offset;
            if (instruction.size != 1 || m_bytes[offset] != before) throw std::runtime_error("准备修改的 IL 数值与预期不符，已停止修补。");
            PatchRange range;
            range.offset = offset;
            range.before = {before};
            range.after = {after};
            range.kind = kind;
            analysis.ranges.push_back(std::move(range));
        }

        // 从当前元数据解析完整类型，供 newarr、stelem 等带类型操作数的指令使用。
        mdToken ResolveType(const std::wstring& identity) const {
            MetadataEnumeration defs{m_metadata.Get()}, refs{m_metadata.Get()}, specs{m_metadata.Get()};
            mdToken items[128]{}; ULONG count = 0;
            while (SUCCEEDED(m_metadata->EnumTypeDefs(&defs.handle, items, 128, &count)) && count) for (ULONG index = 0; index < count; ++index) if (TypeName(items[index]) == identity) return items[index];
            while (SUCCEEDED(m_metadata->EnumTypeRefs(&refs.handle, items, 128, &count)) && count) for (ULONG index = 0; index < count; ++index) if (TypeName(items[index]) == identity) return items[index];
            while (SUCCEEDED(m_metadata->EnumTypeSpecs(&specs.handle, items, 128, &count)) && count) {
                for (ULONG index = 0; index < count; ++index) {
                    try { if (OwnerIdentity(items[index]) == identity) return items[index]; }
                    catch (const std::exception&) {} // 不参与恢复的复杂类型不应阻止后续候选。
                }
            }
            throw std::runtime_error("缺少修补所需的类型，此版本的代码可能已改变。");
        }

        // 字段查找同时验证字段类型，拒绝同名但不同用途的成员。
        mdFieldDef ResolveField(mdTypeDef owner, const wchar_t* name, const std::wstring& expected) const {
            MetadataEnumeration enumeration{m_metadata.Get()}; mdFieldDef items[128]{}; ULONG count = 0;
            while (SUCCEEDED(m_metadata->EnumFields(&enumeration.handle, owner, items, 128, &count)) && count) {
                for (ULONG index = 0; index < count; ++index) {
                    wchar_t fieldName[512]{}; PCCOR_SIGNATURE signature = nullptr; ULONG size = 0;
                    if (FAILED(m_metadata->GetFieldProps(items[index], nullptr, fieldName, 512, nullptr, nullptr, &signature, &size, nullptr, nullptr, nullptr)) || fieldName != std::wstring(name) || size < 2) continue;
                    const auto* cursor = signature + 1;
                    if (signature[0] == IMAGE_CEE_CS_CALLCONV_FIELD && SigType(cursor, signature + size) == expected && cursor == signature + size) return items[index];
                }
            }
            throw std::runtime_error("缺少修补所需的变量，或变量类型与预期不同。");
        }

        // 获取一个确切字段的语义类型。
        std::wstring FieldType(mdFieldDef field) const {
            PCCOR_SIGNATURE signature = nullptr; ULONG size = 0;
            if (FAILED(m_metadata->GetFieldProps(field, nullptr, nullptr, 0, nullptr, nullptr, &signature, &size, nullptr, nullptr, nullptr)) || size < 2 || signature[0] != IMAGE_CEE_CS_CALLCONV_FIELD) throw std::runtime_error("程序中的变量类型信息无效。");
            const auto* cursor = signature + 1;
            return SigType(cursor, signature + size);
        }

        // 创建隔离元数据作用域，不将任何程序集加载为可执行代码。
        static ComPtr<IMetaDataImport2> MetadataScope(std::span<const BYTE> bytes, DWORD flags) {
            ComPtr<ICLRMetaHost> host; ComPtr<ICLRRuntimeInfo> runtime; ComPtr<IMetaDataDispenser> dispenser; ComPtr<IMetaDataImport2> result;
            if (bytes.size() > MAXDWORD || FAILED(CLRCreateInstance(CLSID_CLRMetaHost, IID_PPV_ARGS(&host))) || FAILED(host->GetRuntime(L"v4.0.30319", IID_PPV_ARGS(&runtime))) ||
                FAILED(runtime->GetInterface(CLSID_CorMetaDataDispenser, IID_IMetaDataDispenser, reinterpret_cast<void**>(dispenser.GetAddressOf()))) ||
                FAILED(dispenser->OpenScopeOnMemory(bytes.data(), static_cast<ULONG>(bytes.size()), flags, IID_IMetaDataImport2, reinterpret_cast<IUnknown**>(result.GetAddressOf())))) throw std::runtime_error("无法使用 .NET 读取程序的类型和函数信息。");
            return result;
        }

        // 读取 enum 的命名常量，外部依赖与内部枚举共用同一条元数据规则。
        static std::optional<int> EnumLiteral(IMetaDataImport2* metadata, const std::wstring& type, const wchar_t* name) {
            mdTypeDef definition = 0;
            if (FAILED(metadata->FindTypeDefByName(type.c_str(), 0, &definition))) return std::nullopt;
            MetadataEnumeration enumeration{metadata}; mdFieldDef items[64]{}; ULONG count = 0;
            while (SUCCEEDED(metadata->EnumFields(&enumeration.handle, definition, items, 64, &count)) && count) {
                for (ULONG index = 0; index < count; ++index) {
                    wchar_t fieldName[256]{}; DWORD kind = 0, attributes = 0; UVCP_CONSTANT value = nullptr;
                    if (SUCCEEDED(metadata->GetFieldProps(items[index], nullptr, fieldName, 256, nullptr, &attributes, nullptr, nullptr, &kind, &value, nullptr)) &&
                        std::wstring(fieldName) == name && (attributes & fdLiteral) && kind == ELEMENT_TYPE_I4 && value) { int result = 0; std::memcpy(&result, value, 4); return result; }
                }
            }
            return std::nullopt;
        }

        // 从 PE 的托管资源中只读解析嵌入依赖，禁止靠执行程序集来获得枚举值。
        std::optional<int> EmbeddedEnum(const std::wstring& dependency, const std::wstring& type, const wchar_t* name) const {
            ComPtr<IMetaDataAssemblyImport> assemblies;
            if (FAILED(m_metadata->QueryInterface(IID_IMetaDataAssemblyImport, reinterpret_cast<void**>(assemblies.GetAddressOf())))) return std::nullopt;
            const size_t pe = Read<IMAGE_DOS_HEADER>(0).e_lfanew; const size_t optional = pe + 4 + sizeof(IMAGE_FILE_HEADER);
            const size_t directories = optional + (Read<WORD>(optional) == IMAGE_NT_OPTIONAL_HDR64_MAGIC ? offsetof(IMAGE_OPTIONAL_HEADER64, DataDirectory) : offsetof(IMAGE_OPTIONAL_HEADER32, DataDirectory));
            const auto directory = Read<IMAGE_DATA_DIRECTORY>(directories + IMAGE_DIRECTORY_ENTRY_COM_DESCRIPTOR * sizeof(IMAGE_DATA_DIRECTORY));
            const auto clr = Read<IMAGE_COR20_HEADER>(FileOffset(directory.VirtualAddress));
            if (!clr.Resources.Size) return std::nullopt;
            const size_t start = FileOffset(clr.Resources.VirtualAddress); HCORENUM enumeration = nullptr;
            struct ResourceEnumGuard { IMetaDataAssemblyImport* owner; HCORENUM& handle; ~ResourceEnumGuard() { if (handle) owner->CloseEnum(handle); } } guard{assemblies.Get(), enumeration};
            mdManifestResource items[32]{}; ULONG count = 0;
            while (SUCCEEDED(assemblies->EnumManifestResources(&enumeration, items, 32, &count)) && count) {
                for (ULONG index = 0; index < count; ++index) {
                    wchar_t resourceName[512]{}; mdToken implementation = 0; DWORD offset = 0;
                    if (FAILED(assemblies->GetManifestResourceProps(items[index], resourceName, 512, nullptr, &implementation, &offset, nullptr)) || RidFromToken(implementation) != 0 || !std::wstring(resourceName).ends_with(L".resources")) continue;
                    if (offset > clr.Resources.Size || clr.Resources.Size - offset < 4) throw std::runtime_error("内置资源的位置超出文件范围，文件可能已损坏。");
                    const DWORD size = Read<DWORD>(start + offset);
                    if (size > clr.Resources.Size - offset - 4 || start + offset + 4 > m_bytes.size() || size > m_bytes.size() - (start + offset + 4)) throw std::runtime_error("程序内置资源的数据长度无效，文件可能已损坏。");
                    const ManagedResourceReader reader({m_bytes.data() + start + offset + 4, size});
                    const auto content = reader.ByteArray(dependency);
                    if (!content) continue;
                    const auto metadata = MetadataScope(*content, ofRead);
                    return EnumLiteral(metadata.Get(), type, name);
                }
            }
            return std::nullopt;
        }

        // 解析字段类型所在的程序集名称；资源键跟随目标的 AssemblyRef，不假定某版依赖名称。
        std::wstring EnumDependencyName(mdFieldDef field) const {
            PCCOR_SIGNATURE signature = nullptr; ULONG size = 0;
            if (FAILED(m_metadata->GetFieldProps(field, nullptr, nullptr, 0, nullptr, nullptr, &signature, &size, nullptr, nullptr, nullptr)) || size < 3) return {};
            const auto* cursor = signature + 1;
            if (*cursor++ != ELEMENT_TYPE_VALUETYPE) return {};
            const ULONG coded = SigNumber(cursor, signature + size);
            if ((coded & 3) != 1) return {};
            mdToken token = TokenFromRid(coded >> 2, mdtTypeRef); // 枚举类型应直接声明于当前程序集或某个 AssemblyRef。
            for (unsigned depth = 0; depth < 32 && TypeFromToken(token) == mdtTypeRef; ++depth) {
                mdToken parent = 0;
                if (FAILED(m_metadata->GetTypeRefProps(token, &parent, nullptr, 0, nullptr))) return {};
                token = parent;
            }
            if (TypeFromToken(token) != mdtAssemblyRef || !RidFromToken(token)) return {};
            ComPtr<IMetaDataAssemblyImport> assemblies; wchar_t name[512]{};
            if (FAILED(m_metadata->QueryInterface(IID_IMetaDataAssemblyImport, reinterpret_cast<void**>(assemblies.GetAddressOf()))) ||
                FAILED(assemblies->GetAssemblyRefProps(token, nullptr, nullptr, name, 512, nullptr, nullptr, nullptr, nullptr, nullptr))) return {};
            return name;
        }

        // 读取字段原类型后再交给严格查找，兼容枚举从程序集内部移动到依赖程序集的情况。
        std::wstring FieldTypeByName(mdTypeDef owner, const wchar_t* name) const {
            mdFieldDef field = 0;
            if (FAILED(m_metadata->FindField(owner, name, nullptr, 0, &field))) throw std::runtime_error("未找到用于区分资源类别的变量。");
            return FieldType(field);
        }

        // 按 PE 自身的对齐参数向上取整，拒绝溢出和不合法的对齐。
        static DWORD Align(DWORD value, DWORD alignment) {
            if (!alignment || (alignment & (alignment - 1)) || value > MAXDWORD - alignment + 1) throw std::runtime_error("EXE 的 PE 文件对齐信息无效，无法安全添加补丁。");
            return (value + alignment - 1) & ~(alignment - 1);
        }

        // 将重建方法和更新后的元数据追加到新节或安全扩展的末尾节，不挪动原有内容。
        // 输入内存保持只读；所有写入只发生在待验证的候选镜像。
        Bytes BuildCandidate(const PatchAnalysis& analysis) const {
            std::vector<std::pair<size_t, size_t>> occupied; // 已使用的输入字节区间，阻止注册规则相互覆盖。
            std::set<mdMethodDef> rewritten; // 每个方法最多由一个特征完整重建。
            for (const auto& range : analysis.ranges) {
                if (range.before.empty() || range.before.size() != range.after.size() ||
                    range.offset > m_bytes.size() || range.before.size() > m_bytes.size() - range.offset) {
                    throw std::runtime_error("准备修改的位置超出原文件范围，已停止修补。");
                }
                const size_t end = range.offset + range.before.size(); // 边界检查后的区间终点。
                for (const auto& previous : occupied) {
                    if (range.offset < previous.second && previous.first < end) throw std::runtime_error("多个补丁准备修改同一位置，已停止修补。");
                }
                occupied.emplace_back(range.offset, end);
            }
            for (const auto& method : analysis.methods) {
                if (!rewritten.insert(method.method).second) throw std::runtime_error("多个补丁准备替换同一个函数，已停止修补。");
                const auto body = Body(method.method); // 同一方法内不能混用字节修改与整体重建。
                for (const auto& range : occupied) {
                    if (range.first < body.offset + body.length && body.header < range.second) {
                        throw std::runtime_error("多个补丁对同一个函数的修改互相冲突，已停止修补。");
                    }
                }
            }
            Bytes output = m_bytes;
            auto write = [&output]<class T>(size_t offset, const T& value) {
                if (offset > output.size() || sizeof(T) > output.size() - offset) throw std::runtime_error("补丁准备写入的位置超出新文件范围，已停止修补。");
                std::memcpy(output.data() + offset, &value, sizeof(T));
            };
            for (const auto& range : analysis.ranges) {
                if (range.before.size() != range.after.size() || range.offset > output.size() || range.before.size() > output.size() - range.offset ||
                    !std::equal(range.before.begin(), range.before.end(), output.begin() + range.offset)) throw std::runtime_error("准备修改的位置与扫描时不同，已停止修补。");
                std::copy(range.after.begin(), range.after.end(), output.begin() + range.offset);
            }
            const size_t pe = Read<IMAGE_DOS_HEADER>(0).e_lfanew;
            auto file = Read<IMAGE_FILE_HEADER>(pe + 4); const size_t optional = pe + 4 + sizeof(file);
            const WORD magic = Read<WORD>(optional); const bool wide = magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC;
            if (!wide && magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC) throw std::runtime_error("EXE 的 PE 头格式暂不受支持。");
            const size_t directories = optional + (wide ? offsetof(IMAGE_OPTIONAL_HEADER64, DataDirectory) : offsetof(IMAGE_OPTIONAL_HEADER32, DataDirectory));
            const size_t checksum = optional + offsetof(IMAGE_OPTIONAL_HEADER32, CheckSum);
            write(checksum, DWORD{0});
            if (analysis.methods.empty()) return output;
            const DWORD fileAlignment = Read<DWORD>(optional + offsetof(IMAGE_OPTIONAL_HEADER32, FileAlignment));
            const DWORD sectionAlignment = Read<DWORD>(optional + offsetof(IMAGE_OPTIONAL_HEADER32, SectionAlignment));
            const DWORD headersSize = Read<DWORD>(optional + offsetof(IMAGE_OPTIONAL_HEADER32, SizeOfHeaders));
            const size_t sectionTable = optional + file.SizeOfOptionalHeader;
            const size_t newHeader = sectionTable + file.NumberOfSections * sizeof(IMAGE_SECTION_HEADER);
            DWORD firstData = MAXDWORD, virtualEnd = 0;
            for (const auto& section : m_sections) {
                if (section.PointerToRawData) firstData = (std::min)(firstData, section.PointerToRawData);
                const DWORD length = (std::max)(section.Misc.VirtualSize, section.SizeOfRawData);
                if (section.VirtualAddress > MAXDWORD - length) throw std::runtime_error("EXE 中代码或资源的位置数据过大，无法处理。");
                virtualEnd = (std::max)(virtualEnd, section.VirtualAddress + length);
            }
            const bool newSection = file.NumberOfSections < 95 && newHeader + sizeof(IMAGE_SECTION_HEADER) <= firstData &&
                newHeader + sizeof(IMAGE_SECTION_HEADER) <= headersSize && newHeader + sizeof(IMAGE_SECTION_HEADER) <= m_bytes.size() &&
                std::none_of(m_bytes.begin() + newHeader, m_bytes.begin() + newHeader + sizeof(IMAGE_SECTION_HEADER), [](BYTE value) { return value != 0; });
            // 常见 .NET EXE 的头部没有第四个节表项。此时仅扩展同时位于文件和 RVA 末尾的节，避免移动原文件内容。
            const auto last = std::max_element(m_sections.begin(), m_sections.end(), [](const auto& left, const auto& right) { return left.VirtualAddress < right.VirtualAddress; });
            if (last == m_sections.end() || !last->PointerToRawData) throw std::runtime_error("无法在保留原有内容的情况下向 EXE 添加补丁。");
            if (!newSection) for (const auto& section : m_sections) if (section.PointerToRawData > last->PointerToRawData ||
                (section.PointerToRawData != last->PointerToRawData && static_cast<uint64_t>(section.PointerToRawData) + section.SizeOfRawData > last->PointerToRawData)) throw std::runtime_error("EXE 的文件布局暂不受支持，无法安全添加补丁。");
            const auto directory = Read<IMAGE_DATA_DIRECTORY>(directories + IMAGE_DIRECTORY_ENTRY_COM_DESCRIPTOR * sizeof(IMAGE_DATA_DIRECTORY));
            const size_t clrOffset = FileOffset(directory.VirtualAddress); auto clr = Read<IMAGE_COR20_HEADER>(clrOffset);
            if (directory.Size < sizeof(clr) || clr.cb < sizeof(clr) || !(clr.Flags & COMIMAGE_FLAGS_ILONLY) || clr.ManagedNativeHeader.Size || (clr.Flags & COMIMAGE_FLAGS_NATIVE_ENTRYPOINT)) throw std::runtime_error("此文件包含本机代码或预编译代码，当前只支持普通 .NET IL 程序。");
            const DWORD rawOffset = Align(static_cast<DWORD>(m_bytes.size()), fileAlignment);
            if (rawOffset < last->PointerToRawData || static_cast<uint64_t>(last->VirtualAddress) + rawOffset - last->PointerToRawData > MAXDWORD) throw std::runtime_error("补丁代码的位置超出 EXE 支持范围。");
            const DWORD virtualAddress = newSection ? Align(virtualEnd, sectionAlignment) : last->VirtualAddress + rawOffset - last->PointerToRawData;
            Bytes payload;
            for (const auto& method : analysis.methods) {
                while (payload.size() % 4) payload.push_back(0);
                if (payload.size() > MAXDWORD - virtualAddress || FAILED(m_emitter->SetRVA(method.method, virtualAddress + static_cast<DWORD>(payload.size())))) throw std::runtime_error("无法更新补丁函数在 EXE 中的位置。");
                payload.insert(payload.end(), method.body.begin(), method.body.end());
            }
            while (payload.size() % 4) payload.push_back(0);
            const DWORD metadataOffset = static_cast<DWORD>(payload.size()); ULONG metadataSize = 0;
            if (FAILED(m_emitter->GetSaveSize(cssAccurate, &metadataSize)) || metadataSize > MAXDWORD - payload.size()) throw std::runtime_error("无法计算修改后的程序信息大小。");
            payload.resize(payload.size() + metadataSize);
            if (FAILED(m_emitter->SaveToMemory(payload.data() + metadataOffset, metadataSize))) throw std::runtime_error("无法保存修改后的 .NET 程序信息。");
            IMAGE_SECTION_HEADER section = newSection ? IMAGE_SECTION_HEADER{} : *last;
            if (newSection) { std::memcpy(section.Name, ".pclfix", 7); section.VirtualAddress = virtualAddress; section.PointerToRawData = rawOffset; }
            section.Misc.VirtualSize = virtualAddress - section.VirtualAddress + static_cast<DWORD>(payload.size());
            const DWORD oldRawSize = newSection ? 0 : section.SizeOfRawData;
            section.SizeOfRawData = Align(section.Misc.VirtualSize, fileAlignment);
            section.Characteristics = (section.Characteristics & ~IMAGE_SCN_MEM_DISCARDABLE) | IMAGE_SCN_CNT_CODE | IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_EXECUTE;
            if (section.SizeOfRawData > MAXDWORD - rawOffset || section.Misc.VirtualSize > MAXDWORD - virtualAddress) throw std::runtime_error("需要添加的补丁数据过大，无法写入 EXE。");
            output.resize(static_cast<size_t>(section.PointerToRawData) + section.SizeOfRawData, 0); std::copy(payload.begin(), payload.end(), output.begin() + rawOffset);
            write(newSection ? newHeader : sectionTable + static_cast<size_t>(last - m_sections.begin()) * sizeof(IMAGE_SECTION_HEADER), section);
            if (newSection) { ++file.NumberOfSections; write(pe + 4, file); }
            write(optional + offsetof(IMAGE_OPTIONAL_HEADER32, SizeOfImage), Align(section.VirtualAddress + section.Misc.VirtualSize, sectionAlignment));
            write(optional + offsetof(IMAGE_OPTIONAL_HEADER32, SizeOfCode), Read<DWORD>(optional + offsetof(IMAGE_OPTIONAL_HEADER32, SizeOfCode)) + section.SizeOfRawData - oldRawSize);
            clr.MetaData.VirtualAddress = virtualAddress + metadataOffset; clr.MetaData.Size = metadataSize;
            // 修改后的原签名不再有效；保留原字节和备份，但清除签名目录，避免误呈现为有效签名。
            clr.Flags &= ~COMIMAGE_FLAGS_STRONGNAMESIGNED; clr.StrongNameSignature = {};
            write(clrOffset, clr); write(directories + IMAGE_DIRECTORY_ENTRY_SECURITY * sizeof(IMAGE_DATA_DIRECTORY), IMAGE_DATA_DIRECTORY{});
            return output;
        }

        // 创建仅供分析和候选镜像生成的元数据副本；不加载或执行目标程序集。
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
            if (dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew <= 0) throw std::runtime_error("EXE 的 DOS 头无效，文件可能已损坏或不是有效的 EXE。");
            const size_t pe = dos.e_lfanew;
            if (Read<DWORD>(pe) != IMAGE_NT_SIGNATURE) throw std::runtime_error("EXE 的 PE 头无效，文件可能已损坏或不是有效的 EXE。");
            const auto file = Read<IMAGE_FILE_HEADER>(pe + 4);
            const size_t sections = pe + 4 + sizeof(file) + file.SizeOfOptionalHeader;
            for (unsigned index = 0; index < file.NumberOfSections; ++index) m_sections.push_back(Read<IMAGE_SECTION_HEADER>(sections + index * sizeof(IMAGE_SECTION_HEADER)));
            ComPtr<ICLRMetaHost> host; ComPtr<ICLRRuntimeInfo> runtime; ComPtr<IMetaDataDispenser> dispenser;
            if (FAILED(CLRCreateInstance(CLSID_CLRMetaHost, IID_PPV_ARGS(&host))) || FAILED(host->GetRuntime(L"v4.0.30319", IID_PPV_ARGS(&runtime))) ||
                FAILED(runtime->GetInterface(CLSID_CorMetaDataDispenser, IID_IMetaDataDispenser, reinterpret_cast<void**>(dispenser.GetAddressOf()))) ||
                FAILED(dispenser->OpenScopeOnMemory(m_bytes.data(), static_cast<ULONG>(m_bytes.size()), ofRead | ofWrite, IID_IMetaDataImport2, reinterpret_cast<IUnknown**>(m_metadata.GetAddressOf()))) ||
                FAILED(m_metadata->QueryInterface(IID_IMetaDataEmit2, reinterpret_cast<void**>(m_emitter.GetAddressOf())))) throw std::runtime_error("无法读取此 EXE 的 .NET 程序信息，请确认选择的是支持的 PCL 版本。");
        }

    };
}
