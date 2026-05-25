#include "embree_cuda_benchmark.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>

namespace {

constexpr uint32_t kRadixSortBins = 256;
constexpr uint32_t kRadixSortWgSize = 512;
constexpr uint32_t kBinFlagWords = kRadixSortWgSize / 32;

struct EmbreeCudaPair32 {
  uint32_t key;
  uint32_t value;
};

void CheckCuda(cudaError_t status, const char* what) {
  if (status != cudaSuccess) {
    throw std::runtime_error(std::string(what) + ": " + cudaGetErrorString(status));
  }
}

__device__ __forceinline__ uint64_t RadixKey(uint32_t value) {
  return value;
}

__device__ __forceinline__ uint64_t RadixKey(EmbreeCudaPair32 value) {
  return value.key;
}

template <typename SortType>
__global__ void EmbreeBinningKernel(const SortType* input, uint32_t items,
                                    uint32_t* global_histogram, uint32_t shift,
                                    uint32_t num_dss) {
  __shared__ uint32_t histogram[kRadixSortBins];

  const uint32_t local_id = threadIdx.x;
  const uint32_t group_id = blockIdx.x;
  const uint32_t start_id = group_id * items / num_dss;
  const uint32_t end_id = (group_id + 1) * items / num_dss;

  if (local_id < kRadixSortBins) histogram[local_id] = 0;
  __syncthreads();

  for (uint32_t id = start_id + local_id; id < end_id; id += blockDim.x) {
    const uint32_t bin = static_cast<uint32_t>((RadixKey(input[id]) >> shift) & (kRadixSortBins - 1));
    atomicAdd(&histogram[bin], 1);
  }
  __syncthreads();

  if (local_id < kRadixSortBins) {
    global_histogram[group_id * kRadixSortBins + local_id] = histogram[local_id];
  }
}

__global__ void EmbreeBuildOffsetsKernel(const uint32_t* global_histogram,
                                         uint32_t* global_offsets,
                                         uint32_t num_dss) {
  __shared__ uint32_t totals[kRadixSortBins];
  __shared__ uint32_t bases[kRadixSortBins];

  const uint32_t bin = threadIdx.x;
  uint32_t total = 0;
  for (uint32_t group = 0; group < num_dss; ++group) {
    total += global_histogram[group * kRadixSortBins + bin];
  }
  totals[bin] = total;
  __syncthreads();

  if (bin == 0) {
    uint32_t prefix = 0;
    for (uint32_t i = 0; i < kRadixSortBins; ++i) {
      bases[i] = prefix;
      prefix += totals[i];
    }
  }
  __syncthreads();

  uint32_t offset = bases[bin];
  for (uint32_t group = 0; group < num_dss; ++group) {
    global_offsets[group * kRadixSortBins + bin] = offset;
    offset += global_histogram[group * kRadixSortBins + bin];
  }
}

template <typename SortType>
__global__ void EmbreeScatterKernel(const SortType* input, SortType* output,
                                    uint32_t items, const uint32_t* global_offsets,
                                    uint32_t shift, uint32_t num_dss) {
  __shared__ uint32_t local_offset[kRadixSortBins];
  __shared__ uint32_t bin_flags[kRadixSortBins * kBinFlagWords];

  const uint32_t local_id = threadIdx.x;
  const uint32_t group_id = blockIdx.x;
  const uint32_t start_id = group_id * items / num_dss;
  const uint32_t end_id = (group_id + 1) * items / num_dss;

  if (local_id < kRadixSortBins) {
    local_offset[local_id] = global_offsets[group_id * kRadixSortBins + local_id];
  }
  __syncthreads();

  const uint32_t flags_bin = local_id / 32;
  const uint32_t flags_bit = 1u << (local_id % 32);

  for (uint32_t block_id = start_id; block_id < end_id; block_id += blockDim.x) {
    for (uint32_t i = local_id; i < kRadixSortBins * kBinFlagWords; i += blockDim.x) {
      bin_flags[i] = 0;
    }
    __syncthreads();

    const uint32_t id = block_id + local_id;
    SortType key_value{};
    uint32_t bin_id = 0;
    uint32_t bin_offset = 0;
    bool valid = id < end_id;
    if (valid) {
      key_value = input[id];
      bin_id = static_cast<uint32_t>((RadixKey(key_value) >> shift) & (kRadixSortBins - 1));
      bin_offset = local_offset[bin_id];
      atomicOr(&bin_flags[bin_id * kBinFlagWords + flags_bin], flags_bit);
    }
    __syncthreads();

    if (valid) {
      uint32_t prefix = 0;
      uint32_t count = 0;
      for (uint32_t i = 0; i < kBinFlagWords; ++i) {
        const uint32_t bits = bin_flags[bin_id * kBinFlagWords + i];
        const uint32_t full_count = __popc(bits);
        const uint32_t partial_count = __popc(bits & (flags_bit - 1));
        if (i < flags_bin) prefix += full_count;
        if (i == flags_bin) prefix += partial_count;
        count += full_count;
      }

      const uint32_t out_id = bin_offset + prefix;
      output[out_id] = key_value;
      if (prefix == count - 1) local_offset[bin_id] += count;
    }
    __syncthreads();
  }
}

template <typename SortType>
SortType* EmbreeRadixSortNx8BitCuda(SortType* input, SortType* output, uint32_t items,
                                    uint32_t* histogram, uint32_t* offsets,
                                    uint32_t start_iteration, uint32_t end_iteration,
                                    uint32_t num_dss, cudaStream_t stream) {
  SortType* current_input = input;
  SortType* current_output = output;

  for (uint32_t iter = start_iteration; iter < end_iteration; ++iter) {
    const uint32_t shift = iter * 8;
    EmbreeBinningKernel<<<num_dss, kRadixSortWgSize, 0, stream>>>(
        current_input, items, histogram, shift, num_dss);
    EmbreeBuildOffsetsKernel<<<1, kRadixSortBins, 0, stream>>>(histogram, offsets, num_dss);
    EmbreeScatterKernel<<<num_dss, kRadixSortWgSize, 0, stream>>>(
        current_input, current_output, items, offsets, shift, num_dss);
    std::swap(current_input, current_output);
  }

  return current_input;
}

uint32_t HighestPowerOfTwoAtMost(uint32_t value) {
  uint32_t result = 1;
  while (result <= value / 2) result <<= 1;
  return result;
}

uint32_t ChooseSortWorkgroups(uint32_t items, uint32_t max_sort_workgroups) {
  const uint32_t large_wg_size = 1024;
  const uint32_t next_power = HighestPowerOfTwoAtMost(std::max(items, 1u));
  const uint32_t by_size = std::max(1u, next_power / large_wg_size);
  return std::min(max_sort_workgroups, by_size);
}

uint64_t MsToNs(float ms) {
  return static_cast<uint64_t>(static_cast<double>(ms) * 1e6);
}

}  // namespace

