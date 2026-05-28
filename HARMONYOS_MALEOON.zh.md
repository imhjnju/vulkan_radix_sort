# HarmonyOS Maleoon 935 Vulkan 适配说明

验证日期：2026-05-28

## 背景

本仓库已使用 HarmonyOS Native SDK 交叉编译，并在搭载 Maleoon 935 Vulkan 设备的 HarmonyOS arm64 手机上完成测试。

原先面向桌面 GPU 的 shader 配置使用 `WORKGROUP_SIZE = 512`。在 Maleoon 935 上，该配置可以成功创建 compute pipeline，但在队列执行阶段会导致 Vulkan device lost。

观察到的失败特征：

```text
vkQueueSubmit result=0
vkWaitForFences wait=-4
```

`-4` 对应 `VK_ERROR_DEVICE_LOST`。同时，GPU 输出的第一个值仍等于未排序的输入值，而 CPU 参考路径可以通过正确性检查。

## 关键兼容性改动

HarmonyOS / Maleoon 兼容配置应使用：

```text
WORKGROUP_SIZE = 256
```

以下文件中的常量必须保持一致：

- `src/shader/upsweep.comp`
- `src/shader/spine.comp`
- `src/shader/downsweep.comp`
- `src/shader/constants.slang`
- `src/vk_radix_sort.h.in`
- 重新生成后的 `include/vk_radix_sort.h`

不要只改其中某一个 shader 文件。Host 侧的 `PARTITION_SIZE` 计算和所有 shader 内部常量必须匹配，否则会出现错误结果或设备丢失。

## benchmark 必需的同步处理

HarmonyOS 设备暴露了 benchmark 中原本隐含的 host/device 同步假设。桌面平台上这些问题可能因为内存类型、驱动实现或提交顺序而不容易复现，但在手机上必须显式处理。

### 1. Host 写入 staging buffer 后必须 flush

`bench/vulkan_benchmark.cc` 使用 mapped staging buffer 把 CPU 生成的输入数据写给 GPU。Host 侧 `std::memcpy()` 只保证数据写进 CPU 可见的映射地址，不保证这些写入立即对 GPU 的 transfer 读取可见。

因此在提交 `vkCmdCopyBuffer(staging -> device buffer)` 之前，需要调用：

```cpp
vmaFlushAllocation(allocator_, staging_.allocation, 0, size);
```

keys-only 路径 flush 的范围是 `inout_size`；key-value 路径 flush 的范围是 `2 * inout_size + sizeof(uint32_t)`，因为同一个 staging buffer 中包含 keys、values，以及 indirect element count。

如果缺少这一步，GPU 可能读到旧数据或未刷新的数据，表现为排序结果和 CPU 参考结果不一致。

### 2. GPU 写回 staging buffer 后必须 invalidate

排序结束后，benchmark 使用 `vkCmdCopyBuffer(device buffer -> staging)` 把结果拷回 mapped staging buffer。即使 GPU 已经写入完成，Host 侧直接 `std::memcpy()` 读取映射地址也不一定能看到最新内容。

因此在 Host 读取 staging buffer 之前，需要调用：

```cpp
vmaInvalidateAllocation(allocator_, staging_.allocation, 0, size);
```

keys-only 路径 invalidate 的范围是 `inout_size`；key-value 路径 invalidate 的范围是 `2 * inout_size`。

如果缺少这一步，CPU 可能读到拷回前的旧内容。调试时曾观察到 GPU 输出的第一个值仍等于原始输入值，这类现象就很容易和缺少 invalidate 或 copy-back 可见性有关。

### 3. 上传 transfer 与 sort dispatch 之间需要 barrier

输入数据先通过 transfer 写入 GPU buffer，随后 compute shader 读取这些 buffer 做排序。即使 copy 和 sort 分别提交并等待 fence，benchmark 仍应明确表达资源访问依赖：transfer write 必须先于 shader read 可见。

sort 前需要的 barrier：

```cpp
VkMemoryBarrier sort_before = {VK_STRUCTURE_TYPE_MEMORY_BARRIER};
sort_before.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
sort_before.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
vkCmdPipelineBarrier(commandBuffer,
                     VK_PIPELINE_STAGE_TRANSFER_BIT,
                     VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                     0, 1, &sort_before, 0, nullptr, 0, nullptr);
```

key-value indirect 路径还会让 sort 命令读取 indirect element count，因此目标 access mask 需要同时覆盖 shader read 和 transfer read：

