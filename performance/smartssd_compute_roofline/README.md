# SmartSSD FPGA 峰值算力测试

该示例测量 SmartSSD FPGA kernel 的单精度浮点峰值算力，用作 Roofline
模型的计算上限。默认配置是 320 条并行 lane；每条 lane 每拍执行
`x = x * alpha + beta`，按一次乘法加一次加法，即 2 FLOP 统计。

每次迭代都用整数位操作构造一个不同的正规浮点输入，浮点结果被折叠进片上整数
checksum。这样所有结果都对最终输出可见，又不存在浮点反馈依赖，主循环可以达到
II=1。seed 和最终 checksum 均通过 AXI-Lite 控制寄存器传递，设计中没有全局内存
端口及 AXI memory interconnect，因此计时结果不受外部 DDR 带宽影响。
辅助 XOR/AND 不计入 FLOP 数。

SmartSSD shell 的动态区域只有 1344 个可放置 DSP。kernel 显式将每条 lane 的
FP32 乘法映射到 3 个 DSP、将 FP32 加法映射到 LUT fabric；因此 320 lanes 的
kernel 约使用 960 个 DSP，不会触发动态区域的 DSP pblock 上限，同时仍能保持
每 lane 每拍一次乘加。

## 构建和运行

先加载对应版本的 Vitis/XRT 环境，然后执行：

```bash
cd performance/smartssd_compute_roofline
make PLATFORM=xilinx_u2_gen3x4_xdma_gc_2_202110_1 \
     LANES=320 MEM_TAG=bank0 FREQ_MHZ=300

make run PLATFORM=xilinx_u2_gen3x4_xdma_gc_2_202110_1 \
     LANES=320 MEM_TAG=bank0 FREQ_MHZ=300
```

也可以直接运行 host：

```bash
./smartssd_peak_compute \
  -x build_dir.hw.xilinx_u2_gen3x4_xdma_gc_2_202110_1.l320.mbank0.f300.regout/peak_compute.xclbin \
  --iterations 100000000 --repeats 3
```

320 lane、300 MHz、主循环 II=1 时，上限为：

```text
320 lane * 1 mul-add pair/lane/cycle * 2 FLOP/pair * 300 MHz = 192 GFLOP/s
```

host 报告的是 kernel `start()` 到 `wait()` 的墙钟时间，包含很小的调度和固定
装载/回写开销，因此测量值应略低于该上限。默认每次执行 1 亿次迭代，使固定
开销可以忽略。host 还会根据 `iterations / time` 输出推算的有效 kernel 时钟；
如果它明显低于 `FREQ_MHZ`，应检查链接报告中是否发生了 auto frequency scaling。

旧版 memory-port 实现请求300 MHz时，SmartSSD 实现报告出现 WNS=-0.648 ns，
最差路径位于自动生成的 AXI memory interconnect，Vitis 将实际时钟自动降至
251.1 MHz，因此只能达到约160 GFLOP/s。当前 register-output 实现移除了该模块。

## 确认真正达到峰值

硬件构建完成后运行 `make report ...` 找到综合和实现报告，并检查：

- `compute` 循环的 achieved II 必须为 1。
- 实现后的 kernel 时钟必须达到 `FREQ_MHZ`，不能只看 HLS 估计时钟。
- 浮点乘法器/加法器数量应随 `LANES` 线性增加；320 lanes 的乘法器应使用约
  960 个 DSP，加法器的实现类型应为 `fabric`/`nodsp`。
- 若布局布线失败或实际频率下降，应减小 `LANES`；建议在 288、304、320、336
  等配置间扫描，以 `2 * LANES * 实际频率 / II` 最大的可实现配置作为 Roofline
  的计算屋顶。

只有实现报告确认 achieved clock 为 300 MHz，并且实测接近 192 GFLOP/s 时，才能
说明该配置下调度和计时开销已经很小。需要注意，这里得到的是已实现 kernel 的
有效峰值，不是仅依据器件 DSP 总数计算的纸面峰值。host 输出的
`vs requested roof` 是相对请求频率计算的比例；若工具发生自动降频，它不代表
计算流水线的实际利用率。

## 仿真

功能检查可使用较小配置，避免 320 lane 使仿真和编译过慢：

```bash
make TARGET=hw_emu LANES=4 MEM_TAG=bank0
make run TARGET=hw_emu LANES=4 MEM_TAG=bank0
```

host 会先执行短 warm-up，并与 CPU 结果比较；通过后才开始正式计时。
