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
| **GPU timing** | `vkCmdWriteTimestamp` (ALL_COMMANDS_BIT) × `timestampPeriod` (~107 ns/tick), envelope |
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

## Timing Methodology

Both GPU implementations use `vkCmdWriteTimestamp` with `VK_PIPELINE_STAGE_ALL_COMMANDS_BIT` to measure the **envelope** time from first to last timestamp:

- **vulkan_radix_sort**: 2 timestamps (start/end) around the entire sort command buffer → single envelope
- **VkRadixSort Multi**: 8 timestamps (2 per iteration × 4 iterations) → envelope from first to last timestamp

This captures the full GPU pipeline including barriers, fill buffer, and inter-dispatch synchronization, making the measurements directly comparable.

## Full Comparison (Locked Max Freq, Fair Settings)

![Three-way comparison](benchmark_comparison_full.png)

### Keys-only Sort (GPU timestamp envelope, median of 10 runs)

| N | CPU (ms) | vulkan_radix_sort (ms) | VkRadixSort Multi (ms) | vrs/CPU | vkr/CPU | vkr/vrs |
|---:|---:|---:|---:|---:|---:|---:|
| 1K | 0.038 | 0.686 | 0.729 | 0.06× | 0.05× | 1.06× |
| 2K | 0.078 | 0.825 | 0.816 | 0.09× | 0.10× | **0.99×** |
| 4K | 0.166 | 0.857 | 1.000 | 0.19× | 0.17× | 1.17× |
| 8K | 0.351 | 0.893 | 1.485 | 0.39× | 0.24× | 1.66× |
| 16K | 0.764 | 0.939 | 1.521 | 0.81× | 0.50× | 1.62× |
| **32K** | **1.653** | **1.197** | **2.265** | **1.38×** | 0.73× | 1.89× |
| 64K | 3.562 | 1.967 | 3.336 | 1.81× | **1.07×** | 1.70× |
| 128K | 7.537 | 3.471 | 5.039 | 2.17× | 1.50× | 1.45× |
| 256K | 15.955 | 5.305 | 9.350 | 3.01× | 1.71× | 1.76× |
| 512K | 34.154 | 8.625 | 16.091 | 3.96× | 2.12× | 1.87× |
| **1M** | **71.478** | **16.089** | **20.204** | **4.44×** | **3.54×** | **1.26×** |

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
| VkRadixSort Multi | **64K** | 1.07× |

At N < 32K, CPU `std::sort` is faster due to GPU launch overhead (~0.7 ms latency floor).

### 2. vulkan_radix_sort vs VkRadixSort Multi

| Size Range | vkr/vrs | Winner | Margin |
|------------|:-------:|--------|--------|
| 2K | 0.99× | **Tied** | — |
| 1K | 1.06× | Roughly equal | 6% |
| 4K | 1.17× | vulkan_radix_sort | 17% |
| 8K–16K | 1.62–1.66× | vulkan_radix_sort | 62–66% |
| 32K | 1.89× | vulkan_radix_sort | 89% |
| 64K–512K | 1.45–1.87× | vulkan_radix_sort | 45–87% |
| **1M** | **1.26×** | vulkan_radix_sort | 26% |

**At N=2K the two implementations are essentially tied (0.99×).** vulkan_radix_sort wins decisively at N ≥ 8K due to higher GPU utilization from its 3-pass architecture, with the gap peaking at N=32K (1.89×) and narrowing at N=1M (1.26×) as both saturate the GPU.

### 3. Peak Performance (N=1M, Locked)

| Sort | GPU (ms) | CPU (ms) | GPU Speedup |
|------|--------:|--------:|------------:|
| **vulkan_radix_sort keys** | 16.09 | 71.48 | **4.44×** |
| **vulkan_radix_sort kv** | 18.53 | 105.36 | **5.69×** |
| VkRadixSort Multi keys | 20.20 | 71.48 | 3.54× |

### 4. CPU Scaling

CPU `std::sort` at locked 2.75 GHz follows O(N log N) exactly. The normalized time T/(N·log₂N) converges to **~3.4 ns** for N ≥ 16K, which is ~9.4 clock cycles per comparison-swap on the Cortex core.

## Fair Comparison Methodology

All measurements use identical methodology:

1. **Same warmup**: 5 warmup runs before timing (both implementations)
2. **Same repeat count**: Median of 10 timed runs (both implementations)
3. **Same data range**: Full 32-bit `[0, 2³²-1]` uniform random integers (both implementations)
4. **Same timestamp stage**: `VK_PIPELINE_STAGE_ALL_COMMANDS_BIT` for all timestamps
5. **Same measurement type**: GPU timestamp envelope (first→last timestamp), not per-pass sum
6. **Same pipeline policy**: Vulkan pipeline created once, reused across all sizes
7. **Same frequency**: All 14 CPU cores locked to max frequency
8. **Correctness verified**: Each run verified against CPU `std::sort` result

### Remaining inherent differences

| Aspect | vulkan_radix_sort | VkRadixSort Multi |
|--------|-------------------|-------------------|
| Submit pattern | Single command buffer per sort | 4 command buffers, semaphore-chained |
| Dispatch count | 12 (3 × 4 passes) | 8 (2 × 4 passes) |
| Pipeline barriers | Between upsweep/spine/downsweep | Between histogram/scatter |
| Timestamp count | 2 (single envelope) | 8 (envelope from first to last) |

These are inherent to each algorithm's design and represent real-world performance characteristics.

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
