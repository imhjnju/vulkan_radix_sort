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
# Cholesky 分解 (Cholesky Decomposition)

Cholesky 分解是一种将**对称正定矩阵**分解为下三角矩阵与其转置之积的方法。

## 数学定义

给定一个对称正定矩阵 **A**，Cholesky 分解将其表示为：

$$A = L \cdot L^T$$

其中 **L** 是下三角矩阵（主对角线以上的元素全为零），$L^T$ 是 **L** 的转置。

有时也写作 $A = L \cdot D \cdot L^T$（LDL 变体），其中 **D** 是对角矩阵。

## 计算公式

对于 $n \times n$ 矩阵 **A**，**L** 的元素逐行计算：

**对角元素：**
$$L_{ii} = \sqrt{A_{ii} - \sum_{k=1}^{i-1} L_{ik}^2}$$

**非对角元素：**
$$L_{ij} = \frac{1}{L_{jj}} \left( A_{ij} - \sum_{k=1}^{j-1} L_{ik} L_{jk} \right), \quad i > j$$

## 示例

$$A = \begin{bmatrix} 4 & 2 \\ 2 & 5 \end{bmatrix} \implies L = \begin{bmatrix} 2 & 0 \\ 1 & 2 \end{bmatrix}$$

验证：$L \cdot L^T = \begin{bmatrix} 2 & 0 \\ 1 & 2 \end{bmatrix} \begin{bmatrix} 2 & 1 \\ 0 & 2 \end{bmatrix} = \begin{bmatrix} 4 & 2 \\ 2 & 5 \end{bmatrix} = A$ ✓

## 前提条件

矩阵 **A** 必须满足：
1. **对称**：$A = A^T$
2. **正定**：对所有非零向量 $\mathbf{x}$，有 $\mathbf{x}^T A \mathbf{x} > 0$

等价判断正定的方法：所有特征值 > 0，或所有顺序主子式 > 0。

## 主要应用

| 应用领域                    | 用途                                                                     |
| --------------------------- | ------------------------------------------------------------------------ |
| **求解线性方程组** $Ax = b$ | 先解 $Ly = b$（前代），再解 $L^T x = y$（回代），效率约为 LU 分解的 2 倍 |
| **蒙特卡罗模拟**            | 生成具有指定协方差结构的相关随机变量                                     |
| **矩阵求逆**                | 求逆对称正定矩阵的高效方法                                               |
| **最小二乘法**              | 求解正规方程 $A^T A x = A^T b$                                           |
| **优化算法**                | 牛顿法中近似 Hessian 矩阵                                                |
| **卡尔曼滤波**              | 状态估计中的协方差传播                                                   |

## 与 LU 分解的比较

| 特性       | Cholesky             | LU 分解             |
| ---------- | -------------------- | ------------------- |
| 适用矩阵   | 对称正定             | 任意方阵            |
| 计算量     | $\frac{1}{3}n^3$     | $\frac{2}{3}n^3$    |
| 数值稳定性 | 天然稳定，无需选主元 | 通常需要部分选主元  |
| 存储空间   | 仅存 **L**（约一半） | 需存 **L** 和 **U** |

Cholesky 分解本质上利用了正定矩阵的优良性质，以更少的计算量和存储空间实现了比 LU 分解更高效的矩阵分解。

# Cholesky 分解的数值精度优势

## 1. 无需选主元（No Pivoting Needed）

LU 分解通常需要**部分选主元**（partial pivoting）来保证数值稳定性——即通过行交换将较大的元素移到对角位置。选主元本身引入额外的舍入误差来源，且会破坏矩阵结构（如稀疏性、带状结构）。

Cholesky 分解**天然稳定**，不需要任何行/列交换。原因在于正定矩阵的核心性质：

$$a_{ii} > 0, \quad a_{ii} \cdot a_{jj} > a_{ij}^2$$

对角元素始终是所在行/列中绝对值最大的元素之一，保证了消元过程中的"天然主元"。

## 2. 舍入误差增长有界

设机器精度为 $\varepsilon$，矩阵条件数为 $\kappa(A)$，则求解 $Ax = b$ 的相对误差界为：

