# Radix Sort Benchmark Results — Maleoon 935

Date: 2026-06-11

## Test Environment

| Item | Value |
|------|-------|
| **Device** | HarmonyOS arm64 phone |
| **GPU** | Maleoon 935, Vulkan 1.4.309, subgroupSize=32 |
| **GPU freq** | Locked 933 MHz (was 235 MHz under DVFS auto) |
| **DDR freq** | Locked 4800 MHz (was 1066 MHz under DVFS auto) |
| **L1 bus freq** | Locked 1172 MHz |
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

| Algorithm | Source | Dispatch/Pass | Pipeline | Submit | Warmup | Runs |
|-----------|--------|---------------|----------|--------|--------|------|
| **CPU std::sort** | C++ STL | — | — | — | 5 | 10 (median) |
| **vulkan_radix_sort** | [vulkan_radix_sort](.) | 3 dispatch/pass (upsweep→spine→downsweep) × 4 passes | Created once | **1×** | 5 | 10 (median) |
| **VkRadixSort Multi** | [VkRadixSort](../VkRadixSort) | 2 dispatch/pass (histogram→scatter) × 4 passes | Created once (reused) | **1×** (single cmd buffer) | 5 | 10 (median) |

Both GPU radix sorts operate on 32-bit unsigned integers (full range `[0, 2³²-1]`) with 4 × 8-bit radix passes and `WORKGROUP_SIZE=256`.

## Timing Methodology

Both GPU implementations use `vkCmdWriteTimestamp` with `VK_PIPELINE_STAGE_ALL_COMMANDS_BIT` to measure the **envelope** time from first to last timestamp, both recorded in a **single command buffer** submitted once:

- **vulkan_radix_sort**: 2 timestamps around the entire sort (12 dispatch + internal barriers)
- **VkRadixSort Multi**: 2 timestamps around all 4 iterations (8 dispatch + internal barriers)

This eliminates any submit/semaphore overhead from the measurement, capturing only GPU compute + internal barrier time.

## Full Comparison (All Frequencies Locked, Single Submit)

![Three-way comparison](benchmark_comparison_full.png)

### Keys-only Sort (GPU timestamp envelope, median of 10 runs)

| N | CPU (ms) | vulkan_radix_sort (ms) | VkRadixSort Multi (ms) | vrs/CPU | vkr/CPU | vkr/vrs |
|---:|---:|---:|---:|---:|---:|---:|
| 1K | 0.039 | 0.286 | 0.257 | 0.14× | 0.15× | **0.90×** |
| 2K | 0.080 | 0.291 | 0.294 | 0.27× | 0.27× | **1.01×** |
| 4K | 0.169 | 0.296 | 0.367 | 0.57× | 0.46× | 1.24× |
| **8K** | **0.357** | **0.304** | **0.509** | **1.17×** | 0.70× | 1.67× |
| **16K** | **0.773** | **0.313** | **0.516** | **2.47×** | **1.50×** | 1.65× |
| **32K** | **1.666** | **0.440** | **0.524** | **3.79×** | **3.18×** | 1.19× |
| 64K | 3.549 | 0.627 | 0.823 | 5.66× | 4.31× | 1.31× |
| 128K | 7.505 | 1.111 | 1.271 | 6.76× | 5.90× | 1.14× |
| 256K | 15.963 | 2.140 | 2.367 | 7.46× | 6.74× | 1.11× |
| 512K | 34.155 | 3.924 | 4.590 | 8.70× | 7.44× | 1.17× |
| **1M** | **71.957** | **7.530** | **9.216** | **9.56×** | **7.81×** | 1.22× |
| **2M** | **151.249** | **14.660** | **19.007** | **10.32×** | **7.96×** | 1.30× |
| **4M** | **314.482** | **28.831** | **38.370** | **10.91×** | **8.20×** | 1.33× |
| **8M** | **689.529** | **57.240** | **133.113** | **12.05×** | **5.18×** | **2.33×** |

