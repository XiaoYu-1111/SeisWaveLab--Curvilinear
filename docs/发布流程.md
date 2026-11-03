# 版本、发布包与重建

当前软件版本只在根目录[VERSION](../VERSION)定义，[CHANGELOG](../CHANGELOG.md)保留历史变更。旧实验日期、数据schema和旧Release资产保持原样。

从[GitHub Releases](https://github.com/XiaoYu-1111/SeisWaveLab--Curvilinear/releases)下载对应版本的Windows-x64-Portable.zip，完整解压后运行Start.cmd或主程序。包含运行库、着色器、图标、示例、文档和许可；不要只复制EXE。CPU不要求NVIDIA显卡，CUDA加速需要兼容驱动，界面要求OpenGL 4.3。偏好和结果写入旁边的userdata。

Source.zip提供同一提交的完整源码、资源和GLFW/GLEW构建SDK。按包内README_重新构建.md执行Build-Windows.cmd，默认构建CPU；Build-Windows.cmd cuda需自行安装兼容CUDA Toolkit。MSVC、Windows SDK、CUDA Toolkit和Python环境不随包分发。GitHub自动生成的源码ZIP不包含额外构建SDK，需要SDK时使用项目上传的Source.zip。

SHA256SUMS.txt校验两个ZIP；包内FILE_MANIFEST.json记录逐文件摘要，BUILD_INFO.json记录版本、提交、源码指纹、配置和主程序摘要。便携包免安装，prerequisites下是可选Microsoft运行库安装程序。

版本统一规则：内外层CMake通过cmake/Version.cmake读取VERSION；Windows六个运行EXE的文件版本、界面标题和两个GUI的--version使用构建生成的版本；发布与文档工具共用tools/project_version.py。修改VERSION后必须重新构建，不能仅改旧ZIP名称。

发布流程：更新版本与CHANGELOG，完成构建和回归；提交源码并对干净提交创建同名标签；用tools/package_release.py准备Portable/Source，审计DLL并测试便携启动，再用--finalize生成ZIP和SHA256；推送提交和标签，创建GitHub Release草稿、上传校验资产后发布。不覆盖历史标签和旧版本资产。

所有构建、发布包和测试日志都位于外层out，不提交到源码仓库。完整包与Release始终对应同一个干净提交。