std::string EmbreeCudaBenchmark::LibraryVersion() const {
  return "Embree v4.0.0-ploc radix_sort_Nx8Bit CUDA port (separate prefix kernel)";
}

EmbreeCudaBenchmark::EmbreeCudaBenchmark() {
  CheckCuda(cudaStreamCreate(&stream_), "cudaStreamCreate");
  CheckCuda(cudaEventCreate(&start_timestamp_), "cudaEventCreate start");
  CheckCuda(cudaEventCreate(&end_timestamp_), "cudaEventCreate end");
  CheckCuda(cudaMalloc(&keys_in_, MAX_ELEMENT_COUNT * sizeof(uint32_t)), "cudaMalloc keys_in");
  CheckCuda(cudaMalloc(&keys_out_, MAX_ELEMENT_COUNT * sizeof(uint32_t)), "cudaMalloc keys_out");
  CheckCuda(cudaMalloc(&pairs_in_, MAX_ELEMENT_COUNT * sizeof(EmbreeCudaPair32)), "cudaMalloc pairs_in");
  CheckCuda(cudaMalloc(&pairs_out_, MAX_ELEMENT_COUNT * sizeof(EmbreeCudaPair32)), "cudaMalloc pairs_out");
  max_sort_workgroups_ = RADIX_SORT_MAX_NUM_DSS;
  if (const char* env = std::getenv("EMBREE_CUDA_MAX_DSS")) {
    max_sort_workgroups_ = std::min(RADIX_SORT_MAX_NUM_DSS, std::max(1u, static_cast<uint32_t>(std::strtoul(env, nullptr, 10))));
  }
  CheckCuda(cudaMalloc(&histogram_, RADIX_SORT_MAX_NUM_DSS * RADIX_SORT_BINS * sizeof(uint32_t)), "cudaMalloc histogram");
  CheckCuda(cudaMalloc(&offsets_, RADIX_SORT_MAX_NUM_DSS * RADIX_SORT_BINS * sizeof(uint32_t)), "cudaMalloc offsets");
}

EmbreeCudaBenchmark::~EmbreeCudaBenchmark() {
  if (offsets_) cudaFree(offsets_);
  if (histogram_) cudaFree(histogram_);
  if (pairs_out_) cudaFree(pairs_out_);
  if (pairs_in_) cudaFree(pairs_in_);
  if (keys_out_) cudaFree(keys_out_);
  if (keys_in_) cudaFree(keys_in_);
  if (end_timestamp_) cudaEventDestroy(end_timestamp_);
  if (start_timestamp_) cudaEventDestroy(start_timestamp_);
  if (stream_) cudaStreamDestroy(stream_);
}

