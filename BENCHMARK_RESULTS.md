# Vulkan Radix Sort Benchmark Results — Maleoon 935

Date: 2026-06-10

## Test Environment

| Item | Value |
|------|-------|
| **Device** | HarmonyOS arm64 phone |
| **GPU** | Maleoon 935, Vulkan 1.4.309 |
| **CPU** | 4×1.53 GHz + 8×2.10 GHz + 2×2.75 GHz (big.LITTLE) |
| **CPU sort** | `std::sort` / `std::stable_sort`, pinned to big cores (cpu12-13) |
| **GPU sort** | Vulkan compute radix sort, WORKGROUP_SIZE=256, subgroupSize=32 |
| **Build** | HarmonyOS NDK cross-compiled, `-DCMAKE_BUILD_TYPE=Release` |
| **Measurement** | GPU: `vkCmdWriteTimestamp` × `timestampPeriod`; CPU: `chrono::high_resolution_clock` |
| **Sizes** | 1K, 2K, 4K, 8K, 16K, 32K, 64K, 128K, 256K, 512K, 1M |

## Locked Max Frequency Results

All CPU cores locked to max frequency via `scaling_min_freq = scaling_max_freq`:

| Core Group | Cores | Locked Freq |
|------------|-------|-------------|
| LITTLE | cpu0,1,6,7 | 1.53 GHz |
| Mid | cpu2,3,8–11 | 2.10 GHz |
| Big | cpu4,5,12,13 | 2.75 GHz |

### Keys Sort (Locked Max Freq)

| N | GPU (ms) | CPU std::sort (ms) | GPU Speedup |
|---:|---:|---:|---:|
| 1K | 0.796 | 0.039 | 0.05× (CPU faster) |
| 2K | 0.774 | 0.080 | 0.10× |
| 4K | 0.732 | 0.168 | 0.23× |
| 8K | 0.769 | 0.356 | 0.46× |
| 16K | 0.932 | 0.763 | 0.82× |
| **32K** | **1.284** | **1.645** | **1.28×** |
| 64K | 1.833 | 3.507 | 1.91× |
| 128K | 3.510 | 7.602 | 2.17× |
| 256K | 5.136 | 16.036 | 3.12× |
| 512K | 8.598 | 33.799 | 3.93× |
| **1M** | **16.076** | **72.212** | **4.49×** |

### Key-Value Sort (Locked Max Freq)

| N | GPU (ms) | CPU std::stable_sort (ms) | GPU Speedup |
|---:|---:|---:|---:|
| 1K | 0.837 | 0.054 | 0.06× (CPU faster) |
| 2K | 0.821 | 0.117 | 0.14× |
| 4K | 0.853 | 0.217 | 0.25× |
| 8K | 0.913 | 0.512 | 0.56× |
| 16K | 1.124 | 1.018 | 0.91× |
| **32K** | **1.643** | **2.388** | **1.45×** |
| 64K | 2.771 | 4.778 | 1.72× |
| 128K | 3.688 | 11.039 | 2.99× |
| 256K | 6.595 | 22.236 | 3.37× |
| 512K | 10.886 | 50.654 | 4.65× |
| **1M** | **18.572** | **104.204** | **5.61×** |

## Dynamic Frequency Results (Default Governor)

Default `misc` governor, CPU frequencies dynamically scaled. CPU pinned to big cores but governor may downclock.

### Keys Sort (Dynamic)

| N | GPU (ms) | CPU std::sort (ms) | GPU Speedup |
|---:|---:|---:|---:|
| 1K | 1.085 | 0.084 | 0.08× |
| 2K | 1.178 | 0.179 | 0.15× |
| 4K | 1.185 | 0.328 | 0.28× |
| 8K | 1.171 | 0.586 | 0.50× |
| 16K | 1.281 | 0.959 | 0.75× |
| 32K | 1.838 | 1.659 | 0.90× |
| **64K** | **2.631** | **3.574** | **1.36×** |
| 128K | 4.521 | 7.585 | 1.68× |
| 256K | 7.032 | 16.102 | 2.29× |
| 512K | 9.336 | 33.974 | 3.64× |
| **1M** | **18.809** | **71.615** | **3.81×** |

## Analysis

### Effect of Locking Max Frequency

Locking all cores to max freq improved:

| Component | Small N (≤8K) | Large N (≥64K) |
|-----------|--------------|----------------|
| **GPU** | 36–62% faster | 9–44% faster |
| **CPU** | 26–123% faster | 0–2% faster |

GPU gains are significant across all sizes because the GPU driver threads also benefit from locked CPU frequencies (command submission, fence polling run on CPU). CPU gains are concentrated at small N where the governor hasn't ramped up yet; at large N the dynamic governor already scales to near-max.

### GPU Launch Overhead

At locked max freq, GPU latency floor drops from ~1.2 ms to ~0.75 ms for N ≤ 8K. CPU `std::sort` at small N is still much faster due to near-zero overhead.

### Crossover Point

- **Locked**: GPU overtakes CPU at **N ≈ 32K** (both keys and KV)
- **Dynamic**: GPU overtakes CPU at **N ≈ 32K–64K**

Locking improves the crossover slightly by reducing GPU driver overhead.

### Scaling Behavior

**CPU** `std::sort` (introsort) follows O(N log N) exactly. At locked 2.75 GHz, the normalized time `T / (N·log₂N)` converges to **3.40 ns** for N ≥ 16K, which is ~9.4 clock cycles per comparison-swap.

**GPU** radix sort is O(N) with 4 passes × 8-bit radix. At locked max freq, peak throughput at N=1M reaches **0.065 GItems/s (65 MItems/s)** for keys.

### Peak Performance (N=1M, Locked)

| Sort | GPU | CPU | GPU Speedup |
|------|----:|----:|------------:|
| Keys | 16.08 ms | 72.21 ms | **4.49×** |
| Key-Value | 18.57 ms | 104.20 ms | **5.61×** |

## timestampPeriod Note

The Maleoon 935 reports `timestampPeriod ≈ 107 ns/tick`. GPU timestamp query results must be multiplied by this value to convert raw ticks to nanoseconds.

## Raw Data

- `vulkan_perf_locked.csv` — GPU benchmark, all cores locked max freq
- `cpu_perf_locked.csv` — CPU benchmark, all cores locked max freq
- `vulkan_perf.csv` — GPU benchmark, default dynamic governor
- `cpu_perf.csv` — CPU benchmark, default dynamic governor
- `gpu_vs_cpu_locked.png` — locked vs dynamic comparison chart
- `gpu_vs_cpu_correct.png` — dynamic governor comparison chart
