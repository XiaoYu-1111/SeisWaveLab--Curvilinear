# CPU/CUDA 生产求解器基准

## 最新二维FP32/FP64全线程比较（2026-10-29）

[当前报告](../docs/PERFORMANCE_PRECISION_2D.md)使用独立`seis2d_precision_benchmark`，直接调用二维生产API；CPU32线程，GPU为用户开启的厂商性能模式。四档256²/1024²/1536²/2048² × 弹性/3支路GSLS+CFS-PML × CPU/GPU × FP32/FP64 × 5次，共160次，保留资源拒绝，不改旧基准数据。

在外层x64 MSVC环境构建`cmake --build out/build/x64-Release --target seis2d_precision_benchmark --parallel 4`。从仓库根目录执行：

```powershell
python -B benchmarks/precision_suite.py --output <外层out下新的绝对目录>
python -B benchmarks/report_precision.py <同一结果目录>
```

默认输出外层`out/benchmarks/precision_cpu32_gpu_20261029`；重复运行应换新目录，或用`--resume`续跑相同配置。`--sizes 256 1024`可限定规模，完成后`report_precision.py --publish`发布已验收图文。runner要求交流电，核验实际32线程和处理器亲和性；100 ms NVML记录功率、上限、频率、温度与利用率。它尝试最大受支持功率及Windows高性能计划，记录拒绝信息，并在finally查询/尝试恢复原状态；不绕过驱动或内存保护。请在运行前自行开启厂商性能模式。本轮驱动拒绝140 W设置，Windows计划保持平衡，不能将“已开启性能模式”写成“始终消耗140 W”。

所有组合计入同一生产推进范围；场及记录采样检查在计时之后，数据、统计、六组图、Markdown和PDF图件留在外层out。`--publish`仅复制验收所需图文及压缩遥测到docs，完整进程日志与源码快照留本地。默认精度仍为FP64。

## 历史二维／三维FP64基准

`ComputeBenchmark.cpp` 直接调用当前二维、三维生产 API，不修改内核或同步策略。参照 Cartesian 基准采用二维 256²/1024²/2048²、三维 48³/96³/160³，32 个 OpenMP 线程、20 步预热、100 步计时、每后端 5 次重复。输出 60 条原始试验数据和 CPU/CUDA 全场、接收波形误差。

在外层工作区的 MSVC x64 开发者终端运行：

```powershell
cmake --build out/build/x64-Release --target seis_compute_benchmark --parallel 4
$env:OMP_WAIT_POLICY="PASSIVE"
out/bin/Release/seis_compute_benchmark.exe --output <外层out下的绝对结果路径> --threads 32 --repeats 5 --warmup 20 --steps 100
```

可加 `--dimension 2/3 --size N` 缩小范围。报告从内层仓库运行 `python -B benchmarks/report.py <结果目录>` 生成；需 matplotlib、numpy。

计时包含曲网格 RK4、Float64、每步诊断/回滚保护和一个接收点，不包括初始化、重置、输出、绘图或快照下载。CUDA 每步由生产诊断/采样同步完成，不等同于 Cartesian 的 Float32、无接收记录内核基准，不能直接用两项目绝对耗时判定算法优劣。粗网格高频模型用于性能压力测试，并非精度验证。

运行结果默认保存在外层 `out/benchmarks`。按用户要求发布的最新结果见 [性能对比报告](../docs/PERFORMANCE_BENCHMARK.md)，包括单步耗时图、倍速曲线和 60 轮原始 CSV；文档中的图片、数据与源码版本信息均在仓库内，可随 Git 同步。

明确发布时，从内层仓库执行 `python -B benchmarks/publish.py <已完成的结果目录>`。它校验完成标志、复制已复核图表与数据、改写仓库内相对链接，并记录 SHA-256；运行日志、试跑、EXE 和构建缓存留在外层 out。