> **vrs/CPU** = CPU time / vulkan_radix_sort time (>1 = GPU faster)
> **vkr/CPU** = CPU time / VkRadixSort time (>1 = GPU faster)
> **vkr/vrs** = VkRadixSort time / vulkan_radix_sort time (>1 = vulkan_radix_sort faster, <1 = VkRadixSort faster)

### Key-Value Sort (vulkan_radix_sort only)

| N | CPU kv (ms) | vrs kv GPU (ms) | GPU Speedup |
|---:|---:|---:|---:|
| 1K | 0.055 | 0.314 | 0.18× |
| 2K | 0.120 | 0.327 | 0.37× |
| 4K | 0.219 | 0.329 | 0.67× |
| **8K** | **0.514** | **0.340** | **1.51×** |
| **16K** | **1.036** | **0.357** | **2.90×** |
| **32K** | **2.382** | **0.527** | **4.52×** |
| 64K | 4.812 | 0.780 | 6.17× |
| 128K | 11.114 | 1.459 | 7.62× |
| 256K | 22.171 | 3.060 | 7.25× |
| 512K | 50.724 | 5.672 | 8.94× |
| **1M** | **106.556** | **11.030** | **9.66×** |
| **2M** | **251.300** | **21.694** | **11.58×** |
| **4M** | **529.222** | **43.150** | **12.27×** |
| **8M** | **1258.716** | **85.551** | **14.71×** |

### Scaling Curves

![Scaling comparison](benchmark_comparison_scaling.png)

## Key Findings

### 1. VkRadixSort Wins at Small N

With single command buffer submission (eliminating the submit/semaphore unfairness), VkRadixSort actually wins at the smallest sizes:

| N | vkr/vrs | Winner |
|---:|:-:|---|
| **1K** | **0.90×** | **VkRadixSort wins by 10%** |
| **2K** | **1.01×** | **Essentially tied** |
| 4K | 1.24× | vulkan_radix_sort |

VkRadixSort's advantage at small N comes from fewer dispatches per pass (2 vs 3), meaning lower fixed overhead per radix iteration. The GPU latency floor is 0.257ms (vkr) vs 0.286ms (vrs).

### 2. Crossover Points

| Algorithm | Crossover N | Speedup at Crossover |
|-----------|:-----------:|:--------------------:|
| vulkan_radix_sort | **8K** | 1.17× |
| VkRadixSort Multi | **16K** | 1.50× |

### 3. Peak Performance (N=8M, All Locked)

| Sort | GPU (ms) | CPU (ms) | GPU Speedup | Throughput |
|------|--------:|--------:|------------:|-----------:|
| **vulkan_radix_sort keys** | 57.24 | 689.53 | **12.05×** | **147 MItems/s** |
| **vulkan_radix_sort kv** | 85.55 | 1258.72 | **14.71×** | 98 MItems/s |
| VkRadixSort Multi keys | 133.11 | 689.53 | 5.18× | 63 MItems/s |
| CPU std::sort | — | 689.53 | 1.00× | 12 MItems/s |

### 4. Scaling Analysis

How time grows when data doubles (ideal = 2.00×):

| Transition | CPU | vulkan_radix_sort | VkRadixSort |
|:----------:|:---:|:-----------------:|:-----------:|
| 1M → 2M | 2.10× | **1.95×** | 2.06× |
| 2M → 4M | 2.08× | **1.97×** | 2.02× |
| 4M → 8M | 2.19× | **1.99×** | **3.47×** ⚠️ |

**vulkan_radix_sort scales nearly linearly** (1.95–1.99×) — the GPU is not yet saturated even at 8M, maintaining 139–147 MItems/s throughput.

**VkRadixSort degrades at 4M→8M**: time grows 3.47× for 2× data. Root cause: the scatter shader reads ALL workgroup histograms from global memory for prefix sum — at 8M with 1024 workgroups, this is 1024 × 256 × 4 bytes = 1 MB of global memory reads per workgroup, totaling ~1 GB across all workgroups.

