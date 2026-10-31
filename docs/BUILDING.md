# 构建、设备选择与程序分发

## 二维、三维和共享模块

程序默认打开主页，左侧为二维曲面模拟，右侧为三维曲面模拟；`SeisWaveLab_Curvilinear.exe --2d` / `--3d` 可直接进入。三维旧命令行与截图参数仍由三维工作区处理。二维操作和五场理论见[二维工作流](TWO_D_WORKFLOW.md)。

`src/two_d`、`src/three_d` 分别包含本维度的 CPU/CUDA 求解、验证和 ui 子目录；`src/common` 放设备服务，`src/common/ui` 放主页、应用图形生命周期和语言主题。公共头分别在 `include/Seis2D`、`include/Seis3D`、`include/SeisCommon`，include 通过对应 CMake 目标传播，源码不再留有旧位置副本。

`seis2d_curvilinear_core`、`seis3d_curvilinear_core` 分别链接共享 `seis_compute`；二维核心不链接三维核心。启用 CUDA 时分别链接 `seis2d_cuda`、`seis3d_cuda`，设备探测由 `seis_compute_cuda` 提供。`seis2d_ui`、`seis3d_ui` 链接本维度核心及 `seis_ui_common`，最终组合为 `SeisWaveLab_Curvilinear.exe`。VS 中各目标按真实源码目录分组。关闭 CUDA 时两个核心均可使用 CPU；`seis2d_validation` 验证二维方程及后端一致性。

二维新增 `seis2d_acoustic_core`、`seis2d_acoustic_cuda`、`seis2d_imaging_core` 和对应两个独立UI目标。RTM单向依赖声学核；弹性核心不依赖它们；通用图形运行时不依赖二维模型交换服务。显式交换由应用层 `seis2d_model_exchange` 提供，目录和验收入口见[二维模块架构](MODULE_ARCHITECTURE_2D.md)。

`seis_records` 提供独立 SEG-Y 读写，不依赖 CUDA 或图形上下文；`seis_record_io_validation` 检查数据交换。二维/三维共享的 `RecordView` 位于 `src/common/ui`，三维 `RecordView3D` 是兼容包装。SEG-Y 模块采用标准库实现，不增加外部 SEG-Y SDK 依赖。导入文件及导出限制见[记录与 SEG-Y](SEGY_AND_RECORDS.md)。

## 独立仓库与外部输出

包含本文件、`CMakeLists.txt`、`src` 和 `include` 的目录就是 Git 仓库根目录。在 VS 中直接打开这一层。构建不再依赖外层工作区的配置、图标、许可或原始参考仓库；外层 CMake 仅保留为本机兼容包装入口。

`cmake/Targets.cmake` 定义各目标，`cmake/BuildLayout.cmake` 统一外部输出。所有生成路径相对于仓库根目录：

| 内容 | 路径 |
| --- | --- |
| Release 缓存与中间文件 | `../out/build/x64-Release` |
| Debug 缓存与中间文件 | `../out/build/x64-Debug` |
| EXE、DLL、着色器和许可 | `../out/bin/<配置>` |
| 静态库 | `../out/lib/<配置>` |
| 安装预留位置 | `../out/install/<预设>` |
| 离线说明及博客发布包 | `../out/documentation`、`../out/blog` |

CMake 会拒绝在指定外层 out 以外创建构建树。仓库重命名不改变“父目录/out”的规则；同一父目录中的多个副本会共享该目录，因此不同副本应放在不同父目录下。

## Visual Studio 与命令行

需要 CMake 3.25+、C++20、MSVC x64/Windows SDK 和 Ninja。图形工作台需要 OpenGL 4.3、GLFW/GLEW SDK；通过 `SEIS3D_GLFW_ROOT`、`SEIS3D_GLEW_ROOT` 设置本机路径，现有 D 盘默认值仅适用于原开发机。默认预设启用 CUDA；Toolkit 须兼容所选 MSVC 工具集。VS 直接打开本仓库，选择 `x64-Release` / `x64-Debug`，将 `SeisWaveLab_Curvilinear` 设为启动目标。

本项目直接查找 CUDA、OpenGL 和本机 GLFW/GLEW SDK，预设和 CMakeSettings 显式将 `CMAKE_TOOLCHAIN_FILE` 置空，避免 VS 自动注入本机 vcpkg 工具链。修改工具链后需在 VS 中“删除缓存并重新配置”；若 CUDA 已安装却报告找不到 cudart DLL，或缺少 `CUDA::cudart` / `OpenGL::GL`，请检查缓存是否仍加载了旧工具链。

以下命令在本仓库根目录的 MSVC x64 开发者终端执行：

```powershell
cmake --preset x64-Release
cmake --build --preset x64-Release --parallel
ctest --preset x64-Release
..\out\bin\Release\SeisWaveLab_Curvilinear.exe
```

