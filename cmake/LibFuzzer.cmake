# Vendored from https://github.com/cpp-best-practices/cmake_template (Unlicense),
# commit b86318abbf55a657a11a0cf2acce69f221841b22, cmake/LibFuzzer.cmake,
# with the myproject_ prefix renamed to dq_, and <cstddef> added to the probe:
# without it std::size_t is undeclared under libc++, as in ClusterFuzzLite.

function(dq_check_libfuzzer_support var_name)
  set(LibFuzzerTestSource
      "
#include <cstddef>
#include <cstdint>

extern \"C\" int LLVMFuzzerTestOneInput(const std::uint8_t *data, std::size_t size) {
  return 0;
}
    ")

  include(CheckCXXSourceCompiles)

  set(CMAKE_REQUIRED_FLAGS "-fsanitize=fuzzer")
  set(CMAKE_REQUIRED_LINK_OPTIONS "-fsanitize=fuzzer")
  check_cxx_source_compiles("${LibFuzzerTestSource}" ${var_name})

endfunction()
