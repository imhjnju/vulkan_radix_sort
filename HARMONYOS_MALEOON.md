# HarmonyOS Maleoon 935 Vulkan Notes

Date verified: 2026-05-28

## Context

This repo was cross-compiled with the HarmonyOS native SDK and tested on a HarmonyOS arm64 phone with a Maleoon 935 Vulkan device.

The original desktop-oriented shader configuration used `WORKGROUP_SIZE = 512`. On Maleoon 935, that configuration created compute pipelines successfully but lost the Vulkan device during queue execution.

Observed failure signature:

```text
vkQueueSubmit result=0
vkWaitForFences wait=-4
```

`-4` is `VK_ERROR_DEVICE_LOST`. The first GPU output value also stayed equal to the unsorted input value, while the CPU reference path passed.

## Key compatibility change

Use `WORKGROUP_SIZE = 256` for HarmonyOS/Maleoon compatibility.

Files that must stay consistent:

- `src/shader/upsweep.comp`
- `src/shader/spine.comp`
- `src/shader/downsweep.comp`
- `src/shader/constants.slang`
- `src/vk_radix_sort.h.in`
- regenerated `include/vk_radix_sort.h`

Do not change only one shader file. The host-side `PARTITION_SIZE` calculation and all shader-local constants must match.

## Required synchronization for the benchmark

The HarmonyOS device exposed host/device synchronization assumptions that were implicit in the benchmark. Desktop platforms may hide these issues because of memory type choices, driver behavior, or queue submission behavior, but the mobile path needs them to be explicit.

### 1. Flush after host writes to the staging buffer

`bench/vulkan_benchmark.cc` uses a mapped staging buffer to pass CPU-generated input data to the GPU. The host-side `std::memcpy()` only writes to the CPU-visible mapped address. It does not guarantee that those writes are immediately visible to GPU transfer reads.

Before submitting `vkCmdCopyBuffer(staging -> device buffer)`, call:

```cpp
vmaFlushAllocation(allocator_, staging_.allocation, 0, size);
```

The keys-only path flushes `inout_size`. The key-value path flushes `2 * inout_size + sizeof(uint32_t)`, because the same staging buffer contains keys, values, and the indirect element count.

Without this flush, the GPU can read stale or unflushed input data, which shows up as output mismatches against the CPU reference.

### 2. Invalidate before host reads from the staging buffer

After sorting, the benchmark uses `vkCmdCopyBuffer(device buffer -> staging)` to copy results back into the mapped staging buffer. Even after the GPU write has completed, directly reading the mapped address with `std::memcpy()` does not guarantee the host sees the latest contents.

Before the host reads the staging buffer, call:

```cpp
vmaInvalidateAllocation(allocator_, staging_.allocation, 0, size);
```

The keys-only path invalidates `inout_size`. The key-value path invalidates `2 * inout_size`.

Without this invalidate, the CPU can read old contents from before the copy-back. During debugging, the first GPU output value was observed to remain equal to the original unsorted input value, which is the kind of symptom missing invalidate or copy-back visibility can produce.

### 3. Add a barrier between upload transfer and sort dispatch

Input data is first written into GPU buffers by a transfer copy, then read by compute shaders during sorting. Even if the copy and sort are submitted separately and waited on with fences, the benchmark should still express the resource dependency explicitly: transfer writes must be visible before shader reads.

The pre-sort barrier is:

```cpp
VkMemoryBarrier sort_before = {VK_STRUCTURE_TYPE_MEMORY_BARRIER};
sort_before.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
sort_before.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
vkCmdPipelineBarrier(commandBuffer,
                     VK_PIPELINE_STAGE_TRANSFER_BIT,
                     VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                     0, 1, &sort_before, 0, nullptr, 0, nullptr);
```

The key-value indirect path also lets the sort command read the indirect element count, so the destination access mask needs to cover both shader reads and transfer reads:

```cpp
sort_before.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT;
```

### 4. Add a barrier between sort dispatch and copy-back transfer

The sort dispatch writes output buffers through compute shaders, and the benchmark immediately copies those results back to staging through a transfer copy. This must explicitly express that shader writes are visible before transfer reads.

The pre-copy-back barrier is:

```cpp
VkMemoryBarrier sort_after = {VK_STRUCTURE_TYPE_MEMORY_BARRIER};
sort_after.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
sort_after.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
vkCmdPipelineBarrier(commandBuffer,
                     VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                     VK_PIPELINE_STAGE_TRANSFER_BIT,
                     0, 1, &sort_after, 0, nullptr, 0, nullptr);
```

Without this barrier, the copy-back transfer can read the buffer before shader writes are visible to the transfer stage, causing the host to see unsorted or partially updated data.

### 5. These sync points are benchmark boundaries

The `vk_radix_sort` library already inserts internal barriers between compute passes such as upsweep, spine, and downsweep. The synchronization described here is external synchronization around the benchmark's use of the library:

- The boundary from host staging writes to GPU input buffers.
- The boundary from GPU sort output to host staging reads.

When integrating the library into another project, callers still need equivalent synchronization around their own resource flow. Do not assume the library function handles every producer/consumer relationship for external buffers.

These synchronization steps are implemented in `bench/vulkan_benchmark.cc` for both the keys-only and key-value paths.

## Verified build command

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

## Verified deploy and run commands

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

## Verified results

```text
Correctness check passed (N=1024)
keys gpu: 0.019365 ms
kv   gpu: 0.013454 ms

Correctness check passed (N=1048576)
keys gpu: 0.196242 ms
kv   gpu: 0.225817 ms
```

## Regression checklist

Before treating future HarmonyOS/Maleoon changes as working:

1. Rebuild `bench` with the HarmonyOS toolchain.
2. Push the binary to `/data/local/tmp/vulkan_radix_sort/bench`.
3. Run Vulkan correctness at `N=1024`.
4. Run Vulkan correctness at `N=1048576`.
5. Confirm neither run reports `VK_ERROR_DEVICE_LOST` or a correctness failure.
