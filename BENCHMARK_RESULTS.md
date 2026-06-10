# Vulkan Radix Sort Benchmark Results — Maleoon 935

Date: 2026-06-10

## Test Environment

| Item | Value |
|------|-------|
| **Device** | HarmonyOS arm64 phone |
| **GPU** | Maleoon 935, Vulkan 1.4.309 |
| **CPU** | 4×1.53 GHz + 8×2.10 GHz + 2×2.75 GHz (big.LITTLE) |
| **CPU sort** | `std::sort` / `std::stable_sort`, pinned to big cores (cpu12-13, 2.75 GHz) |
| **GPU sort** | Vulkan compute radix sort, WORKGROUP_SIZE=256, subgroupSize=32 |
| **Build** | HarmonyOS NDK cross-compiled, `-DCMAKE_BUILD_TYPE=Release` |
| **Measurement** | GPU: `vkCmdWriteTimestamp` × `timestampPeriod`; CPU: `chrono::high_resolution_clock` |
| **Sizes** | 1K, 2K, 4K, 8K, 16K, 32K, 64K, 128K, 256K, 512K, 1M |

## GPU vs CPU Latency

### Keys Sort

| N | GPU (ms) | CPU std::sort (ms) | GPU Speedup |
|---:|---:|---:|---:|
| 1K | 1.085 | 0.084 | 0.08× (CPU faster) |
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

### Key-Value Sort

| N | GPU (ms) | CPU std::stable_sort (ms) | GPU Speedup |
|---:|---:|---:|---:|
| 1K | 1.282 | 0.117 | 0.09× (CPU faster) |
| 2K | 1.303 | 0.271 | 0.21× |
| 4K | 1.345 | 0.411 | 0.31× |
| 8K | 1.411 | 0.832 | 0.59× |
| 16K | 1.501 | 1.097 | 0.73× |
| 32K | 2.291 | 2.387 | 1.04× |
| **64K** | **3.532** | **4.775** | **1.35×** |
| 128K | 6.254 | 11.068 | 1.77× |
| 256K | 7.850 | 22.287 | 2.84× |
| 512K | 18.903 | 51.088 | 2.70× |
| **1M** | **28.038** | **103.812** | **3.70×** |

## Analysis

### GPU Launch Overhead

For N ≤ 8K, GPU latency is essentially constant at ~1.2 ms regardless of input size. This floor represents the Vulkan command submission, pipeline execution startup, and fence synchronization overhead on Maleoon 935. CPU `std::sort` has near-zero overhead and is faster in this regime.

### Crossover Point

The GPU overtakes CPU at approximately **N = 32K–64K**:

- Keys: GPU faster at N ≥ 64K
- Key-Value: GPU faster at N ≥ 32K

This crossover is higher than typical desktop GPUs (~4K–8K), reflecting the relatively large GPU launch overhead on this mobile device.

### Scaling Behavior

**GPU** scales sub-linearly in practice (radix sort is O(N) with 4 passes × 8-bit radix). GPU throughput climbs from 0.001 GI/s at 1K to **0.056 GI/s** at 1M as the launch overhead amortizes.

**CPU** `std::sort` (introsort) follows O(N log N) complexity exactly. The normalized time `T / (N·log₂N)` converges to **3.41 ns** for N ≥ 32K, confirming correct O(N log N) scaling. At 2.75 GHz this is ~9.4 clock cycles per comparison-swap, consistent with branch misprediction + L1/L2 access costs.

### Peak GPU Throughput

| Sort | N=1M Throughput |
|------|----------------:|
| Keys | 0.056 GItems/s (56 MItems/s) |
| Key-Value | 0.037 GItems/s (37 MItems/s) |

## timestampPeriod Note

The Maleoon 935 reports `timestampPeriod ≈ 107 ns/tick`. GPU timestamp query results must be multiplied by this value to convert raw ticks to nanoseconds. Early measurements that omitted this conversion reported artificially low GPU times (~0.2 ms instead of ~19 ms at N=1M).

## Raw Data

- `vulkan_perf.csv` — GPU benchmark (`./bench vulkan`)
- `cpu_perf.csv` — CPU benchmark (`./bench cpu`), pinned to big cores
- `gpu_vs_cpu_correct.png` — comparison chart
