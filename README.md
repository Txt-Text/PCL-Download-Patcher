# PCL Download Patcher

鉴于最近的神秘善意举报事件，大部分大陆第三方启动器已无法进行原版游戏下载。

![...](/Image/Reported.jpg "")

所以，你可以使用本工具进行修补！修补后的 PCL 可以正常进行原版游戏的下载。

C++20 / Windows 控制台工具。本项目完全免费开源。

选择“修补 PCL”后直接打开系统文件选择窗口，备份与还原也通过同一窗口选择目标，不扫描任何目录。选择后才检查 PE、可选校验和和下载限制结构；取消选择则返回菜单。

## 修补与兼容范围

修补规则不包含版本号、哈希白名单、固定偏移或固定元数据令牌。

程序只读解析目标文件的 .NET 元数据，查找下载页面 `LoaderInit` 中传给 `PageLoaderInit` 的“关闭下载”任务，校验加载器类型、分支边界和表达式栈结构，再等长替换为现有 `ModDownload.DlClientListLoader` 字段引用，其余空间填充 NOP。

适用于保留这一下载页面、字段命名与限制任务结构的版本。程序集重新编译、令牌和地址变化不需要逐版本适配。无法保证所有未来版本：混淆、重命名、异常处理或控制流变化超出规则时会报错并停止。`RestrictMcDownload`、`SecretCdnSign` 字符串仅用于界面提示，不作为修补的必要条件。常量元数据本身不作修改，CDN 签名和账号登录逻辑不作修改。

确认后创建同目录 `.exe.pclfix.bak` 并校验 SHA-256。修补临时文件通过结构复检且目标哈希未变后，再替换目标文件。已有备份仅在与当前文件完全一致时复用，否则拒绝覆盖。备份还原后再次校验内容一致性。二进制修改会改变原文件哈希及原有签名的有效性。

## 使用

按照引导，选择修补 PCL，程序将打开一个文件选取窗口，选择需要修补的 PCL，程序会自动进行修补并备份，后续可以通过本程序恢复。

## 编译

Visual Studio 的桌面 C++ 工作负载与 Windows SDK，C++20。工程使用 `/utf-8`，控制台通过 `WriteConsoleW` 输出中文，重定向输出为 UTF-8。

```powershell
MSBuild PCL-Download-Patcher.slnx /p:Configuration=Release /p:Platform=x64 /m
```

生成位置：`x64/Release/PCL-Download-Patcher.exe`。元数据接口依赖 Windows .NET Framework v4；运行时不会执行待修补的程序集。

## 验证

两个提供的受限样本均通过修补、重复修补拒绝、备份还原及 SHA-256 一致性回归；原始样本未改动。独立 ILDasm 确认快照的加载器绑定已恢复。

合成程序集覆盖不同版本号、元数据令牌和方法位置，修补后实际执行正常加载器。它们用于结构回归，不能代替历史 PCL 版本覆盖。尚未实测 PCL 联网下载游戏。

在 VS x64 Native Tools 命令行中运行回归（只修改输出目录中的副本）：

```powershell
New-Item -ItemType Directory tests/output -Force
cl /nologo /std:c++20 /EHsc /O2 /utf-8 /DUNICODE /D_UNICODE tests/Regression.cpp /Fo:tests/output/Regression.obj /Fe:tests/output/Regression.exe
tests/output/Regression.exe tests/output/new-run "待测原始PCL.exe"
```

每次使用新的输出目录。`tests/StructuralFixture.cs` 可用 .NET Framework 的 `csc.exe` 编译；加 `/define:REORDER` 生成令牌和方法位置变化的第二个样本。
