# Git 仓库与本地输出的边界

当前 Git 根目录是 `SeisWaveLab_Curvilinear_Workspace/SeisWaveLab_Curvilinear` 中的内层目录。本文件、README、构建配置、源码和说明图片均在该仓库内部；外层目录只是本机工作区。克隆到其他位置时，只需当前仓库及已安装的编译工具、图形 SDK 和可选 CUDA，不需要原开发机外层文件。

## 应纳入仓库

| 路径 | 用途 |
| --- | --- |
| `CMakeLists.txt`、`CMakePresets.json`、`CMakeSettings.json`、`cmake` | 独立构建入口和外部输出布局 |
| `src`、`include` | CPU/CUDA、界面、I/O 与验证代码 |
| `src/common`、`src/two_d`、`src/three_d` | 共享基础服务、独立二维与三维实现；各维度含自己的 ui |
| `src/two_d/acoustic`、`src/two_d/imaging`、`src/two_d/ui/acoustic`、`src/two_d/ui/imaging` | 声学核心、RTM任务与各自界面；原弹性核心保留原目录 |
| `resources/icons`、`resources/AppIcon.rc.in`、`shaders` | 应用原始资源 |
| `resources/icons/regular` | 规则网格图标原图及多尺寸 ICO，保留设计资产供 Git 同步 |
| `docs`、`docs/images` | 学术说明、操作说明和发表图片 |
| `docs/PERFORMANCE_BENCHMARK.md`、`docs/data/performance-2026-10-19`、`docs/images/performance` | 按用户要求发布的性能实测报告、原始 CSV、版本信息与图表快照；运行中间输出仍在外层 out |
| `tools` | 分析、导出验证、参考提取及博客打包工具 |
| `third_party` | 已使用的库、必要参考源码及来源说明 |
| `LICENSE`、`THIRD_PARTY_NOTICES.md` | 项目许可与第三方来源 |
| `VERSION`、`cmake/Version.cmake`、`CHANGELOG.md` | 单一软件版本来源、构建读取入口与历史变更 |
| `.gitignore`、`.gitattributes`、`AGENTS.md` | 仓库维护约定 |

上游参考目录只保存直接 RHS 验证所需的三个原文件及许可，不复制整个原始仓库。应用图标属于源资源，故保存在仓库；原来位于外层 `out/assets/icons` 的副本不再是构建前提。

## 留在仓库外

所有构建和运行输出写入仓库父目录的 `out`，从仓库查看即 `../out`：构建缓存、对象文件、EXE/DLL/LIB/PDB、生成的 RC 和参考头、模拟 VTS/CSV、UI 偏好、原始 PPM、分析结果、文档/博客发布目录及 ZIP。个人 SDK 设置放在被忽略的 `CMakeUserPresets.json`。

博客发表内容由仓库内 `docs/PROJECT_MANUAL.md` 及图片生成，发布包无需重复提交到源码仓库。完整图文在 GitHub 上可通过 README 和文档导航阅读。

外层输出按用途整理：当前缓存保留在 `build/x64-Release`、`build/x64-Debug`，界面检查位于 `validation/ui`，二维完整截图位于 `screenshots/two-d-workflow`，三维完整截图位于 `screenshots/curvilinear-workflow`，数值数据位于 `validation`，用户二维导出位于 `2d`。旧独立构建副本和未压缩截图可清理；不要将其回填到源码目录。说明和博客只使用已复核的仓库内 PNG。

`.gitignore` 提供误操作保护，不能取消已经跟踪的文件。提交前可用 `git status --short`、`git diff --stat` 和 `git ls-files` 检查清单，确认没有新加入的二进制、缓存、个人路径或 Git 凭据。本次整理不自动提交或推送。

## 打开与构建

VS 直接打开本仓库。从仓库根目录运行 `cmake --preset x64-Release`、`cmake --build --preset x64-Release`，输出仍是外层 `../out/build/x64-Release` 和 `../out/bin/Release`。Debug 规则相同。本机外层 CMake 包装入口保留；同一个缓存只能使用一个源码入口，推荐始终使用仓库入口。

进一步说明见[构建文档](BUILDING.md)、[文档与博客生成](README.md)。