| 方法            | 误差上界                                   |
| --------------- | ------------------------------------------ |
| **Cholesky**    | $O(\kappa(A) \cdot \varepsilon)$           |
| **LU + 选主元** | $O(\kappa(A) \cdot g_n \cdot \varepsilon)$ |

其中 $g_n$ 是**增长因子**（growth factor），理论上可达 $2^{n-1}$（虽然实际中很少出现）。Cholesky 没有增长因子的问题——**误差增长与问题规模 $n$ 呈线性关系**，而非指数关系。

## 3. 向后稳定性（Backward Stability）

Cholesky 分解是**向后稳定**的算法，即计算得到的 $L$ 满足：

$$(A + \delta A) = \tilde{L} \tilde{L}^T$$

其中扰动 $\|\delta A\| = O(\varepsilon) \|A\|$。

这意味着：**计算得到的分解是某个"邻近"精确矩阵的精确 Cholesky 分解**。对正定矩阵而言，这个邻近矩阵仍然是正定的（因为正定矩阵集合是开的），所以算法不会中途崩溃。

## 4. 条件数平方根效应

许多算法通过 Cholesky 分解处理协方差矩阵 $\Sigma$ 时，实际上是在 $L$ 的空间而非 $\Sigma$ 的空间中工作：

- $\kappa(\Sigma) = \kappa(A)$ 可能很大
- $\kappa(L) = \sqrt{\kappa(A)}$

**在 $L$ 空间操作等价于将条件数开方**，直接降低了数值敏感度。这在蒙特卡罗模拟和卡尔曼滤波中至关重要。

## 5. 实际影响示例

```
假设 A 的条件数 κ(A) = 10^8，机器精度 ε ≈ 10^-16 (double)

Cholesky 求解误差:  ≈ 10^8 × 10^-16 = 10^-8  （约 8 位有效数字）
LU 求解误差 (最坏): ≈ 10^8 × 2^63 × 10^-16 ≈ 10^8 × 10^3 × 10^-16 = 10^-5
                                                   ↑ 增长因子对 64×64 矩阵
```

## 6. 与其他方法的精度对比

| 场景               | Cholesky               | LU                     | QR                     |
| ------------------ | ---------------------- | ---------------------- | ---------------------- |
| 正定矩阵           | **最优**（稳定+高效）  | 可能过度选主元         | 精度好但开销 2×        |
| 接近奇异的正定矩阵 | 仍然向后稳定           | 选主元可能不够         | 更稳健但更贵           |
| 病态但正定         | 误差与 $\kappa$ 成正比 | 误差可能因增长因子放大 | 误差与 $\kappa$ 成正比 |

## 总结

Cholesky 的数值精度优势本质上来源于**正定性的数学约束**：

1. **无需选主元** → 消除增长因子这一误差放大器
2. **向后稳定** → 扰动与机器精度同量级
3. **天然对角占优** → 消元过程中数值不会失控膨胀
4. **平方根空间** → 等效条件数降低

这也是为什么在数值线性代数中，**只要矩阵满足对称正定条件，Cholesky 分解几乎总是首选**。

# 条件数（Condition Number）

## 直观理解

条件数衡量的是一个数学问题的**敏感度**：当输入发生微小变化时，输出会有多大变化。

- 条件数**小** → 问题"良态"（well-conditioned），输入的小扰动只引起输出的小变化
- 条件数**大** → 问题"病态"（ill-conditioned），输入的小扰动可能引起输出的剧烈变化

> 类比：条件数就像一个"放大镜倍数"——它告诉你误差最坏情况下会被放大多少倍。

## 矩阵条件数的定义

对于矩阵 **A**，条件数定义为：

$$\kappa(A) = \|A\| \cdot \|A^{-1}\|$$

其中 $\|\cdot\|$ 是矩阵范数。最常用的是**谱范数**（2-范数），此时：

$$\kappa_2(A) = \frac{\sigma_{\max}(A)}{\sigma_{\min}(A)}$$

即矩阵**最大奇异值与最小奇异值之比**。

如果 **A** 是对称矩阵，奇异值就是特征值的绝对值：

