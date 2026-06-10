# Radix Sort Benchmark Results — Maleoon 935

Date: 2026-06-10

## Test Environment

| Item | Value |
|------|-------|
| **Device** | HarmonyOS arm64 phone |
| **GPU** | Maleoon 935, Vulkan 1.4.309, subgroupSize=32 |
| **CPU** | Kirin big.LITTLE: 4×1.72 GHz + 8×2.27 GHz + 2×2.75 GHz |
| **CPU sort** | `std::sort` / `std::stable_sort`, pinned to big cores (cpu12-13, 2.75 GHz) |
| **Frequency** | All 14 CPU cores locked to max via `scaling_min_freq = scaling_max_freq` |
| **GPU timing** | `vkCmdWriteTimestamp` × `timestampPeriod` (~107 ns/tick) |
| **CPU timing** | `std::chrono::high_resolution_clock` for `std::sort` |
| **Build** | HarmonyOS NDK cross-compiled, `-DCMAKE_BUILD_TYPE=Release` |
| **Sizes** | 1K, 2K, 4K, 8K, 16K, 32K, 64K, 128K, 256K, 512K, 1M |

## Algorithms Compared

| Algorithm | Source | Dispatch/Pass | Pipeline | Warmup | Runs |
|-----------|--------|---------------|----------|--------|------|
| **CPU std::sort** | C++ STL | — | — | 5 | 10 (median) |
| **vulkan_radix_sort** | [vulkan_radix_sort](.) | 3 dispatch/pass (upsweep→spine→downsweep) × 4 passes | Created once | 5 | 10 (median) |
| **VkRadixSort Multi** | [VkRadixSort](../VkRadixSort) | 2 dispatch/pass (histogram→scatter) × 4 passes | Created once (reused) | 5 | 10 (median) |

Both GPU radix sorts operate on 32-bit unsigned integers (full range `[0, 2³²-1]`) with 4 × 8-bit radix passes and `WORKGROUP_SIZE=256`.

## Full Comparison (Locked Max Freq, Fair Settings)

![Three-way comparison](benchmark_comparison_full.png)

### Keys-only Sort (GPU timestamp, median of 10 runs)

| N | CPU (ms) | vulkan_radix_sort (ms) | VkRadixSort Multi (ms) | vrs/CPU | vkr/CPU | vkr/vrs |
|---:|---:|---:|---:|---:|---:|---:|
| 1K | 0.038 | 0.686 | 0.664 | 0.06× | 0.06× | **0.97×** |
| 2K | 0.078 | 0.825 | 0.749 | 0.09× | 0.10× | **0.91×** |
| 4K | 0.166 | 0.857 | 0.742 | 0.19× | 0.22× | **0.87×** |
| 8K | 0.351 | 0.893 | 1.264 | 0.39× | 0.28× | 1.42× |
| 16K | 0.764 | 0.939 | 1.426 | 0.81× | 0.54× | 1.52× |
| **32K** | **1.653** | **1.197** | **2.229** | **1.38×** | 0.74× | 1.86× |
| 64K | 3.562 | 1.967 | 3.253 | 1.81× | **1.09×** | 1.65× |
| 128K | 7.537 | 3.471 | 4.960 | 2.17× | 1.52× | 1.43× |
| 256K | 15.955 | 5.305 | 9.235 | 3.01× | 1.73× | 1.74× |
| 512K | 34.154 | 8.625 | 16.069 | 3.96× | 2.13× | 1.86× |
| **1M** | **71.478** | **16.089** | **20.249** | **4.44×** | **3.53×** | **1.26×** |

> **vrs/CPU** = CPU time / vulkan_radix_sort time (>1 = GPU faster)
> **vkr/CPU** = CPU time / VkRadixSort time (>1 = GPU faster)
> **vkr/vrs** = VkRadixSort time / vulkan_radix_sort time (>1 = vulkan_radix_sort faster, <1 = VkRadixSort faster)

### Key-Value Sort (vulkan_radix_sort only)

| N | CPU kv (ms) | vrs kv GPU (ms) | GPU Speedup |
|---:|---:|---:|---:|
| 1K | 0.054 | 0.887 | 0.06× |
| 2K | 0.116 | 0.931 | 0.12× |
| 4K | 0.220 | 0.963 | 0.23× |
| 8K | 0.512 | 1.017 | 0.50× |
| 16K | 1.014 | 1.133 | 0.89× |
| **32K** | **2.375** | **1.630** | **1.46×** |
| 64K | 4.773 | 2.763 | 1.73× |
| 128K | 11.005 | 3.665 | 3.00× |
| 256K | 22.136 | 6.293 | 3.52× |
| 512K | 50.647 | 10.909 | 4.64× |
| **1M** | **105.362** | **18.532** | **5.69×** |

### Scaling Curves

