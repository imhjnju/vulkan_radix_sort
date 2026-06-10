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

| Algorithm | Source | Dispatch/Pass | Pipeline |
|-----------|--------|---------------|----------|
| **CPU std::sort** | C++ STL | — | — |
| **vulkan_radix_sort** | [vulkan_radix_sort](.) | 3 dispatch/pass (upsweep→spine→downsweep) × 4 passes | Created once |
| **VkRadixSort Multi** | [VkRadixSort](../VkRadixSort) | 2 dispatch/pass (histogram→scatter) × 4 passes | Created once (reused) |

Both GPU radix sorts operate on 32-bit keys with 4 × 8-bit radix passes and `WORKGROUP_SIZE=256`.

## Full Comparison (Locked Max Freq)

![Three-way comparison](benchmark_comparison_full.png)

### Keys-only Sort

| N | CPU (ms) | vulkan_radix_sort (ms) | VkRadixSort Multi (ms) | vrs/CPU | vkr/CPU | vkr/vrs |
|---:|---:|---:|---:|---:|---:|---:|
| 1K | 0.039 | 0.808 | 0.643 | 0.05× | 0.06× | 0.80× |
| 2K | 0.080 | 0.717 | 0.752 | 0.11× | 0.11× | 1.05× |
| 4K | 0.168 | 0.728 | 0.922 | 0.23× | 0.18× | 1.27× |
| 8K | 0.349 | 0.774 | 1.233 | 0.45× | 0.28× | 1.59× |
| 16K | 0.761 | 0.836 | 1.268 | 0.91× | 0.60× | 1.52× |
| **32K** | **1.658** | **1.340** | **1.428** | **1.24×** | **1.16×** | **1.07×** |
| 64K | 3.542 | 1.998 | 2.037 | 1.77× | 1.74× | 1.02× |
| 128K | 7.562 | 3.409 | 4.766 | 2.22× | 1.59× | 1.40× |
| 256K | 15.984 | 5.166 | 8.911 | 3.09× | 1.79× | 1.72× |
| 512K | 33.920 | 8.594 | 17.320 | 3.95× | 1.96× | 2.02× |
| **1M** | **71.779** | **16.073** | **27.010** | **4.47×** | **2.66×** | **1.68×** |

> **vrs/CPU** = vulkan_radix_sort acceleration ratio vs CPU (>1 = GPU faster)
> **vkr/CPU** = VkRadixSort Multi acceleration ratio vs CPU
> **vkr/vrs** = VkRadixSort time / vulkan_radix_sort time (>1 = vulkan_radix_sort faster)

### Key-Value Sort (vulkan_radix_sort only)

| N | CPU kv (ms) | vrs kv GPU (ms) | GPU Speedup |
|---:|---:|---:|---:|
| 1K | 0.054 | 0.888 | 0.06× |
| 2K | 0.117 | 0.818 | 0.14× |
| 4K | 0.221 | 0.846 | 0.26× |
| 8K | 0.513 | 0.894 | 0.57× |
| 16K | 1.022 | 1.021 | 1.00× |
| **32K** | **2.383** | **1.628** | **1.46×** |
| 64K | 4.766 | 2.751 | 1.73× |
| 128K | 11.038 | 3.681 | 3.00× |
| 256K | 22.168 | 6.541 | 3.39× |
| 512K | 50.721 | 10.899 | 4.65× |
| **1M** | **105.365** | **18.835** | **5.59×** |

### Scaling Curves

![Scaling comparison](benchmark_comparison_scaling.png)

## Key Findings

### 1. Crossover Point

Both GPU radix sorts surpass CPU `std::sort` at **N ≈ 32K**:

| Algorithm | Crossover N | Speedup at Crossover |
|-----------|:-----------:|:--------------------:|
| vulkan_radix_sort | 32K | 1.24× |
| VkRadixSort Multi | 32K | 1.16× |

At N < 32K, the GPU launch overhead (~0.7 ms) dominates actual sort time, making CPU faster.

### 2. vulkan_radix_sort vs VkRadixSort Multi

vulkan_radix_sort is consistently faster than VkRadixSort Multi across nearly all sizes:

| Size Range | vkr/vrs Ratio | Interpretation |
|------------|:------------:|----------------|
| N ≤ 2K | 0.80–1.05× | Roughly equivalent |
| 4K–16K | 1.27–1.59× | vulkan_radix_sort 30–60% faster |
| 32K–64K | 1.02–1.07× | Nearly tied |
| 128K–1M | 1.40–2.02× | vulkan_radix_sort 40–100% faster |

**Why vulkan_radix_sort wins despite 3 dispatches per pass:**
- 3-pass (upsweep/spine/downsweep) has more dispatches but each is lighter weight
- 2-pass (histogram/scatter) does more work per dispatch but has higher register pressure
- At large N, the algorithmic efficiency of 3-pass wins decisively

### 3. Peak Performance (N=1M, Locked)

| Sort | GPU (ms) | CPU (ms) | GPU Speedup |
|------|--------:|--------:|------------:|
| **vulkan_radix_sort keys** | 16.07 | 71.78 | **4.47×** |
| **vulkan_radix_sort kv** | 18.84 | 105.37 | **5.59×** |
| VkRadixSort Multi keys | 27.01 | 71.78 | 2.66× |

### 4. Effect of Pipeline Reuse

VkRadixSort originally recreated its Vulkan pipeline (shaders, descriptor pools, command pools, sync objects) for every benchmark size. After refactoring to `init()/execute()/cleanup()`, the pipeline is created once and reused:

| N | Before (ms) | After (ms) | Improvement |
|---:|---:|---:|---:|
| 1K | 1.114 | 0.643 | −42.3% |
| 32K | 2.056 | 1.428 | −30.5% |
| 1M | 26.620 | 27.010 | +1.5% |

Pipeline creation overhead is significant at small N but negligible at large N where GPU computation dominates.

### 5. CPU Scaling

CPU `std::sort` at locked 2.75 GHz follows O(N log N) exactly. The normalized time T/(N·log₂N) converges to **~3.4 ns** for N ≥ 16K, which is ~9.4 clock cycles per comparison-swap on the Cortex core.

## Fair Comparison Methodology

To ensure a fair comparison between the two GPU radix sort implementations:

1. **Pipeline reuse**: Both implementations create their Vulkan pipeline once before the benchmark loop
2. **Same GPU timestamp method**: Both use `vkCmdWriteTimestamp` × `timestampPeriod` for GPU timing
3. **Same CPU frequency**: All cores locked to max frequency
4. **Same test data**: 32-bit unsigned integers, uniform random distribution
5. **Same sizes**: Power-of-2 from 1K to 1M
6. **Correctness verified**: Each size verified against CPU `std::sort` result

### Remaining differences in submit/fence patterns

- **vulkan_radix_sort**: Each sort uses separate `vkQueueSubmit` + `vkWaitForFences` calls (upload → fence, sort → fence, readback → fence = 3 submissions). GPU time measured by 2 timestamp queries (start/end of sort dispatches only).
- **VkRadixSort Multi**: Uses semaphore-chained dispatches within 4 iterations, with `vkQueueWaitIdle` at end. GPU time measured by 8 timestamp queries (2 per iteration, summed).

This means the GPU timestamp measurements capture pure compute time in both cases, but the wall-clock submit/fence overhead is different. The GPU timestamp numbers are comparable; the submit patterns are inherent to each algorithm's design.

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
