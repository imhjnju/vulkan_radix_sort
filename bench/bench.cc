#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <limits>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "benchmark_factory.h"
#include "benchmark_base.h"
#include "data_generator.h"

namespace {

constexpr int      kWarmupRuns = 1;
constexpr int      kTimedRuns  = 10;
constexpr uint32_t kNMin       = 1u << 20;
constexpr uint32_t kNMax       = 1u << 20;
constexpr int      kNCount     = 1;
constexpr uint32_t kNStep      = 0;

double toMs(uint64_t ns) { return static_cast<double>(ns) / 1e6; }
double toGItemsS(uint32_t n, uint64_t ns) {
  return (static_cast<double>(n) / 1e9) / (static_cast<double>(ns) / 1e9);
}

uint64_t median(std::vector<uint64_t>& v) {
  auto mid = static_cast<std::ptrdiff_t>(v.size() / 2);
  std::nth_element(v.begin(), v.begin() + mid, v.end());
  return v[static_cast<size_t>(mid)];
}

uint32_t parseU32(const char* s) {
  if (s[0] == '-') throw std::runtime_error(std::string("Invalid uint32: ") + s);
  errno = 0;
  char* end = nullptr;
  unsigned long value = std::strtoul(s, &end, 10);
  if (errno == ERANGE || end == s || *end != '\0' || value > std::numeric_limits<uint32_t>::max()) {
    throw std::runtime_error(std::string("Invalid uint32: ") + s);
  }
  return static_cast<uint32_t>(value);
}

std::vector<uint32_t> parseSizes(int argc, char** argv) {
  if (argc == 4) {
    std::vector<uint32_t> sizes;
    std::stringstream ss(argv[3]);
    std::string item;
    while (std::getline(ss, item, ',')) {
      if (!item.empty()) sizes.push_back(parseU32(item.c_str()));
    }
    if (sizes.empty()) throw std::runtime_error("No sizes provided");
    for (uint32_t n : sizes) {
      if (n == 0) throw std::runtime_error("N must be positive");
    }
    return sizes;
  }

  uint32_t n_min = kNMin;
  uint32_t n_max = kNMax;
  int n_count = kNCount;
  if (argc == 6) {
    n_min = parseU32(argv[3]);
    n_max = parseU32(argv[4]);
    n_count = static_cast<int>(parseU32(argv[5]));
  }

  if (n_count <= 0) throw std::runtime_error("n_count must be positive");
  if (n_min == 0 || n_max == 0) throw std::runtime_error("N must be positive");
  if (n_max < n_min) throw std::runtime_error("n_max must be >= n_min");
  std::vector<uint32_t> sizes;
  sizes.reserve(static_cast<size_t>(n_count));
  uint32_t step = n_count == 1 ? 0 : (n_max - n_min) / static_cast<uint32_t>(n_count - 1);
  for (int i = 0; i < n_count; ++i) sizes.push_back(n_min + static_cast<uint32_t>(i) * step);
  return sizes;
}

struct Row {
  uint32_t n;
  std::string sort;
  double gpu_ms, cpu_ms;
  double gpu_gitems_s, cpu_gitems_s;
};

bool checkCorrectness(BenchmarkBase* bench, BenchmarkBase* cpu, uint32_t n, DataGenerator& gen) {
  auto data = gen.Generate(n);

  auto r0 = bench->Sort(data.keys);
  auto r1 = cpu->Sort(data.keys);
  for (uint32_t i = 0; i < n; ++i) {
    if (r0.keys[i] != r1.keys[i]) {
      std::cerr << "Sort correctness failed at index " << i << std::endl;
      return false;
    }
  }

  auto r2 = bench->SortKeyValue(data.keys, data.values);
  auto r3 = cpu->SortKeyValue(data.keys, data.values);
  for (uint32_t i = 0; i < n; ++i) {
    if (r2.keys[i] != r3.keys[i] || r2.values[i] != r3.values[i]) {
      std::cerr << "SortKeyValue correctness failed at index " << i << std::endl;
      return false;
    }
  }

  std::cout << "Correctness check passed (N=" << n << ")" << std::endl;
  return true;
}

Row measure(BenchmarkBase* bench, uint32_t n, const std::string& sort, DataGenerator& gen) {
  // warmup
  for (int i = 0; i < kWarmupRuns; ++i) {
    auto data = gen.Generate(n);
    if (sort == "keys")
      bench->Sort(data.keys);
    else
      bench->SortKeyValue(data.keys, data.values);
  }

  std::vector<uint64_t> gpu_times, cpu_times;
  gpu_times.reserve(kTimedRuns);
  cpu_times.reserve(kTimedRuns);

  for (int i = 0; i < kTimedRuns; ++i) {
    auto data = gen.Generate(n);
    BenchmarkBase::Results r;
    if (sort == "keys")
      r = bench->Sort(data.keys);
    else
      r = bench->SortKeyValue(data.keys, data.values);
    gpu_times.push_back(r.total_time);
    cpu_times.push_back(r.cpu_time);
  }

  uint64_t gpu_med = median(gpu_times);
  uint64_t cpu_med = median(cpu_times);

  return Row{n, sort, toMs(gpu_med), toMs(cpu_med),
             toGItemsS(n, gpu_med), toGItemsS(n, cpu_med)};
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2 || argc > 6 || argc == 5) {
    std::cerr << "Usage: bench <type> [output.csv] [sizes_csv | n_min n_max n_count]" << std::endl;
    std::cerr << "  type: vulkan | embree-cuda | cpu" << std::endl;
    return 1;
  }