### 5. Why the Gap Widens at Large N

| Factor | vulkan_radix_sort | VkRadixSort Multi |
|--------|-------------------|-------------------|
| Global prefix sum | Dedicated spine pass (fixed 32 WG, O(1) read) | Scatter inlines O(num_WG) global reads |
| WG-internal ranking | **subgroup ballot** (hardware, zero cost) | bin_flags 8 KB shared memory |
| Atomics per element | ~1/32 (waveOffset==0 only) | 1 atomicAdd per element |
| Memory layout | keys ↔ storage.inout ping-pong (~2N) | buffer0 ↔ buffer1 ping-pong (~2N) |
| Workgroup granularity | 2048 elem/WG | 8192 elem/WG |

> **Note**: Both implementations use ping-pong buffers of ~2N size — vulkan_radix_sort swaps `keysBuffer` ↔ `storageBuffer.inout` between even/odd passes (see `vk_radix_sort.h:1404-1414`), VkRadixSort swaps `buffer0` ↔ `buffer1`. Memory footprint is roughly equal (~64-65 MB at 8M), so the 8M degradation is purely algorithmic, not memory-pressure.

### 6. CPU Scaling

CPU `std::sort` at locked 2.75 GHz follows O(N log N) exactly. The normalized time T/(N·log₂N) converges to **~3.4 ns** for N ≥ 16K, which is ~9.4 clock cycles per comparison-swap on the Cortex core.

## Fair Comparison Methodology

All measurements use identical methodology:

1. **Same warmup**: 5 warmup runs before timing (both implementations)
2. **Same repeat count**: Median of 10 timed runs (both implementations)
3. **Same data range**: Full 32-bit `[0, 2³²-1]` uniform random integers (both implementations)
4. **Same timestamp stage**: `VK_PIPELINE_STAGE_ALL_COMMANDS_BIT` for all timestamps
5. **Same measurement type**: GPU timestamp envelope (first→last timestamp), not per-pass sum
6. **Same pipeline policy**: Vulkan pipeline created once, reused across all sizes
7. **Same submit pattern**: Both use **single command buffer, single submit** (no semaphores)
8. **Same frequency**: All DVFS domains locked to max (GPU 933 MHz, DDR 4.8 GHz, CPU 2.75 GHz)
9. **Correctness verified**: Each run verified against CPU `std::sort` result

### Remaining inherent differences

| Aspect | vulkan_radix_sort | VkRadixSort Multi |
|--------|-------------------|-------------------|
| Dispatch count | 12 (3 × 4 passes) | 8 (2 × 4 passes) |
| Pipeline barriers | Between upsweep/spine/downsweep | Between histogram/scatter |
| Descriptor binding | VK_KHR_push_descriptor | Traditional descriptor sets |
| Memory layout | keys ↔ storage.inout ping-pong (~2N) | buffer0 ↔ buffer1 ping-pong (~2N) |

These are inherent to each algorithm's design and represent real-world performance characteristics.

## Workgroup Size Tuning (vulkan_radix_sort)

vulkan_radix_sort's performance is governed by three constants that must stay **consistent across the host and all three shaders** (upsweep/spine/downsweep):

```cpp
constexpr uint32_t RADIX             = 256;   // 4 passes × 8 bits
constexpr int      WORKGROUP_SIZE    = 256;   // local_size_x in every shader
constexpr int      PARTITION_DIVISION = 8;    // blocks per workgroup
constexpr int      PARTITION_SIZE    = 8 * 256 = 2048;  // elements per workgroup
```

### Parameter-by-parameter impact

#### 1. WORKGROUP_SIZE = 256 — device-constrained, NOT a free parameter

This is the workgroup thread count (`local_size_x`). On Maleoon 935 it is **capped at 256**:

