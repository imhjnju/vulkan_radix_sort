#ifndef VK_RADIX_SORT_EMBREE_CUDA_BENCHMARK_H
#define VK_RADIX_SORT_EMBREE_CUDA_BENCHMARK_H

#include "benchmark_base.h"

#include <cuda_runtime.h>

class EmbreeCudaBenchmark : public BenchmarkBase {
 private:
  static constexpr uint32_t MAX_ELEMENT_COUNT = 1 << 25;
  static constexpr uint32_t RADIX_SORT_BINS = 256;
  static constexpr uint32_t RADIX_SORT_MAX_NUM_DSS = 256;

 public:
  EmbreeCudaBenchmark();
  ~EmbreeCudaBenchmark() override;

  std::string LibraryVersion() const override;

  Results Sort(const std::vector<uint32_t>& keys) override;
  Results SortKeyValue(const std::vector<uint32_t>& keys,
                       const std::vector<uint32_t>& values) override;

 private:
  cudaStream_t stream_ = nullptr;
  cudaEvent_t start_timestamp_ = nullptr;
  cudaEvent_t end_timestamp_ = nullptr;

  void* keys_in_ = nullptr;
  void* keys_out_ = nullptr;
  void* pairs_in_ = nullptr;
  void* pairs_out_ = nullptr;
  uint32_t* histogram_ = nullptr;
  uint32_t* offsets_ = nullptr;
  uint32_t max_sort_workgroups_ = RADIX_SORT_MAX_NUM_DSS;
};

#endif  // VK_RADIX_SORT_EMBREE_CUDA_BENCHMARK_H
