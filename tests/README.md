# 修补回归测试

在项目根目录打开 VS x64 Native Tools 命令行，先创建 `tests/output`。每次测试指定一个新的输出目录，输入的 PCL 文件只读，修改与备份均在输出目录内完成。

## 原有下载功能

```powershell
cl /nologo /std:c++20 /EHsc /O2 /utf-8 /DUNICODE /D_UNICODE tests/Regression.cpp /Fo:tests/output/Regression.obj /Fe:tests/output/Regression.exe
tests/output/Regression.exe tests/output/history-run "原始PCL文件.exe"
./tests/RunRecovery.ps1
```

覆盖修补、重复修补拒绝、备份与逐字节还原、只修补部分功能后的升级、缺少部分结构时跳过该功能并继续修补其他功能。合成程序还会实际执行下载加载器、整合包选择与取消、七类格式安装、归档释放、资源安装与保存等行为。

## 2.99.0.1 新增入口

```powershell
cl /nologo /std:c++20 /EHsc /O2 /utf-8 /DUNICODE /D_UNICODE tests/EntryRegression.cpp /Fo:tests/output/EntryRegression.obj /Fe:tests/output/EntryRegression.exe
./tests/RunEntryRecovery.ps1
```

合成程序实际执行七个入口的恢复逻辑，同时检查隐藏下载页面、临时显示、隐藏版本、未选择游戏和未创建版本列表页面等情况。修补前后均运行 PEVerify。

## 版本选择与结构检查

```powershell
cl /nologo /std:c++20 /EHsc /O2 /utf-8 /DUNICODE /D_UNICODE tests/VersionRegression.cpp /Fo:tests/output/VersionRegression.obj /Fe:tests/output/VersionRegression.exe
tests/output/VersionRegression.exe "原始2.99.0.0.exe" "原始2.99.0.1.exe" tests/output/version-run
```

检查低于引入版本和高于已验证版本时只警告、兼容结构仍可修补，结构不兼容时拒绝写入，以及文件名版本不参与判断。另将任务标题和拦截提示全部改写，验证识别与修补不依赖这些显示文字。

版本规则表额外测试第三套规则的乱序登记、边界选择、版本间隙及旧版和未来版本回退，检查规则描述与功能集合一同被选中。重复编号、重叠范围和无效范围应拒绝登记；修补前后版本或规则编号变化时复检应失败。

已验证历史目录中的 2.99.0.0、2.99.0.1 正式版与快照版，并确认 2.13.1.1 不会被修改。真实 2.99.0.1 文件在补齐内置依赖后，修补前后均只有原文件自带的一项 LoaderTask 泛型构造器 IL 校验错误，没有新增错误。上述测试不能代替真实 PCL 界面中的联网下载和整合包安装，目前尚未完成这些界面操作测试。

额外覆盖扫描失败、补丁生成中途失败、失败功能添加的元数据与字节修改不会写入文件、被跳过函数的原代码保持不变，以及本次已修改功能验证失败时仍拒绝替换。补丁修改冲突仍属于整次修补失败。