Debug 使用对应的 `x64-Debug` 预设。旧版 VS 可使用仓库内 `CMakeSettings.json`，输出路径一致；支持预设的版本优先使用 `CMakePresets.json`。个人 SDK 位置或长期选项放在不提交的 `CMakeUserPresets.json` 中，继承现有预设并覆盖变量。

无 CUDA Toolkit 时运行 `cmake --preset x64-Release -DSEIS3D_ENABLE_CUDA=OFF`；无图形 SDK 时再加 `-DSEIS3D_BUILD_GUI=OFF`，仍可构建 CPU 命令行和数值验证。重新配置预设会恢复其中显式设置的值。Linux GUI 使用系统 OpenGL、glfw3、GLEW 包；当前实际构建验证以 Windows 为主。

从旧外层入口迁移时，缓存中的 `CMAKE_HOME_DIRECTORY` 不同。先将原 Release/Debug 构建树移至外层 out 的备份目录，再从本仓库重新配置；保留现有 EXE 和模拟数据。不要对同一个缓存交替使用外层与仓库两个入口。

## 日常运行构建与Release目录清理

日常更新运行程序可使用以下构建预设，避免重新生成全部开发验证工具：

```powershell
cmake --build --preset x64-Release-runtime --parallel
```

该预设保留主界面、MASW工具、二维常规/Q值/黄土先导实验入口及三维命令行入口，共6个EXE。DLL、examples、shaders、licenses仍位于原运行目录。`x64-Release`全量构建仍保留全部验证/基准目标；清理测试EXE后，执行CTest前应先全量构建或构建对应测试目标。

`tools/clean_release_executables.ps1`默认只预览；加`-Apply`仅删除Release目录下已识别的开发/测试EXE，不递归删除文件，不修改DLL和用户数据，未识别程序保留。2026-10-30清理39个开发EXE，释放约37.08 MiB；逐文件SHA-256及保留清单记录在外层`out/maintenance/release-executable-cleanup.json`。完整源码和资源保留，开发程序可重建。

## CUDA 架构

默认列表为 `75-real;86-real;89-real;120-real;75-virtual`，完整列表要求 CUDA Toolkit 12.8 或支持这些目标的后续版本。

| GPU 系列 | 原生架构 | 示例 |
| --- | --- | --- |
| RTX 20 | sm_75 | RTX 2060、2070、2080 |
| RTX 30 | sm_86 | RTX 3060、3070、3080 |
| RTX 40 | sm_89 | RTX 4060、4070、4080 |
| RTX 50 | sm_120 | RTX 5060、5070、5080 |

`-real` 编入对应机器码，`75-virtual` 额外保留 compute_75 PTX，供兼容驱动即时编译。较早 Toolkit 应缩减到其支持的架构。仅在本机运行可覆盖 `-DCMAKE_CUDA_ARCHITECTURES=native`；多值参数需整体加引号，例如 `"-DCMAKE_CUDA_ARCHITECTURES=75-real;89-real;75-virtual"`。架构兼容不替代驱动和显存要求。

## 图标、参考验证与输出资源

默认图标原件在仓库 `resources/icons/seis3d.ico`，可通过 `SEIS3D_APP_ICON` 覆盖。PNG 和 64 px 预览同目录保存。构建生成的 RC 与上游参考头位于外部构建目录的 `generated` 子目录，不再需要原有 `out/assets/icons` 副本。图标缺失时警告并省略嵌入，不使整个 GUI 构建失败。

ImGui/ImPlot 源码及许可保存在 `third_party`；公共头文件通过目标 include 路径导出，仍使用 `#include "Seis3D/..."`。`third_party/cgfd3d_reference` 保留直接 RHS 验证所需的三个原始上游文件及来源、摘要与许可，原始外层 CGFD3D 仓库不再是构建前提。Python 可用时生成对照头并注册对应测试，不可用时只省略该验证目标。分析与文档依赖见 `tools/requirements.txt`。

程序从 EXE 同级 `shaders` 读取着色器，不依赖当前工作目录。DLL、着色器和许可在构建后复制到外层 bin。MSVC/Ninja 的中文头文件依赖探测逻辑保留在仓库 cmake 模块中。

示例高程原件在 `resources/examples/elevation`；CMake 自动复制到 EXE 同级 `examples/elevation`。二维高程弹窗按 EXE 位置查找这些数据，不依赖原开发机源码路径。发送程序时也保留 `examples` 目录，其中 README 说明覆盖范围与推荐网格。

## 显卡检测与求解设备

