# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [1.1.0] - 2026-10-27

### 新增
- 整合包、版本修改、资源安装的下载修补功能，而不是只有原版下载页面。现在你可以在任何地方得到我们应有的启动器体验。
- 对 PCL 2.99.0.1 版本的支持。

### 更改
- 不再因为一处 验证失败/无法验证 阻止修补，而是跳过它们，只修补能够验证确需修补的地方。

### 注意
- 对于此前（v1.0 版本）只修补下载页面的文件，可以使用本工具的新版本直接升级。

### Developer Notes
- 打印控制台日志不再需要手写前缀、指定颜色，而是指定日志级别，内部将自动应用颜色。如果你需要与默认日志级别不同的颜色，可以在可选参数中传入你需要的颜色替代默认。不指定日志级别会得到无前缀打印。
- 修改点已被抽象为 IPatchFeature 类，现在所有修改点子类需要写到 PatchFeatureRegistry.Expanded() 函数中进行注册。