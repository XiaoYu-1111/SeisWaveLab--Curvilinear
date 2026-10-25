# Windows Release 发布与重建

更新日期：2026-10-25。项目版本以仓库 CMakeLists.txt 为准。

## 建议上传的附件

| 文件 | 用途 |
| --- | --- |
| SeisWaveLab_Curvilinear-v版本-Windows-x64-Portable.zip | 普通使用者：完整解压即可运行，含 CPU / CUDA 主程序、MASW 工具及运行资源 |
| SeisWaveLab_Curvilinear-v版本-Source.zip | 开发者：最新源码、资源、文档和本次 Windows GLFW / GLEW SDK，可从源码重建 |
| SHA256SUMS.txt | 核对下载文件是否完整 |
| RELEASE_NOTES.md | 可复制为 GitHub Release 正文 |

运行包是免安装便携 ZIP，不是带卸载器的 Setup/MSI。发布无需先制作安装向导；需要组织统一安装时，可再增加安装器。便携包附带 prerequisites/vc_redist.x64.exe，供目标机修复 Microsoft C++ 运行库，打包过程不会执行该安装器。

GitHub 按 Release 标签自动提供的源码 ZIP/TAR.GZ 只包含该提交的仓库内容；完整 Source.zip 另带 Windows SDK 快照和构建入口。发布前先提交本次代码、资源与发布工具，推送提交和版本标签，再上传与该版本匹配的附件。源码包记录基准提交、工作区快照摘要和每个文件的 SHA-256，不能用旧标签冒充已修改的源码。

## 运行包内容与要求

- 主程序 SeisWaveLab_Curvilinear.exe，辅助工具 SeisWaveLab_MASW.exe。
- GLFW、GLEW、CUDA cudart 运行时，以及来自 VS Redist 的 x64 C++ / OpenMP 运行库。
- shaders、examples/elevation、docs、LICENSE、licenses 和第三方声明。
- Start.cmd / Start-2D.cmd / Start-3D.cmd，以及 portable.flag。
- BUILD_INFO.json、FILE_MANIFEST.json 和使用说明。

目标系统为 Windows x64，显卡驱动须支持 OpenGL 4.3。CPU 运行不需要 NVIDIA GPU 或 CUDA Toolkit；CUDA 加速需要兼容 NVIDIA 驱动和足够显存。已编入 sm_75、sm_86、sm_89、sm_120 及 compute_75 PTX，实际兼容性仍需目标设备验证。不要将 Windows 的 OpenGL/显卡驱动 DLL 从开发机复制给其他用户。

便携包将偏好和默认输出写入 EXE 旁边的 userdata。portable.flag 优先于开发目录识别，因而在当前工作区的 out 子目录中测试便携包，也不会写回开发数据目录。标准构建树的 out/bin/Release 和 out/bin/Debug 仍使用其对应的外层 out；普通移动副本则使用旁边的 userdata。请解压到可写文件夹。

## 源码与构建

完整源码包保留当前版本的代码、CMake、图标、着色器、示例、文档、Notebook、测试与第三方源码 / 许可；不包含 .git 历史、.vs 缓存、out、中间产物、Python 虚拟环境和个人模拟结果。

安装 VS 2022 / Build Tools 的 C++ 桌面组件、Windows SDK、CMake / Ninja 后，运行源码包根目录 Build-Windows.cmd，默认构建 CPU 版本。CUDA 版本还需 CUDA Toolkit 12.8+，使用 Build-Windows.cmd cuda。辅助脚本从 vswhere 定位 VS 2022 工具链，并使用包内 dependencies/windows 的相对路径，不依赖原开发机 D 盘。建议解压到 C:\SeisWave 等短路径，避免 MSVC 的临时编译文件超长；同时安装 VS 2026 时也不会自动选中尚未验证的 CUDA 工具链。

不使用完整源码包时，可安装官方 GLFW 3.4 / GLEW 2.1.0 SDK，并为 tools/build_windows_release.cmd 的第二个参数提供含 glfw、glew 子目录的路径；或依照 [构建说明](BUILDING.md) 设置 SEIS3D_GLFW_ROOT / SEIS3D_GLEW_ROOT。构建始终写入源码父目录的 out。

运行 GUI 不需要 Python。Notebook 与绘图可按 tools/requirements.txt、tools/dem_profiles/requirements.txt 和 tools/seismic_plotting/requirements.txt 安装各自依赖，不应打包整个开发机 Python 环境。

## 打包与验证

在外层工作区运行 Python -B 和 tools/package_release.py，显式指定新的 out/releases 子目录以及 GLFW、GLEW、VS Redist、CUDA 根目录。工具先产生暂存目录，核对 PE 导入依赖与文件清单；验证完成后以 --finalize 生成 ZIP 和 SHA256SUMS.txt。已有目录不会被删除或覆盖。

至少检查：将运行包复制到含空格 / 中文的新目录，在移除开发工具路径的环境中执行主页、二维和三维冒烟检查；确认输出落在新目录 userdata。关闭 CUDA 可见设备后检查 CPU 路径；使用全新源码目录和随包 SDK 进行 CPU Release 重建。公开附件不包含验证产生的 userdata、out、日志或开发机偏好。

本机迁移和禁用 CUDA 的检查不能替代干净 Windows 虚拟机、不同 GPU 和双显示器实机验收。已知数值及格式边界继续以 [当前验证说明](NUMERICS_AND_VALIDATION.md) 和 [三维外部 DEM](EXTERNAL_DEM_3D.md) 为准。

参考：[GitHub Release 与自动源码附件](https://docs.github.com/en/repositories/releasing-projects-on-github/about-releases)、[Microsoft C++ 运行库](https://learn.microsoft.com/en-us/cpp/windows/latest-supported-vc-redist)、[GLFW](https://www.glfw.org/download.html)、[GLEW](https://glew.sourceforge.net/)。