$$\kappa_2(A) = \frac{|\lambda_{\max}|}{|\lambda_{\min}|}$$

## 性质

| 性质                         | 说明                                        |
| ---------------------------- | ------------------------------------------- |
| $\kappa(A) \geq 1$           | 恒成立，单位矩阵 $\kappa(I) = 1$ 是最优情况 |
| $\kappa(cA) = \kappa(A)$     | 缩放矩阵不改变条件数                        |
| $\kappa(A^{-1}) = \kappa(A)$ | 逆矩阵条件数不变                            |
| $\kappa(Q) = 1$              | 正交矩阵条件数为 1（数值上最理想）          |

## 对求解线性方程组的影响

求解 $Ax = b$ 时，若 $\mathbf{b}$ 有相对扰动 $\frac{\|\delta b\|}{\|b\|}$，则解的相对误差满足：

$$\frac{\|\delta x\|}{\|x\|} \leq \kappa(A) \cdot \frac{\|\delta b\|}{\|b\|}$$

**含义：条件数就是误差的放大倍数。**

```
κ(A) = 10    → 最多损失 1 位有效数字
κ(A) = 10³   → 最多损失 3 位有效数字
κ(A) = 10⁸   → 最多损失 8 位有效数字
κ(A) = 10¹⁶  → double 精度下可能完全无效（16位全丢）
```

## 具体例子

### 良态矩阵

$$A = \begin{bmatrix} 2 & 0 \\ 0 & 3 \end{bmatrix}, \quad \kappa(A) = \frac{3}{2} = 1.5$$

对角矩阵，特征值接近，条件数接近 1，非常稳定。

### 病态矩阵 — Hilbert 矩阵

$$H_5 = \begin{bmatrix} 1 & \frac{1}{2} & \frac{1}{3} & \frac{1}{4} & \frac{1}{5} \\ \frac{1}{2} & \frac{1}{3} & \frac{1}{4} & \frac{1}{5} & \frac{1}{6} \\ \frac{1}{3} & \frac{1}{4} & \frac{1}{5} & \frac{1}{6} & \frac{1}{7} \\ \frac{1}{4} & \frac{1}{5} & \frac{1}{6} & \frac{1}{7} & \frac{1}{8} \\ \frac{1}{5} & \frac{1}{6} & \frac{1}{7} & \frac{1}{8} & \frac{1}{9} \end{bmatrix}, \quad \kappa(H_5) \approx 4.8 \times 10^5$$

仅 5×5 的矩阵就损失约 5 位有效数字。更大的 Hilbert 矩阵条件数指数级增长：

| 尺寸 $n$ | $\kappa(H_n)$                |
| -------- | ---------------------------- |
| 5        | $\approx 4.8 \times 10^5$    |
| 10       | $\approx 1.6 \times 10^{13}$ |
| 15       | $\approx 6.1 \times 10^{17}$ |
| 20       | $\approx 2.5 \times 10^{22}$ |

## 条件数与其他概念的关系

```
条件数
  ├── 矩阵求逆的难度    κ 大 → 求逆结果不可信
  ├── 线性方程组的敏感度  κ 大 → 解对数据误差极度敏感
  ├── 最小二乘的可靠性    κ(AᵀA) = κ(A)² → 法方程法精度更差
  ├── 迭代法的收敛速度    κ 大 → 共轭梯度等迭代法收敛慢
  └── 舍入误差的放大器    有效数字损失 ≈ log₁₀(κ)
```

## 为什么 Cholesky 中条件数重要

回到前面的讨论，任何数值方法求解 $Ax = b$ 的误差都不能低于条件数设定的理论下界：

$$\text{相对误差} \geq O(\kappa(A) \cdot \varepsilon_{\text{machine}})$$

- **这不是算法的缺陷，而是问题本身的数学属性。**
- Cholesky 能达到这个下界，所以是**最优的**——没有其他算法能在同样的矩阵上做得更好。
- 当 $\kappa(A)$ 本身很大时，任何方法都无法避免精度损失，此时需要**预处理**（preconditioning）等技术来降低等效条件数。

