# PCL Download Patcher

> PCL II 下载功能修补与恢复工具

鉴于最近的神秘事件，大部分大陆第三方启动器已无法进行原版游戏下载。

![...](/Image/Reported.jpg "")

但你可以使用本工具进行修补，修补后的 PCL 可以正常进行原版游戏下载、整合包下载、游戏修改、资源安装。

本工具为 C++20 开发的 Windows 控制台应用，完全免费开源。

## 修补与兼容范围

目前，正式版与快照版中各有五个修补点，其中整合包下载包含两个点。

程序将解析目标文件的 .NET 元数据，扫描目前四个涉及下载的功能（如果你是来阅读使用教程的请跳过）：

- 下载页面：验证 `LoaderInit` 的关闭任务类型、分支边界和表达式栈，再恢复目标自己的 `DlClientListLoader`。
- 整合包下载：恢复文件选择与后台安装入口，以及 CurseForge、HMCL、MultiMC、MCBBS、Modrinth、启动器打包和普通压缩包的安装分发功能，格式编号提取自目标的归档识别代码。
- Minecraft 修改页：修改 `LoadInstanceFinished` 中的目标卡片显示设置。
- 资源安装入口：恢复“整合包点击安装、按钮另存为；其他资源点击保存”的条件逻辑，资源类型编号读取自目标或其嵌入依赖的枚举元数据。

修补行为是特征扫描，而非使用版本号、哈希或偏移等数据作为依据，所以应当兼容所有**未改变拦截规则**的 PCL 版本。如果后续 PCL 改了字段命名、阻止任务的结构，甚至删除了这些下载页面我也没办法，只能等待更新(更激进的修补)，你可以发 Issue 提醒我啦...

扫描过程中，当程序检测到不兼容将报错并中断任务，如混淆/重命名/异常处理/控制流变化超出规则等。

VB 的常量限制会使编译器删除正常分支，因此恢复整合包将重建方法，将使用目标的闭包、字段、完整方法签名、格式识别分支和各格式安装器生成恢复 IL。

程序**不会**移动原有资源和方法，恢复内容与新元数据将追加到新节或安全扩展的文件末尾节。

开始修补后，程序将创建同目录 `.exe.pclfix.bak` 备份并校验 SHA-256。修补后程序将进行校验，成功校验所有条目且哈希未变才会进行替换。已有备份与当前文件一致时复用，内容不同时保留最初备份，并将修改前状态另存为 `.exe.pclfix.<SHA-256前16位>.bak`。

“还原 PCL” 仍还原最初的 `.pclfix.bak` 并校验哈希。修补行为是二进制修改，这一操作会改变文件哈希。

开始前的扫描会区分需要修补/已恢复/未识别/结构不兼容的特征，存在修补点但缺少必要特征或仅识别到部分正常结构时将拒绝修补，前者直接报错，后者会提示其余功能状态无法确认。

**对于此前只修补下载页面的文件，可以使用本工具的新版本直接升级。**

## 使用

按照引导，选择修补 PCL，程序将打开一个文件选取窗口，选择需要修补的 PCL，程序会自动进行修补并备份，后续可以通过本程序恢复。

`PatchAnalysis` 统一描述等长修改、方法重建和候选镜像；`ManagedPatch.h` 负责语义识别及恢复规则，`ManagedRewrite.h` 负责 IL 汇编与标签重定位，`ManagedResources.h` 只读取内置 ByteArray 资源。新增修补规则可复用同一事务、备份和校验流程。

`PatchTypes.hpp` 定义特征描述、扫描状态和补丁计划；`ManagedPatchContext.hpp` 提供通用元数据、IL 解析和候选 PE 输出能力。五个修补点的具体识别与重建逻辑分别位于 `Features/` 的独立 `.hpp` 子类中，每个子类提供 `Scan`、`Patch`、`Verify`。程序启动时通过 `PatchFeatureRegistry::BuiltIn()` 注册，`ManagedPatch.h` 仅协调扫描与计划合并；新增功能只需实现子类并注册，无需修改中央扫描、验证或控制台类别列表。备份、还原和文件替换仍统一复用原有事务流程。

## 编译

需要 Visual Studio、VS 的桌面 C++ 开发工具与 Windows SDK。项目语言标准为 C++20。

使用 `/utf-8`，控制台通过 `WriteConsoleW` 输出中文，重定向输出为 UTF-8。

```powershell
MSBuild PCL-Download-Patcher.slnx /p:Configuration=Release /p:Platform=x64 /m
```

生成位置：`build/bin/x64/Release/PCL-Download-Patcher.exe`。中间文件统一位于 `build/obj/平台/配置/`，最终程序位于 `build/bin/平台/配置/`。元数据接口依赖 Windows .NET Framework v4。

## 验证

被阻止下载的 PCL 版本通过五处功能修补、重复修补拒绝、字节一致的备份还原、旧补丁升级及不兼容安装器的拒绝行为，无阻止版本被识别为无需修补，保持文件原样。ILDasm 与 PEVerify 检查确认可解析，修补没有增加 IL 校验错误（PCL 原文件存在一个 `LoaderTask` 泛型构造器的 PEVerify 校验错误），修补前后相同。

两组可执行合成程序集改变版本号、元数据令牌、方法位置、资源枚举值及格式编号，修补前后均通过 PEVerify。修补后实际执行测试覆盖正常下载加载器、卡片显示隔离、整合包安装与另存为、普通资源保存、七类归档分发、归档释放、文件选择与取消。它们不能代替历史 PCL 版本覆盖；尚未实测真实 PCL 界面下的联网游戏或整合包安装。

在 VS x64 Native Tools 命令行中运行：

```powershell
New-Item -ItemType Directory tests/output -Force
cl /nologo /std:c++20 /EHsc /O2 /utf-8 /DUNICODE /D_UNICODE tests/Regression.cpp /Fo:tests/output/Regression.obj /Fe:tests/output/Regression.exe
tests/output/Regression.exe tests/output/new-run "待测原始PCL.exe"
```

每次使用新的输出目录。编译 `Regression.exe` 后运行 `tests/RunRecovery.ps1` 可生成两组完整行为样本，并依次执行修补回归、独立 PEVerify 和行为测试；脚本依赖 .NET Framework 编译器和 Windows SDK 的 .NET Framework 4.8 工具。`tests/StructuralFixture.cs` 保留为下载加载器的最小结构样本。所有生成文件位于 `tests/output`。
