# Downsweep 优化与稳定性经验

最后验证日期：2026-06-07

本文记录 downsweep 优化过程、发现的同步问题、性能取舍，以及 HarmonyOS 和通用 Vulkan
环境的回归方法。

## 修改概览

本轮修改不只涉及 downsweep：

- 将 workgroup 大小调整为 `256`，并保持 Host 与所有 shader 的分区常量一致。
- compute shader 使用 `maxElementCount` 限制 indirect element count，避免越界 dispatch。
- 设备支持时，通过 subgroup size control 优先请求 subgroup size `32`。
- 重构 downsweep 的本地 radix histogram 和 prefix 计算。
- 保持 GLSL 与 Slang shader 行为一致。
- 使用 `VRDX_PROFILE` 增加分阶段 GPU 时间统计。
- Vulkan benchmark 支持零长度输入。
- 将无限 fence 等待改成可配置超时。
- 增加边界长度和极端 key 分布的重复稳定性测试。
- 重新生成 `include/vk_radix_sort.h` 单头文件。

## 五轮优化结论

每个候选方案都同时比较 keys-only 和 key-value，不能只因为某个局部阶段变快就接受。

1. 混合 key-value scatter 在部分测量中更快，但不能稳定复现。
2. 全 direct scatter 更慢。
3. 减小 key-value partition division 能改善局部 downsweep，但 keys-only 回退超过可接受范围。
4. Padded histogram 没有可测量的收益。
5. Subgroup specialization 初测更快，但最终 baseline A/B 复测没有确认收益。

因此丢弃了无法稳定复现的候选方案。最终实现优先保证跨驱动正确性和可重复性能。

## 同步问题与规则

### Barrier 必须一致到达

workgroup barrier 之前不能出现 invocation-dependent 的提前 `return`。workgroup 中的所有线程必须
一起执行 barrier，或者一致地跳过包含 barrier 的整个区域。

当前 compute shader 使用 workgroup-uniform 条件：

```glsl
if (partitionStart < elementCount) {
    // 所有 workgroup barrier 均位于一致分支内。
}
```

同一 workgroup 内的 `partitionStart` 和 `elementCount` 相同。不能将 barrier 放入
`keyIndex < elementCount` 之类的逐线程条件中。

### 非一致 subgroup broadcast 不可移植

优化过程中曾让不同 lane 使用不同的 `subgroupBroadcast` source lane。不能假设这种用法在所有
Vulkan 驱动上都有效，可能产生错误结果、`VK_ERROR_DEVICE_LOST` 或表面挂死。

最终实现使用 subgroup barrier 和 shared-memory atomic 完成有序的逐 slot histogram 更新。
同步有成本，但行为明确。

### 跨 subgroup 共享内存必须同步

每个 invocation 写入一个 `localHistogramSum` 条目后，后续代码会根据任意 radix 读取其他条目，
读取目标可能由另一个 subgroup 写入。因此写入完成和首次任意索引读取之间必须有 workgroup
barrier。

缺少 barrier 时在某个 GPU 上通过测试，并不能证明代码正确；驱动调度顺序可能暂时隐藏数据竞争。

### 什么时候可以删除 barrier

只有能证明以下任一条件时才删除 barrier：

- producer 和 consumer 是同一个 invocation。
- 通信只发生在同一个 subgroup，且 subgroup primitive 已提供需要的执行和内存顺序。
- 数据已移入寄存器，不再通过 shared memory 交换。
- 算法重写后 producer/consumer 依赖已经不存在。

不能因为重复测试暂时通过就删除 barrier。必须先分析 barrier 两侧每个共享地址的写入者、读取者和
可见性。

## 挂死检测

benchmark 不再永久等待 fence，默认超时为 30 秒：

```bash
VRDX_FENCE_TIMEOUT_MS=30000 ./build/bench vulkan results.csv 1048576
```

可通过 `VRDX_FENCE_TIMEOUT_MS` 调整。失败信息会区分 upload、sort 和 readback 阶段。该机制能把
永久等待转换成可诊断失败，但不能恢复已经丢失的 Vulkan device。

## 性能分析

启用分阶段时间统计：

```bash
VRDX_PROFILE=1 ./build/bench vulkan results.csv 1048576
```

