# 维护与文档工具

`python -B tools/report_acoustic_cpml.py --publish`：从原生六组参考域对照生成CPML吸收报告图、CSV和验证日志；仅通过完整验收才发布。方法和边界见[声学CPML](../docs/ACOUSTIC_CPML_2D.md)。

`report_acoustic_validation.py --publish`发布声学直达波、CPU/CUDA与精度验证、短步大网格容量日志及声学正演工作区截图；证据及模式选择见[二维三种正演](../docs/ACOUSTIC_FORWARD_2D.md)。完整原始波场和记录仍在外层out，短时成本样本不作为通用倍速结论。

`report_rtm_validation.py`读取外层`out/validation/rtm-2d`的原生验证导出，绘制真实/背景速度、RTM和照明；`--publish`将审阅图件、完整窗口截图和数值日志更新到[二维RTM说明](../docs/RTM_2D.md)所用资源，完整模拟数组仍留在out。

从 Git 仓库根目录运行。所有工具的默认输出均为仓库外的 `../out`，请使用 `python -B` 避免产生仓库内 Python 缓存。Python 分析及文档依赖由 `requirements.txt` 列出；C++ 模拟器不依赖这些分析包。

| 命令/脚本 | 用途与输出 |
| --- | --- |
| `python -B tools/publish_module_ui.py` | 完成声学/RTM界面与模块隔离验收后，无损发布十八张完整窗口截图及隔离/声学交互日志，记录尺寸、主题和SHA-256；见[模块组织](../docs/MODULE_ARCHITECTURE_2D.md) |
| `python -B tools/review_loess_pilot.py --root <已有先导目录> --output <外层out下的检查目录>` | 核查完成标志、原始记录、快照/探针和SHA-256，输出600 dpi PNG/PDF及检查报告；模型turbo、记录gray、波场coolwarm+detail_symlog。不启动模拟 |
| `python -B tools/run_loess_pilot.py --prepare --root ../out/research/loess-apparent-attenuation/SESSION` | 准备黄土沟谷先导配置和源码摘要；加独立的`--run`步骤启动有限验收/14组先导队列，详见[先导实现](../docs/LOESS_SURFACE_ATTENUATION_PILOT.md) |
| `./tools/clean_release_executables.ps1`，确认范围后加 `-Apply` | 预览/清理Release目录已识别的开发EXE，保留6个运行/实验程序、依赖和数据；SHA-256清单在外层out/maintenance；日常构建可用x64-Release-runtime |
| `python -B tools/check_repository.py` | 只读检查文档本地链接、必需源资源、外部构建预设和误放的生成文件 |
| `python -B tools/organize_outputs.py` | 预览外层 out 中原始 PPM 截图；加 `--apply` 压缩归档、逐文件 SHA-256 校验后移除原始帧，保留恢复清单和用户数据校验值 |
| `python -B tools/package_documentation.py` | 从 docs 正文和发表图片生成离线说明与 GitHub Pages/Hexo 包；输出到 `../out/documentation`、`../out/blog` |
| `python -B tools/analyze_dispersion.py` | 均匀内点 P/S 波频散与振幅分析；输出到 `../out/validation/dispersion` |
| `python -B tools/analyze_cpml_validation.py` | 分析 `seis2d_cpml_validation --audit` 的原始记录；输出七组边界图、摘要和中文验收报告到 `../out/validation/cfs-pml-2d`，见[方法及复现](../docs/CFS_PML_2D.md) |
| `python -B tools/analyze_precision_validation.py` | 汇总`seis2d_precision_validation`及`--benchmark`/`--capacity`的误差、速度和显存；图文报告在`../out/validation/precision-2d`，`--publish`更新验收后的文档资源；见[二维精度](../docs/PRECISION_2D.md) |
| `python -B tools/analyze_cfl_symbol.py` | 交替方向八步 Fourier 周期审计；输出到 `../out/validation/cfl_symbol.json` |
| `python -B tools/verify_exports.py` | 验证已有导出；默认读取 `../out`，可传显式目录 |
| `generate_upstream_reference.py` | 由 CMake 调用，将仓库内三个原始参考文件的选定函数提取到外部构建目录 |

CPU/CUDA性能测试见[benchmarks/README.md](../benchmarks/README.md)：最新二维CPU32线程/GPU、FP32/FP64四档比较使用`seis2d_precision_benchmark`、`precision_suite.py`及`report_precision.py`，记录功耗与资源拒绝；旧二维/三维FP64目标`seis_compute_benchmark`继续保留。所有原始数据和图文写入外层`out/benchmarks`。

数值分析脚本各有明确的内点与参数假设，结果不能代替自由表面、吸收区和复杂介质的完整精度验证。更新发表图片需经过人工核对，再更新 docs/images 中的 PNG 与 capture.tsv；打包工具不会自动用外层原始截图覆盖仓库资源。


## 试验声学DRP核复现

- `run_acoustic_drp_suite.py --run --references`：顺序执行15/30 Hz低速干黄土局部对照，已完成组保留，所有原始数据位于外层out。
- `report_acoustic_drp.py`：绘制解析/吸收验收、未对齐原始压力误差、计算成本及数组内存。需激活包含numpy/matplotlib的环境。
- [实现及验收范围](../docs/ACOUSTIC_DRP_2D.md)：当前不满足5%复杂模型误差门槛，不发布等误差加速结论。