- `WORKGROUP_SIZE = 512` (the desktop default) compiles fine but causes **`VK_ERROR_DEVICE_LOST`** during queue execution — see [HARMONYOS_MALEOON.md](HARMONYOS_MALEOON.md). So 256 is the hard ceiling on this GPU, not a tuning choice.
- **Register/shared-memory pressure** (downsweep): `localHistogram[2048]` = 8 KB + `localHistogramSum[256]` = 1 KB + `subgroupSums[8]` ≈ **9 KB/WG**. With `maxSharedMemory = 32 KB`, at most ~3 workgroups can be resident per compute unit, capping occupancy. Raising WORKGROUP_SIZE would worsen this even if the device allowed it.

#### 2. PARTITION_DIVISION = 8 — the main algorithmic knob

Each workgroup processes `PARTITION_DIVISION` blocks of `WORKGROUP_SIZE` keys, i.e. **2048 keys/WG**. This trades launch overhead against resource pressure:

| Larger PARTITION_DIVISION (e.g. 16 → 4096 keys/WG) | Smaller PARTITION_DIVISION (e.g. 4 → 1024 keys/WG) |
|---|---|
| Fewer workgroups → fewer dispatches, less launch overhead | More workgroups → better GPU occupancy |
| **Spine scans fewer partitions** (O(partitionCount) shrinks) | Spine scans more partitions |
| Downsweep threads hold more registers (`localKeys[8]`→`[16]`, etc.) | Fewer registers per thread |
| Shared memory `localHistogram[PARTITION_SIZE]` grows linearly → lower occupancy | Shared memory smaller → higher occupancy |

The value **8** is an empirically tuned compromise: large enough that spine's `O(partitionCount)` scan stays cheap at 8M (4096 partitions), small enough that downsweep's 9 KB shared memory leaves room for occupancy. It is the most impactful tunable but changing it requires re-measuring at multiple N — small N favors larger divisions (fewer dispatches), large N favors smaller (more parallelism + occupancy).

#### 3. subgroupSize = 32 — queryable, can be requested via extension

Maleoon 935 reports subgroupSize = 32. The benchmark requests it explicitly via `VK_EXT_subgroup_size_control` (`requiredSubgroupSize = 32`). It affects two things:

- **Spine dispatch count**: `RoundUp(256, 256/32) = 32 workgroups` (fixed, independent of N). A larger subgroup size (e.g. 64) would reduce this to 16, but is hardware-limited.
- **Wave count per workgroup**: `waveCount = 256/32 = 8` in downsweep. More subgroup operations run in lockstep, affecting ballot granularity.

Changing it is only possible within the device's `[minSubgroupSize, maxSubgroupSize]` range — for Maleoon 935 that range is narrow, so it's effectively fixed.

### Derived quantities (not independently tunable)

| Quantity | Formula | Value at 8M |
|----------|---------|:-----------:|
| Partition count | `ceil(N / 2048)` | 4096 |
| Upsweep dispatch | `partitionCount` | 4096 WG |
| Spine dispatch | `RoundUp(256, 256/subgroupSize)` | **32 WG** (fixed) |
| Downsweep dispatch | `partitionCount` | 4096 WG |
| Storage buffer size | `(4 + 4·256 + 4096·256) × 4 B` | ~4 MB |
| Per-WG shared mem (downsweep) | `2048 + 256 + 8` uints | ~9 KB |

### Summary: what's actually tunable on this device

| Parameter | Tunable? | Constraint |
|-----------|:--------:|-----------|
| WORKGROUP_SIZE | ❌ | Capped at 256 (512 = device lost) |
| PARTITION_DIVISION | ✅ | Main knob, 8 is tuned default; affects occupancy vs launch overhead |
| subgroupSize | ⚠️ | Narrow range on Maleoon 935, effectively 32 |
| RADIX | ❌ | Tied to 8-bit radix = 4 passes, algorithmic |

The only parameter worth experimenting with is **PARTITION_DIVISION** (e.g. try 4, 8, 16), but it requires rebuilding shaders and re-benchmarking across the full N range since the optimal value shifts with problem size.

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