  std::string type = argv[1];
  std::string csv_path = argc >= 3 ? argv[2] : "results.csv";
  std::vector<uint32_t> sizes;
  try {
    sizes = parseSizes(argc, argv);
  } catch (const std::exception& e) {
    std::cerr << e.what() << std::endl;
    return 1;
  }

  std::unique_ptr<BenchmarkBase> bench, cpu;
  try {
    bench = BenchmarkFactory::Create(type);
    cpu   = BenchmarkFactory::Create("cpu");
  } catch (const std::exception& e) {
    std::cerr << e.what() << std::endl;
    return 1;
  }

  DataGenerator gen;
  std::vector<Row> rows;

  for (size_t i = 0; i < sizes.size(); ++i) {
    uint32_t n = sizes[i];

    if (!checkCorrectness(bench.get(), cpu.get(), n, gen))
      return 1;

    for (const std::string& sort : {"keys", "kv"}) {
      Row row = measure(bench.get(), n, sort, gen);
      rows.push_back(row);

      std::cout << "[" << std::setw(3) << i + 1 << "/" << sizes.size() << "]"
                << " N=" << std::setw(9) << n
                << " [" << std::setw(4) << sort << "]"
                << "  gpu: " << std::fixed << std::setprecision(3) << row.gpu_ms << "ms"
                << " (" << std::setprecision(2) << row.gpu_gitems_s << " GItems/s)"
                << "  cpu: " << std::setprecision(3) << row.cpu_ms << "ms"
                << " (" << std::setprecision(2) << row.cpu_gitems_s << " GItems/s)"
                << std::endl;
    }
  }

  std::ofstream csv(csv_path);
  if (!csv) {
    std::cerr << "Failed to open " << csv_path << " for writing" << std::endl;
    return 1;
  }

  std::string lib_ver = bench->LibraryVersion();
  if (!lib_ver.empty())
    csv << "# version: " << lib_ver << "\n";
  csv << "backend,n,sort,gpu_ms,cpu_ms,gpu_gitems_s,cpu_gitems_s\n";
  for (const auto& r : rows) {
    csv << type << "," << r.n << "," << r.sort << ","
        << std::fixed << std::setprecision(6)
        << r.gpu_ms << "," << r.cpu_ms << ","
        << r.gpu_gitems_s << "," << r.cpu_gitems_s << "\n";
  }

  std::cout << "\nResults written to " << csv_path << std::endl;
  return 0;
}
