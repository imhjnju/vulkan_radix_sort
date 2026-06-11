# Radix Sort Benchmark Results — Maleoon 935

Date: 2026-06-11

## Test Environment

| Item | Value |
|------|-------|
| **Device** | HarmonyOS arm64 phone |
| **GPU** | Maleoon 935, Vulkan 1.4.309, subgroupSize=32 |
| **GPU freq** | Locked 933 MHz (max), was 235 MHz idle / DVFS auto |
| **DDR freq** | Locked 4800 MHz (max), was 1066 MHz idle / DVFS auto |
| **L1 bus freq** | Locked 1172 MHz (max) |
| **CPU** | Kirin big.LITTLE: 4×1.72 GHz + 8×2.27 GHz + 2×2.75 GHz |
| **CPU freq** | All 14 cores locked to max |
| **CPU sort** | `std::sort` / `std::stable_sort`, pinned to big cores (cpu12-13, 2.75 GHz) |
| **GPU timing** | `vkCmdWriteTimestamp` (ALL_COMMANDS_BIT) × `timestampPeriod` (~107 ns/tick), envelope |
| **CPU timing** | `std::chrono::high_resolution_clock` for `std::sort` |
| **Build** | HarmonyOS NDK cross-compiled, `-DCMAKE_BUILD_TYPE=Release` |
| **Sizes** | 1K, 2K, 4K, 8K, 16K, 32K, 64K, 128K, 256K, 512K, 1M, 2M, 4M, 8M |

## DVFS Locking

All DVFS domains locked to maximum frequency via sysfs:

```
GPU:  echo 933000000  > /sys/class/devfreq/gpufreq/{min,max}_freq        (235 → 933 MHz)
DDR:  echo 4800000000 > /sys/class/devfreq/ddrfreq/{min,max}_freq        (1066 → 4800 MHz)
DDRL: echo 4800000000 > /sys/class/devfreq/ddrfreq_latency/{min,max}_freq
BUS:  echo 1172000000 > /sys/class/devfreq/ddrfreq_l1bus_latency/{min,max}_freq
CPU:  echo $max_freq   > /sys/devices/system/cpu/cpu*/cpufreq/scaling_min_freq
```

DVFS auto mode had GPU running at 235 MHz (25% of max) and DDR at 1066 MHz (22% of max) during idle. Locking all domains removes frequency as a variable, making measurements deterministic and reproducible.

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

## Full Comparison (All Frequencies Locked)

![Three-way comparison](benchmark_comparison_full.png)

### Keys-only Sort (GPU timestamp envelope, median of 10 runs)

| N | CPU (ms) | vulkan_radix_sort (ms) | VkRadixSort Multi (ms) | vrs/CPU | vkr/CPU | vkr/vrs |
|---:|---:|---:|---:|---:|---:|---:|
| 1K | 0.038 | 0.287 | 0.392 | 0.13× | 0.10× | 1.37× |
| 2K | 0.076 | 0.295 | 0.417 | 0.26× | 0.18× | 1.41× |
| 4K | 0.167 | 0.298 | 0.473 | 0.56× | 0.35× | 1.59× |
| **8K** | **0.349** | **0.308** | **0.547** | **1.13×** | 0.64× | 1.78× |
| **16K** | **0.768** | **0.311** | **0.620** | **2.47×** | **1.24×** | 1.99× |
| **32K** | **1.641** | **0.442** | **0.566** | **3.71×** | **2.90×** | 1.28× |
| 64K | 3.503 | 0.610 | 0.865 | 5.74× | 4.05× | 1.42× |
| 128K | 7.511 | 1.104 | 1.317 | 6.80× | 5.70× | 1.19× |
| 256K | 15.977 | 2.153 | 2.403 | 7.42× | 6.65× | 1.12× |
| 512K | 33.768 | 3.936 | 4.631 | 8.58× | 7.29× | 1.18× |
| **1M** | **71.216** | **7.535** | **9.254** | **9.45×** | **7.70×** | 1.23× |
| **2M** | **150.964** | **14.713** | **19.042** | **10.26×** | **7.93×** | 1.29× |
| **4M** | **317.808** | **28.725** | **38.705** | **11.06×** | **8.21×** | 1.35× |
| **8M** | **684.130** | **57.050** | **132.641** | **11.99×** | **5.16×** | **2.32×** |