程序枚举 CUDA 可见的所有设备，逐卡读取设备编号、型号、计算能力、PCI 地址和显存，并在该设备上检查当前二进制的数值内核。第 0 块设备不可用时，仍继续检查后续设备；失败原因保留在列表中。Windows 同时通过 DXGI 列出硬件显示适配器，因而核显、AMD 显卡与 NVIDIA 显卡都能在信息区看到，CUDA 求解器只接受通过检查的 NVIDIA CUDA 设备。操作系统、驱动或环境变量隐藏的设备不会出现在 CUDA 可见列表。

进入“运行 → 性能对比 → 计算后端”，选择 CPU/CUDA，再选择具体 CUDA 设备。默认“自动”选择总显存最大的可用设备，总显存相同时选择枚举中较早者；根据总量而非瞬时空闲量选择可避免重建时由于旧会话占用显存而反复换卡。“刷新显卡设备”更新清单；手动选择后点击“应用/重建”生效，从 t=0 开始。显存预检和实际分配针对同一张已解析的设备；显存不足时报告错误并保留原会话，不自动改用另一张卡或 CPU。显卡编号属于当前运行时枚举结果，因此不跨计算机保存编号。

设备扫描、显存查询、步进、接收采样、切片回传、快照导出和缓冲区释放均使用明确的 CUDA 设备上下文。刷新列表不会迁移正在运行的计算。一次模拟仍在一块卡上执行，尚未实现跨卡区域分解。渲染使用 OpenGL 上下文所属显卡，求解使用选定 CUDA 卡，通过主机切片传递数据，二者可以不同。

发送运行程序时保留 EXE 同级的 CUDA/GLFW/GLEW DLL、`shaders` 和 `licenses`，并在目标机安装相应 MSVC 运行库及兼容的 NVIDIA 驱动；不要求安装整个 CUDA Toolkit。RTX 2060 等较小显存设备需按剩余显存降低模型尺寸，架构兼容并不增加显存容量。

`curvilinear_device_selection` 验证完整扫描后的选择策略、显式指定设备、逐卡 CPU/CUDA 对照、扫描与读回交错以及失败切卡保留会话。只有检测到至少两块可用 CUDA 设备时才能实测跨卡上下文；单卡测试不能代替多卡硬件验收。界面检查可运行 `../out/bin/Release/SeisWaveLab_Curvilinear.exe --smoke-devices`。

架构与构建语义参考：[NVIDIA CUDA GPU 计算能力](https://developer.nvidia.com/cuda/gpus)、[CMake 3.29 CUDA_ARCHITECTURES](https://cmake.org/cmake/help/v3.29/prop_tgt/CUDA_ARCHITECTURES.html)。

参考：[Visual Studio CMake 预设](https://learn.microsoft.com/en-us/cpp/build/cmake-presets-vs)、[CMake MSVC 调试信息设置](https://cmake.org/cmake/help/latest/variable/CMAKE_MSVC_DEBUG_INFORMATION_FORMAT.html)。

## 分发边界

发送 Release 程序时保留同级 DLL、`shaders`、`licenses`；目标机安装相应 MSVC 运行库，支持 OpenGL 4.3，CUDA 还需兼容的 NVIDIA 驱动。无需安装完整 CUDA Toolkit。较小显存卡需要相应降低模型尺寸。

运行时数据目录已支持迁移：标准 out/bin/<配置> 构建使用其对应的外层 out；EXE 旁存在 portable.flag 时优先使用旁边的 userdata；移出构建树的普通副本也使用旁边的 userdata。二维、三维及 MASW 共用该规则。请将便携包完整解压到可写目录。完整运行包、源码包和发布步骤见 [Release 发布与重建](RELEASE_DISTRIBUTION.md)。

## 项目标识与目录迁移

产品显示名为 **SeisWave Lab · Curvilinear**，本地工作区为 `SeisWaveLab_Curvilinear_Workspace`，独立 Git 仓库和 GUI CMake 目标均为 `SeisWaveLab_Curvilinear`；主程序为 `../out/bin/<配置>/SeisWaveLab_Curvilinear.exe`。二维、三维数值命名空间、库名、验证程序及 `SEIS3D_*` 选项保持原接口，模型和记录格式保持兼容。

迁移开发工作区时，应整体移动 `.git`、源码、资源和同级 `out`；模拟数据与 DEM 工作流继续位于外层 `out`。CMake 缓存保留旧绝对路径，迁移后仍需从新仓库目录重新配置并构建 Release/Debug。分发运行程序不需要开发缓存或 .git，完整便携包可直接移动。原缓存和旧主程序可留在外层 `out/archive`，不加入源码仓库。VS 请关闭原文件夹，重新打开新仓库目录并选择新的 GUI 启动目标。

本地改名不修改 GitHub 上的仓库名或 `origin`。如另行在 GitHub 更名，确认新地址已存在后再执行 `git remote set-url origin <新地址>`；当前已有文章的 `seis3d-curvilinear` 页面路径及附件目录保留，避免已发布链接失效。