```cpp
sort_before.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT;
```

### 4. Sort dispatch 与 copy-back transfer 之间需要 barrier

sort dispatch 会通过 compute shader 写入输出 buffer，随后 benchmark 立即用 transfer copy 把结果拷回 staging buffer。这里需要明确表达：shader write 必须先于 transfer read 可见。

copy-back 前需要的 barrier：

```cpp
VkMemoryBarrier sort_after = {VK_STRUCTURE_TYPE_MEMORY_BARRIER};
sort_after.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
sort_after.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
vkCmdPipelineBarrier(commandBuffer,
                     VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                     VK_PIPELINE_STAGE_TRANSFER_BIT,
                     0, 1, &sort_after, 0, nullptr, 0, nullptr);
```

如果缺少这一步，copy-back transfer 可能在 shader 写入对 transfer 可见之前读取 buffer，导致 Host 侧看到未排序或部分更新的数据。

### 5. 这些同步只属于 benchmark 边界

`vk_radix_sort` 库内部已经在 upsweep、spine、downsweep 等 compute pass 之间插入了内部 barrier。这里新增的是 benchmark 在调用库函数前后的外部同步：

- Host staging 写入到 GPU 输入 buffer 的边界。
- GPU sort 输出到 Host staging 读取的边界。

实际集成到其他项目时，也需要在调用 sort 前后根据自己的资源流向添加等价同步；不能假设库函数会替调用者处理所有外部 buffer 的生产者/消费者关系。

这些同步处理已经在 `bench/vulkan_benchmark.cc` 中同时应用到 keys-only 和 key-value 路径。

## 已验证的构建命令

```bash
cmake -S benchmark_repos/vulkan_radix_sort \
  -B benchmark_repos/vulkan_radix_sort/build-ohos-arm64 \
  -DCMAKE_TOOLCHAIN_FILE="$HOME/harmonyos/linux/native/build/cmake/ohos.toolchain.cmake" \
  -DOHOS_ARCH=arm64-v8a \
  -DCMAKE_BUILD_TYPE=Release \
  -DBENCH_ENABLE_EMBREE_CUDA=OFF \
  -DVulkan_INCLUDE_DIR="$HOME/harmonyos/linux/native/sysroot/usr/include" \
  -DVulkan_LIBRARY="$HOME/harmonyos/linux/native/sysroot/usr/lib/aarch64-linux-ohos/libvulkan.so" \
  -DVulkan_GLSLANG_VALIDATOR_EXECUTABLE=/usr/bin/glslangValidator

cmake --build benchmark_repos/vulkan_radix_sort/build-ohos-arm64 --target bench -j
```

## 已验证的部署和运行命令

```bash
$HOME/harmonyos/linux/toolchains/hdc shell "mkdir -p /data/local/tmp/vulkan_radix_sort"
$HOME/harmonyos/linux/toolchains/hdc file send \
  benchmark_repos/vulkan_radix_sort/build-ohos-arm64/bench \
  /data/local/tmp/vulkan_radix_sort/bench
$HOME/harmonyos/linux/toolchains/hdc shell \
  "chmod 755 /data/local/tmp/vulkan_radix_sort/bench"

$HOME/harmonyos/linux/toolchains/hdc shell \
  "cd /data/local/tmp/vulkan_radix_sort && ./bench vulkan /data/local/tmp/vulkan_radix_sort/vulkan_clean_1024.csv 1024"

$HOME/harmonyos/linux/toolchains/hdc shell \
  "cd /data/local/tmp/vulkan_radix_sort && ./bench vulkan /data/local/tmp/vulkan_radix_sort/vulkan_clean_1m.csv 1048576"
```

## 已验证结果

```text
Correctness check passed (N=1024)
keys gpu: 0.019365 ms
kv   gpu: 0.013454 ms

Correctness check passed (N=1048576)
keys gpu: 0.196242 ms
kv   gpu: 0.225817 ms
```

## 回归检查清单

后续如果修改 HarmonyOS / Maleoon 相关逻辑，在认为可用前至少完成以下检查：

1. 使用 HarmonyOS toolchain 重新构建 `bench`。
2. 将二进制推送到 `/data/local/tmp/vulkan_radix_sort/bench`。
3. 运行 `N=1024` 的 Vulkan 正确性检查。
4. 运行 `N=1048576` 的 Vulkan 正确性检查。
5. 确认两次运行都没有 `VK_ERROR_DEVICE_LOST`，也没有 correctness failure。
