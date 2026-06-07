cmake_minimum_required(VERSION 3.24)

project(vk_radix_sort VERSION 0.3.3 LANGUAGES CXX)

find_package(Vulkan REQUIRED)
message(STATUS "Vulkan version: ${Vulkan_VERSION}")
find_package(Python3 REQUIRED)
include(FetchContent)

option(BENCH_ENABLE_EMBREE_CUDA "Build Embree CUDA radix sort benchmark backend" OFF)
set(BENCH_EMBREE_CUDA_ARCH "native" CACHE STRING "CUDA architecture for the Embree CUDA benchmark backend")

# shaders
file(MAKE_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/src/generated)
message(STATUS "glslangValidator executable: ${Vulkan_GLSLANG_VALIDATOR_EXECUTABLE}")

# build_shader(SHADER OUTPUT DEFINE...)
function(build_shader)
  list(POP_FRONT ARGV SHADER OUTPUT)
  list(TRANSFORM ARGV PREPEND "-D" OUTPUT_VARIABLE DEFINES)

  get_filename_component(SHADER ${SHADER} ABSOLUTE)
  set(OPTIMIZATION_ARGS -Os)
  if(OUTPUT STREQUAL "downsweep_key_value_slang")
    set(OPTIMIZATION_ARGS)
  endif()

  add_custom_target(${OUTPUT}
    COMMAND
      ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/glslang_to_header.py
      ${Vulkan_GLSLANG_VALIDATOR_EXECUTABLE}
      -V
      ${OPTIMIZATION_ARGS}
      --target-env spirv1.5
      -o ${CMAKE_CURRENT_SOURCE_DIR}/src/generated/${OUTPUT}.h
      ${DEFINES}
      ${SHADER}
    COMMENT "Compiling ${CMAKE_CURRENT_SOURCE_DIR}/src/generated/${OUTPUT}.h from ${SHADER}"
    VERBATIM
  )
endfunction()

# header-only library
add_library(vk_radix_sort INTERFACE)

target_include_directories(vk_radix_sort INTERFACE
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
)

build_shader(src/shader/upsweep.comp upsweep_slang)
build_shader(src/shader/spine.comp spine_slang)
build_shader(src/shader/downsweep.comp downsweep_slang)
build_shader(src/shader/downsweep.comp downsweep_key_value_slang KEY_VALUE)

add_custom_target(vk_radix_sort_header
  COMMAND
    ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/generate_header.py
    ${CMAKE_CURRENT_SOURCE_DIR}/src/vk_radix_sort.h.in
    ${CMAKE_CURRENT_SOURCE_DIR}/include/vk_radix_sort.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/generated
    ${PROJECT_VERSION_MAJOR} ${PROJECT_VERSION_MINOR} ${PROJECT_VERSION_PATCH}
  DEPENDS
    upsweep_slang
    spine_slang
    downsweep_slang
    downsweep_key_value_slang
    ${CMAKE_CURRENT_SOURCE_DIR}/src/vk_radix_sort.h.in
  COMMENT "Generating include/vk_radix_sort.h from template"
  VERBATIM
)

add_dependencies(vk_radix_sort vk_radix_sort_header)

# bench
if(PROJECT_IS_TOP_LEVEL)
  FetchContent_Declare(
    volk
    GIT_REPOSITORY https://github.com/zeux/volk.git
    GIT_TAG        f30088b3f4160810b53e19258dd2f7395e5f0ba3  # vulkan-sdk-1.4.328.1
  )

  FetchContent_Declare(
    VulkanMemoryAllocator
    GIT_REPOSITORY https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator.git
    GIT_TAG        1d8f600fd424278486eade7ed3e877c99f0846b1  # v3.3.0
  )
  FetchContent_MakeAvailable(volk VulkanMemoryAllocator)

  set(BENCH_SOURCES
    bench/bench.cc
    bench/benchmark_factory.cc
    bench/cpu_benchmark.cc
    bench/data_generator.cc
    bench/vma_impl.cc
    bench/volk_impl.cc
    bench/vrdx_impl.cc
    bench/vulkan_benchmark.cc
  )

  if(BENCH_ENABLE_EMBREE_CUDA)
    enable_language(CUDA)
    find_package(CUDAToolkit REQUIRED)
    list(APPEND BENCH_SOURCES
      bench/embree_cuda_benchmark.cu
    )
  endif()

  add_executable(bench ${BENCH_SOURCES})
  add_dependencies(bench vk_radix_sort_header)

  set_target_properties(bench PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL"
  )

  if(BENCH_ENABLE_EMBREE_CUDA)
    set_target_properties(bench PROPERTIES CUDA_ARCHITECTURES ${BENCH_EMBREE_CUDA_ARCH})
    target_compile_definitions(bench PUBLIC BENCH_EMBREE_CUDA)
    target_link_libraries(bench PRIVATE CUDA::cudart)
  endif()

  target_compile_definitions(bench PRIVATE VK_NO_PROTOTYPES)

  target_link_libraries(bench PRIVATE vk_radix_sort volk::volk_headers VulkanMemoryAllocator)
endif()