输出包含 transfer、upsweep、spine、downsweep 和 finalize 累计时间。分阶段数据用于定位回退，
最终是否接受优化仍以总排序时间为准。把 downsweep 工作转移到其他 pass 不属于端到端优化。

处理 GPU 测量噪声时：

1. 保持输入尺寸和构建配置一致。
2. 先 warmup，使频率和 pipeline 状态稳定。
3. 比较中位数，不比较最好的一次。
4. 所有候选测试完成后重新运行原始 baseline。
5. 最终直接 A/B 中不能复现的收益应当放弃。

## 极端输入稳定性测试

通过 `VRDX_STRESS_RUNS` 重复运行正确性和挂死测试：

```bash
VRDX_STRESS_RUNS=5 \
VRDX_FENCE_TIMEOUT_MS=30000 \
./build/bench vulkan stress.csv
```

压力测试会将 keys-only 和稳定 key-value 排序与 CPU 结果比较，覆盖：

- 空输入。
- subgroup、workgroup、partition 和较大分配边界前后的长度。
- 全零和全 `UINT32_MAX`。
- 升序和降序。
- 零与 `UINT32_MAX` 交替。
- 大量重复值的 2-bit、8-bit 低熵输入。
- 固定 seed 的随机输入。

## Subgroup 与连续提交测试

新增环境变量：

```bash
VRDX_SUBGROUP_SIZE=auto|native|32|64
VRDX_SUBMISSION_STRESS_RUNS=5
VRDX_SUBMISSION_STRESS_BATCH=32
```

每轮 submission stress 对 keys-only 和稳定 key-value 分别测试：

1. 同一个 command buffer 内连续录制多次 sort。
2. 多个 command buffer 放在同一个 `vkQueueSubmit` 中。
3. 多次调用 `vkQueueSubmit`，中间不等待 fence，只在最后一次 submit 等待。

连续 sort 之间显式加入 compute shader write 到后续 transfer/compute read-write 的 barrier。
这是合法的调用方式测试，不测试调用方故意遗漏外部同步的未定义行为。

在目标 HarmonyOS 设备上，应分别验证驱动选择的 native subgroup 路径和设备支持的显式 subgroup
配置。不能仅根据查询到的 subgroup 属性推断 shader 正确性，必须运行完整的 keys/KV、边界输入和
连续提交测试。

## 构建与校验

### Ubuntu native

```bash
cmake -S . -B build-native -DCMAKE_BUILD_TYPE=Release
cmake --build build-native -j
```

### Windows 交叉编译

使用已配置的 MinGW build 目录：

```bash
cmake --build build-windows -j
```

Windows 的应用控制策略可能阻止新生成的可执行文件。`Unblock-File` 只能移除 downloaded-file
标记，不能绕过强制 Device Guard 策略。

### HarmonyOS arm64

toolchain、部署和真机检查参见
[HARMONYOS_MALEOON.zh.md](HARMONYOS_MALEOON.zh.md)。

```bash
cmake --build build-ohos-arm64 -j
```

### SPIR-V 校验

shader 修改后至少校验两种 downsweep：

```bash
glslangValidator -V -Os --target-env spirv1.5 \
  -o /tmp/downsweep.spv src/shader/downsweep.comp
glslangValidator -V --target-env spirv1.5 -DKEY_VALUE \
  -o /tmp/downsweep-kv.spv src/shader/downsweep.comp

spirv-val --target-env vulkan1.2 /tmp/downsweep.spv
spirv-val --target-env vulkan1.2 /tmp/downsweep-kv.spv
```

最后执行：

```bash
git diff --check
```

## 最终经验

- Barrier 数量影响性能，但缺少必要 barrier 是正确性问题，不是优化。
- Subgroup 行为必须符合 Vulkan/SPIR-V 约定，不能依赖单一驱动的表现。
- 查询 subgroup 属性不能替代对应 pipeline 配置的实际正确性测试。
- 低熵和大量重复 key 特别适合暴露 histogram 竞争和顺序问题。
- 零长度和各种边界长度应成为固定回归用例。
- GPU 测试必须设置有限的 Host 等待时间，避免真正的永久挂死。
- 连续 command buffer 和无中间 fence 的多 submit 是必要的同步回归场景。
- GPU 时钟和驱动状态会污染初期数据，候选方案必须经过最终 baseline 复测。
- 编译通过、SPIR-V 合法、结果正确、长期稳定和性能提升是五个独立检查项。