![Scaling comparison](benchmark_comparison_scaling.png)

## Key Findings

### 1. Crossover Points

| Algorithm | Crossover N | Speedup at Crossover |
|-----------|:-----------:|:--------------------:|
| vulkan_radix_sort | **32K** | 1.38× |
| VkRadixSort Multi | **64K** | 1.09× |

At N < 32K, CPU `std::sort` is faster due to GPU launch overhead (~0.7 ms latency floor).

### 2. vulkan_radix_sort vs VkRadixSort Multi

In the fair comparison (same warmup, same data range), the results are more nuanced:

| Size Range | vkr/vrs | Winner | Margin |
|------------|:-------:|--------|--------|
| 1K–4K | 0.87–0.97 | **VkRadixSort** | 3–13% faster |
| 8K–16K | 1.42–1.52 | vulkan_radix_sort | 42–52% faster |
| 32K–64K | 1.65–1.86 | vulkan_radix_sort | 65–86% faster |
| 128K | 1.43 | vulkan_radix_sort | 43% faster |
| 256K–512K | 1.74–1.86 | vulkan_radix_sort | 74–86% faster |
| 1M | 1.26 | vulkan_radix_sort | 26% faster |

**VkRadixSort is faster at very small N (1K–4K)** where its 2-dispatch architecture has lower overhead. **vulkan_radix_sort wins decisively at N ≥ 8K** where its 3-pass approach (upsweep/spine/downsweep) achieves higher GPU utilization despite more dispatches. At N=1M the gap narrows to 1.26× because both approaches saturate the GPU.

### 3. Effect of Fairness Fixes

Compared to the initial unfair comparison, the fixes significantly changed the picture:

| Fix | Impact |
|-----|--------|
| **5 warmup + 10 median** | VkRadixSort N=1M improved from 27.0ms to 20.2ms (−25%). Small N also stabilized. |
| **Full 32-bit range** | VkRadixSort N=32K slowed from 1.4ms to 2.2ms (+59%). The 4th radix pass (shift=24) now has real work instead of trivially sorting zero top bits. |

### 4. Peak Performance (N=1M, Locked)

| Sort | GPU (ms) | CPU (ms) | GPU Speedup |
|------|--------:|--------:|------------:|
| **vulkan_radix_sort keys** | 16.09 | 71.48 | **4.44×** |
| **vulkan_radix_sort kv** | 18.53 | 105.36 | **5.69×** |
| VkRadixSort Multi keys | 20.25 | 71.48 | 3.53× |

### 5. CPU Scaling

CPU `std::sort` at locked 2.75 GHz follows O(N log N) exactly. The normalized time T/(N·log₂N) converges to **~3.4 ns** for N ≥ 16K, which is ~9.4 clock cycles per comparison-swap on the Cortex core.

## Fair Comparison Methodology

To ensure a fair comparison between the two GPU radix sort implementations:

1. **Same warmup**: Both use 5 warmup runs before timing
2. **Same repeat count**: Both take the median of 10 timed runs
3. **Same data range**: Both use full 32-bit `[0, 2³²-1]` uniform random integers
4. **Same GPU timing**: Both use `vkCmdWriteTimestamp` × `timestampPeriod` for GPU timing
5. **Same pipeline policy**: Both create Vulkan pipeline once, reuse across all sizes
6. **Same frequency**: All CPU cores locked to max frequency
7. **Correctness verified**: Each run verified against CPU `std::sort` result

### Remaining inherent differences

| Aspect | vulkan_radix_sort | VkRadixSort Multi |
|--------|-------------------|-------------------|
| Submit pattern | 3 × (submit+fence) per sort | Semaphore-chained, vkQueueWaitIdle |
| Timestamp queries | 2 (start/end of sort) | 8 (2 per iteration × 4 iterations) |
| Dispatch count | 12 (3 × 4 passes) | 8 (2 × 4 passes) |
| Pipeline barriers | Between upsweep/spine/downsweep | Between histogram/scatter |

These are inherent to each algorithm's design and represent real-world performance.

## Device Info

- **SoC**: Kirin (ARM big.LITTLE, 14 cores)
- **CPU topology**: 4×1.72 GHz (LITTLE) + 8×2.27 GHz (mid) + 2×2.75 GHz (big)
- **GPU**: Maleoon 935, Vulkan 1.4.309
- **subgroupSize**: 32, maxComputeInvocations=1024, maxSharedMemory=32768
- **timestampPeriod**: ~107 ns/tick

## Raw Data

- `vulkan_perf_locked.csv` — vulkan_radix_sort GPU, locked max freq
- `cpu_perf_locked.csv` — CPU std::sort, locked max freq
- `benchmark_comparison_full.png` — 3-panel comparison chart
- `benchmark_comparison_scaling.png` — log-log scaling curves