> **vrs/CPU** = CPU time / vulkan_radix_sort time (>1 = GPU faster)
> **vkr/CPU** = CPU time / VkRadixSort time (>1 = GPU faster)
> **vkr/vrs** = VkRadixSort time / vulkan_radix_sort time (>1 = vulkan_radix_sort faster, <1 = VkRadixSort faster)

### Key-Value Sort (vulkan_radix_sort only)

| N | CPU kv (ms) | vrs kv GPU (ms) | GPU Speedup |
|---:|---:|---:|---:|
| 1K | 0.053 | 0.316 | 0.17× |
| 2K | 0.117 | 0.328 | 0.36× |
| 4K | 0.220 | 0.330 | 0.67× |
| 8K | 0.511 | 0.332 | 1.54× |
| **16K** | **1.018** | **0.360** | **2.83×** |
| **32K** | **2.379** | **0.526** | **4.52×** |
| 64K | 4.859 | 0.777 | 6.25× |
| 128K | 11.114 | 1.468 | 7.57× |
| 256K | 22.340 | 3.095 | 7.22× |
| 512K | 50.696 | 5.693 | 8.90× |
| **1M** | **105.247** | **10.984** | **9.58×** |
| **2M** | **246.798** | **21.543** | **11.46×** |
| **4M** | **528.225** | **42.898** | **12.31×** |
| **8M** | **1249.504** | **85.528** | **14.61×** |

### Scaling Curves

![Scaling comparison](benchmark_comparison_scaling.png)

## Key Findings

### 1. DVFS Locking Impact

Locking GPU (235→933 MHz) and DDR (1066→4800 MHz) dramatically changed results:

| Metric | DVFS Auto | All Locked | Change |
|--------|----------:|-----------:|-------:|
| vrs GPU latency floor | ~0.7 ms | **~0.29 ms** | 2.4× lower |
| vrs 1M | 16.1 ms | **7.5 ms** | 2.1× faster |
| vrs 8M | 58.2 ms | **57.0 ms** | 1.02× (already at max) |
| vkr 1M | 20.3 ms | **9.3 ms** | 2.2× faster |
| vkr 8M | 147.2 ms | **132.6 ms** | 1.1× faster |

Key insight: Under DVFS auto, the GPU was at 25% frequency (235 MHz) during small-N tests. Large-N tests naturally ramped up frequency, making earlier data appear to show better GPU scaling than actually exists.

### 2. Crossover Points

| Algorithm | Crossover N | Speedup at Crossover |
|-----------|:-----------:|:--------------------:|
| vulkan_radix_sort | **8K** | 1.13× |
| VkRadixSort Multi | **16K** | 1.24× |

With frequencies locked, crossover drops from 32K/64K to **8K/16K** — the GPU is useful at much smaller sizes than DVFS-auto data suggested.

### 3. vulkan_radix_sort vs VkRadixSort Multi

| Size Range | vkr/vrs | Winner | Margin |
|------------|:-------:|--------|--------|
| 1K–2K | 1.37–1.41× | vulkan_radix_sort | 37–41% |
| 4K | 1.59× | vulkan_radix_sort | 59% |
| 8K–16K | 1.78–1.99× | vulkan_radix_sort | 78–99% |
| 32K | 1.28× | vulkan_radix_sort | 28% |
| 64K–256K | 1.12–1.42× | vulkan_radix_sort | 12–42% |
| 512K–4M | 1.18–1.35× | vulkan_radix_sort | 18–35% |
| **8M** | **2.32×** | **vulkan_radix_sort** | **132%** |

vulkan_radix_sort wins at **all sizes** with all frequencies locked. The gap is smallest at 256K (1.12×) and widest at 16K (1.99×) and 8M (2.32×).

### 4. Peak Performance (N=8M, All Locked)

