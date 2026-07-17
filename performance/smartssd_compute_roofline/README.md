# SmartSSD FPGA 峰值算力测试

该示例测量 SmartSSD FPGA kernel 的单精度浮点峰值算力，用作 Roofline
模型的计算上限。默认配置是 320 条并行 lane；每条 lane 每拍执行
`x = x * alpha + beta`，按一次乘法加一次加法，即 2 FLOP 统计。

每次迭代都用整数位操作构造一个不同的正规浮点输入，浮点结果被折叠进片上整数
checksum。这样所有结果都对最终输出可见，又不存在浮点反馈依赖，主循环可以达到
II=1。320 lanes 默认拆成20个独立的16-lane流水线，使循环控制和寄存器使能保持在
局部区域，避免单一流水线产生扇出数千的全局控制网络。seed 和最终 checksum 均
通过 AXI-Lite 控制寄存器传递，设计中没有全局内存
端口及 AXI memory interconnect，因此计时结果不受外部 DDR 带宽影响。
辅助 XOR/AND 不计入 FLOP 数。

SmartSSD shell 的动态区域只有 1344 个可放置 DSP。kernel 显式将每条 lane 的
FP32 乘法映射到3个DSP。默认让8个16-lane分组的FP32加法使用DSP，其余12组
加法使用 LUT fabric；320 lanes 的 kernel 预计使用 `960 + 128*2 = 1216` 个
DSP，不会触发动态区域的DSP pblock上限，同时降低全fabric加法造成的LUT布线
压力，并保持每 lane 每拍一次乘加。

## 构建和运行

先加载对应版本的 Vitis/XRT 环境，然后执行：

```bash
cd performance/smartssd_compute_roofline
make PLATFORM=xilinx_u2_gen3x4_xdma_gc_2_202110_1 \
     LANES=320 GROUP_LANES=16 DSP_ADD_GROUPS=8 \
     MEM_TAG=bank0 FREQ_MHZ=300

make run PLATFORM=xilinx_u2_gen3x4_xdma_gc_2_202110_1 \
     LANES=320 GROUP_LANES=16 DSP_ADD_GROUPS=8 \
     MEM_TAG=bank0 FREQ_MHZ=300
```

也可以直接运行 host：

```bash
./smartssd_peak_compute \
  -x build_dir.hw.xilinx_u2_gen3x4_xdma_gc_2_202110_1.l320.g16.da8.mbank0.f300/peak_compute.xclbin \
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
251.1 MHz，因此只能达到约160 GFLOP/s。register-output 实现移除该模块后达到
265.9 MHz，但单一320-lane流水线又产生了扇出为7360的循环使能路径；当前版本
将其拆成20个独立的16-lane流水线以降低控制扇出。

## 确认真正达到峰值

硬件构建完成后运行 `make report ...` 找到综合和实现报告，并检查：

- `compute` 循环的 achieved II 必须为 1。
- 实现后的 kernel 时钟必须达到 `FREQ_MHZ`，不能只看 HLS 估计时钟。
- 浮点乘法器/加法器数量应随 `LANES` 线性增加；默认320/16/8配置应使用约
  1216个kernel DSP，其中960用于乘法、256用于128路DSP加法，其余加法在fabric。
- 若实际频率下降，应在288、304、320、336、352、368等配置间扫描，而不是只追求
  300 MHz。以 `2 * LANES * 实际频率 / II` 最大的可实现配置作为 Roofline计算屋顶。
- `GROUP_LANES` 只改变控制网络的分组，不改变 FLOP 数。默认先测试16；若仍存在
  高扇出控制路径，可比较8、16、32，选择 routed timing 最好的配置。
- `DSP_ADD_GROUPS` 控制多少个分组的加法使用DSP。对于320/16配置，建议先比较
  0、4、8、10；数值10预计使用1280个kernel DSP，继续增大可能接近1344个
  pblock站点上限。最终选择应以布局后的频率而不是DSP利用率百分比为准。
- 还可以用 `IMPL_STRATEGIES=Performance_WLBlockPlacementFanoutOpt` 单独尝试 Vivado
  的高扇出布局优化策略。策略名会写入构建目录，避免复用其他实现结果。

只有实现报告确认 achieved clock 为 300 MHz，并且实测接近 192 GFLOP/s 时，才能
说明该配置下调度和计时开销已经很小。需要注意，这里得到的是已实现 kernel 的
有效峰值，不是仅依据器件 DSP 总数计算的纸面峰值。host 输出的
`vs requested roof` 是相对请求频率计算的比例；若工具发生自动降频，它不代表
计算流水线的实际利用率。

## 仿真

功能检查可使用较小配置，避免 320 lane 使仿真和编译过慢：

```bash
make TARGET=hw_emu LANES=32 GROUP_LANES=16 DSP_ADD_GROUPS=1 MEM_TAG=bank0
make run TARGET=hw_emu LANES=32 GROUP_LANES=16 DSP_ADD_GROUPS=1 MEM_TAG=bank0
```

host 会先执行短 warm-up，并与 CPU 结果比较；通过后才开始正式计时。
