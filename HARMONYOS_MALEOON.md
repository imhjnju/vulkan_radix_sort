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

The HarmonyOS device exposed the benchmark's missing host/device synchronization assumptions:

1. Flush the mapped staging buffer after host writes and before GPU transfer reads.
2. Invalidate the mapped staging buffer after GPU transfer writes and before host reads.
3. Add an external barrier between the host-upload transfer and the sort dispatch.
4. Add an external barrier between the sort dispatch and the transfer copy-back.

These are implemented in `bench/vulkan_benchmark.cc` for both keys-only and key-value paths.

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