| Sort | GPU (ms) | CPU (ms) | GPU Speedup | Throughput |
|------|--------:|--------:|------------:|-----------:|
| **vulkan_radix_sort keys** | 57.05 | 684.13 | **11.99×** | **147 MItems/s** |
| **vulkan_radix_sort kv** | 85.53 | 1249.50 | **14.61×** | 98 MItems/s |
| VkRadixSort Multi keys | 132.64 | 684.13 | 5.16× | 63 MItems/s |
| CPU std::sort | — | 684.13 | 1.00× | 12 MItems/s |

### 5. Scaling Analysis

How time grows when data doubles (ideal = 2.00×):

| Transition | CPU | vulkan_radix_sort | VkRadixSort |
|:----------:|:---:|:-----------------:|:-----------:|
| 1M → 2M | 2.12× | **1.95×** | 2.06× |
| 2M → 4M | 2.10× | **1.95×** | 2.03× |
| 4M → 8M | 2.15× | **1.99×** | **3.43×** ⚠️ |

**vulkan_radix_sort scales nearly linearly** (1.95–1.99×) across all sizes — the GPU is not yet saturated even at 8M, maintaining 139–147 MItems/s throughput.

**VkRadixSort degrades at 4M→8M**: time grows 3.43× for 2× data. This confirms the degradation is **algorithmic, not DVFS-related** — locking DDR to 4.8 GHz did not fix it. The root cause is the scatter shader's O(num_workgroups) global memory reads for histogram prefix sum (1024 WGs × 256 bins × 4 bytes = 1 GB at 8M).

### 6. Why vulkan_radix_sort Wins at All Sizes

| Factor | vulkan_radix_sort | VkRadixSort Multi |
|--------|-------------------|-------------------|
| GPU latency floor | **~0.29 ms** (single cmd buffer) | ~0.39 ms (4 cmd buffers, semaphore-chained) |
| Workgroup granularity | **2048 elem/WG** (more parallelism) | 8192 elem/WG (fewer WGs) |
| Global prefix sum | Dedicated spine pass (fixed 32 WG) | Scatter inlines O(num_WG) reads |
| WG-internal ranking | **subgroup ballot** (hardware) | bin_flags 8 KB shared memory |
| Atomics per element | ~1/32 (waveOffset==0 only) | 1 atomicAdd per element |

### 7. CPU Scaling

CPU `std::sort` at locked 2.75 GHz follows O(N log N) exactly. The normalized time T/(N·log₂N) converges to **~3.4 ns** for N ≥ 16K, which is ~9.4 clock cycles per comparison-swap on the Cortex core.

## Fair Comparison Methodology

All measurements use identical methodology:

1. **Same warmup**: 5 warmup runs before timing (both implementations)
2. **Same repeat count**: Median of 10 timed runs (both implementations)
3. **Same data range**: Full 32-bit `[0, 2³²-1]` uniform random integers (both implementations)
4. **Same timestamp stage**: `VK_PIPELINE_STAGE_ALL_COMMANDS_BIT` for all timestamps
5. **Same measurement type**: GPU timestamp envelope (first→last timestamp), not per-pass sum
6. **Same pipeline policy**: Vulkan pipeline created once, reused across all sizes
7. **Same frequency**: All DVFS domains locked to max (GPU 933 MHz, DDR 4.8 GHz, CPU 2.75 GHz)
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
- **GPU DVFS**: 235–933 MHz, 11 steps (locked to 933 MHz)
- **DDR DVFS**: 418–4800 MHz, 10 steps (locked to 4800 MHz)
- **subgroupSize**: 32, maxComputeInvocations=1024, maxSharedMemory=32768
- **timestampPeriod**: ~107 ns/tick

## Raw Data

- `vulkan_perf_locked.csv` — vulkan_radix_sort GPU, all freq locked (1K–8M)
- `cpu_perf_locked.csv` — CPU std::sort, all freq locked (1K–8M)
- `benchmark_comparison_full.png` — 3-panel comparison chart
- `benchmark_comparison_scaling.png` — log-log scaling curves