`3σ tile-touch` 是一种快速判断：

> 一个投影后的 2D Gaussian 是否可能覆盖某个屏幕 tile。

**什么是 3σ**
一维 Gaussian：

\[
G(x)=\exp\left(-\frac{x^2}{2\sigma^2}\right)
\]

距离中心 \(3\sigma\) 时：

\[
G(3\sigma)=\exp(-9/2)\approx0.0111
\]

也就是峰值的约 `1.11%`。原始 3DGS 将 Gaussian 的有效区域近似截断在 `3σ`，范围之外直接忽略。

对二维椭圆 Gaussian：

\[
G(\Delta)=\exp\left(-\frac12\Delta^T\Sigma^{-1}\Delta\right)
\]

`3σ` 边界满足：

\[
\Delta^T\Sigma^{-1}\Delta=9
\]

边界内部是椭圆。实现通常不会精确遍历椭圆，而是计算能够包住它的轴对齐矩形，再找这个矩形覆盖哪些 tile。

**什么是 tile-touch**
屏幕会被分成例如 `16×16` 像素的 tile。对每个 Gaussian：

1. 将它投影为屏幕空间椭圆。
2. 计算 `3σ` 椭圆的轴对齐包围盒。
3. 找出包围盒触及的全部 tile。
4. 为每个触及 tile 生成一条排序记录：

```text
(tile ID, depth) -> Gaussian ID
```

后续只在这些 tile 内评估该 Gaussian。

例如：

```text
Gaussian 3σ 包围盒
x = 30..70
y = 20..55
```

使用 `16×16` tile 时，它会被复制到覆盖这一矩形的所有 tile。这里的“touch”只表示包围盒与 tile 相交，不保证 Gaussian 对 tile 内每个像素都有显著贡献。

**为什么固定 3σ 会漏贡献**
实际 alpha 是：

\[
\alpha(\Delta)=o\exp\left(-\frac12
\Delta^T\Sigma^{-1}\Delta\right)
\]

其中 \(o\) 是 Gaussian opacity。rasterizer 的像素贡献阈值为 `1/255`。

固定 `3σ` 边界处：

\[
\alpha_{3\sigma}=o e^{-4.5}
\]

若 \(o=1\)：

\[
\alpha_{3\sigma}\approx0.0111>\frac1{255}\approx0.00392
\]

也就是说，高 opacity Gaussian 在 `3σ` 之外仍有部分贡献超过 renderer 的忽略阈值，但固定 `3σ` tile-touch 已经把这些 tile 排除了。

精确阈值边界应满足：

\[
o e^{-r^2/2}=\frac1{255}
\]

所以：

\[
r=\sqrt{2\ln(255o)}
\]

当 \(o=1\)：

\[
r\approx3.33\sigma
\]

因此最大 opacity 下，`3σ` 与 `3.33σ` 之间的贡献仍然应该被 rasterizer 处理，却可能在 tile 分配阶段被漏掉。

什么时候 `3σ` 足够？令：

\[
o e^{-4.5}\leq\frac1{255}
\]

可得：

\[
o\leq\frac{e^{4.5}}{255}\approx0.353
\]

因此：

- `opacity ≤ 0.353`：`3σ` 外贡献已低于 `1/255`
- `opacity > 0.353`：固定 `3σ` 可能过早截断

**StopThePop 的修正**
StopThePop 不只是看固定半径或包围盒。它对候选 tile 求 Gaussian 在 tile 内的最大贡献：

\[
\alpha_{\max,\text{tile}}
\]

然后判断：

\[
\alpha_{\max,\text{tile}} \geq \frac1{255}
\]

才保留该 Gaussian-tile pair。

这样既能：

- 保留 `3σ` 外仍有有效贡献的高 opacity Gaussian
- 剔除轴对齐包围盒中实际没有有效贡献的 tile
- 减少错误截断和无效排序记录

所以 `3σ tile-touch` 本质上是一个便宜但不完全匹配实际 alpha 阈值的候选 tile 生成规则。它的问题不是 FP32 或 FP16 精度，而是**截断规则本身与 renderer 的贡献阈值不一致**。