# SpaceScanner

基于 NTFS MFT 的磁盘空间分析工具（WPF + C++ 引擎）。

## 下载已编译版本

- Windows 可执行文件（v1.0.0）：<https://github.com/MiracleNan/spacescanner/releases/tag/v1.0.0>
- 进入页面后在 `Assets` 中下载对应的 `.exe` 文件

## 功能

- Treemap 可视化：按占用空间比例显示文件夹/文件，快速定位大文件与热点目录
- 目录交互：
  - 单击目录可展开/折叠当前层级
  - 双击目录可钻取进入该目录视图
  - 双击当前视图根节点可返回上一级
- 右键操作：
  - 对目录支持“在资源管理器中打开”
  - 对文件支持“打开（默认）”和“打开所在的文件夹”
- 路径能力：支持从可视化块反推完整路径并直接调用系统资源管理器
- 扫描引擎：C++ 引擎读取 NTFS 元数据，C# UI 负责解析与可视化渲染
- 运行模式可见：状态栏显示当前扫描模式
  - `DataRuns`：高速路径
  - `RecordIoctlFallback`：兼容回退路径（更慢）

## 效果展示

<p align="center">
  <img src="docs/assets/demo.gif" alt="SpaceScanner Demo" width="920" />
</p>

<table>
  <tr>
    <td align="center" width="50%">
      <img src="docs/assets/ui-overview.png" alt="UI Overview" />
      <br />
      <sub><b>主界面</b></sub>
    </td>
    <td align="center" width="50%">
      <img src="docs/assets/scan-result.png" alt="Scan Result" />
      <br />
      <sub><b>扫描结果</b></sub>
    </td>
  </tr>
</table>

## 环境要求

- Windows 10/11
- Visual Studio 2022
- 工作负载：
  - `Desktop development with C++`
  - `.NET desktop development`
- .NET SDK 8

## 快速构建（推荐）

在仓库根目录执行：

```powershell
.\publish.ps1 -Configuration Release -Runtime win-x64 -Deployment framework-dependent
```

输出目录：

`SpaceScannerUI\publish\win-x64\`

说明：

- `framework-dependent`：不打包 runtime，默认输出单文件 exe；目标机器需已安装 .NET Desktop Runtime 8。
- `self-contained`：打包 runtime，输出单文件 exe，体积会明显增大；目标机器无需预装 .NET。

## 在 Visual Studio 构建

1. 打开 `SpacescannerPro.sln`
2. 选择 `Release | x64`
3. 执行 `Build Solution`

说明：

- 解决方案已配置先编译 `MFT`，再编译 UI
- 生成的 `MftEngine.exe` 会自动放到 `SpaceScannerUI` 并作为嵌入资源打包

## 运行说明

- 必须以管理员身份运行 `spacescanner.exe`（读取 NTFS 元数据需要权限）
- 仅支持扫描 NTFS 分区

## 常见问题

- 扫描慢：观察状态栏模式
  - `DataRuns`：正常高速
  - `RecordIoctlFallback`：兼容模式，速度会明显降低
- 路径打开失败：确认当前扫描盘符和管理员权限

## 目录结构

```text
.
├─ MftEngine/                 # C++ 扫描引擎
├─ SpaceScannerUI/            # WPF 可视化界面
├─ SpacescannerPro.sln        # 解决方案
└─ build.ps1                  # 构建脚本
```