EmbreeCudaBenchmark::Results EmbreeCudaBenchmark::Sort(const std::vector<uint32_t>& keys) {
  const auto n = keys.size();
  if (n > MAX_ELEMENT_COUNT) throw std::runtime_error("EmbreeCudaBenchmark key count exceeds MAX_ELEMENT_COUNT");

  Results result;
  result.keys.resize(n);

  CheckCuda(cudaMemcpyAsync(keys_in_, keys.data(), n * sizeof(uint32_t), cudaMemcpyHostToDevice, stream_), "cudaMemcpyAsync keys input");
  CheckCuda(cudaStreamSynchronize(stream_), "cudaStreamSynchronize before keys sort");

  auto cpu_start = std::chrono::steady_clock::now();
  CheckCuda(cudaEventRecord(start_timestamp_, stream_), "cudaEventRecord keys start");
  const uint32_t num_dss = ChooseSortWorkgroups(static_cast<uint32_t>(n), max_sort_workgroups_);
  uint32_t* sorted = EmbreeRadixSortNx8BitCuda(static_cast<uint32_t*>(keys_in_),
                                               static_cast<uint32_t*>(keys_out_),
                                               static_cast<uint32_t>(n), histogram_, offsets_,
                                               0, 4, num_dss, stream_);
  CheckCuda(cudaGetLastError(), "embree keys kernel launch");
  CheckCuda(cudaEventRecord(end_timestamp_, stream_), "cudaEventRecord keys end");
  CheckCuda(cudaStreamSynchronize(stream_), "cudaStreamSynchronize keys sort");
  auto cpu_end = std::chrono::steady_clock::now();

  CheckCuda(cudaMemcpyAsync(result.keys.data(), sorted, n * sizeof(uint32_t), cudaMemcpyDeviceToHost, stream_), "cudaMemcpyAsync keys output");
  CheckCuda(cudaStreamSynchronize(stream_), "cudaStreamSynchronize keys output");

  float ms = 0.0f;
  CheckCuda(cudaEventElapsedTime(&ms, start_timestamp_, end_timestamp_), "cudaEventElapsedTime keys");
  result.total_time = MsToNs(ms);
  result.cpu_time = std::chrono::duration_cast<std::chrono::nanoseconds>(cpu_end - cpu_start).count();
  return result;
}

EmbreeCudaBenchmark::Results EmbreeCudaBenchmark::SortKeyValue(const std::vector<uint32_t>& keys,
                                                               const std::vector<uint32_t>& values) {
  const auto n = keys.size();
  if (n > MAX_ELEMENT_COUNT) throw std::runtime_error("EmbreeCudaBenchmark key count exceeds MAX_ELEMENT_COUNT");

  std::vector<EmbreeCudaPair32> pairs(n);
  for (size_t i = 0; i < n; ++i) pairs[i] = EmbreeCudaPair32{keys[i], values[i]};

  Results result;
  result.keys.resize(n);
  result.values.resize(n);

  CheckCuda(cudaMemcpyAsync(pairs_in_, pairs.data(), n * sizeof(EmbreeCudaPair32), cudaMemcpyHostToDevice, stream_), "cudaMemcpyAsync pairs input");
  CheckCuda(cudaStreamSynchronize(stream_), "cudaStreamSynchronize before pairs sort");

  auto cpu_start = std::chrono::steady_clock::now();
  CheckCuda(cudaEventRecord(start_timestamp_, stream_), "cudaEventRecord pairs start");
  const uint32_t num_dss = ChooseSortWorkgroups(static_cast<uint32_t>(n), max_sort_workgroups_);
  EmbreeCudaPair32* sorted = EmbreeRadixSortNx8BitCuda(static_cast<EmbreeCudaPair32*>(pairs_in_),
                                                       static_cast<EmbreeCudaPair32*>(pairs_out_),
                                                       static_cast<uint32_t>(n), histogram_, offsets_,
                                                       0, 4, num_dss, stream_);
  CheckCuda(cudaGetLastError(), "embree pairs kernel launch");
  CheckCuda(cudaEventRecord(end_timestamp_, stream_), "cudaEventRecord pairs end");
  CheckCuda(cudaStreamSynchronize(stream_), "cudaStreamSynchronize pairs sort");
  auto cpu_end = std::chrono::steady_clock::now();

  CheckCuda(cudaMemcpyAsync(pairs.data(), sorted, n * sizeof(EmbreeCudaPair32), cudaMemcpyDeviceToHost, stream_), "cudaMemcpyAsync pairs output");
  CheckCuda(cudaStreamSynchronize(stream_), "cudaStreamSynchronize pairs output");

  for (size_t i = 0; i < n; ++i) {
    result.keys[i] = pairs[i].key;
    result.values[i] = pairs[i].value;
  }

  float ms = 0.0f;
  CheckCuda(cudaEventElapsedTime(&ms, start_timestamp_, end_timestamp_), "cudaEventElapsedTime pairs");
  result.total_time = MsToNs(ms);
  result.cpu_time = std::chrono::duration_cast<std::chrono::nanoseconds>(cpu_end - cpu_start).count();
  return result;
}
