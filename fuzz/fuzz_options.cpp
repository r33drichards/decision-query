// Fuzzes dq_load's option parsing: arbitrary bytes as the options JSON.
#include <cstddef>
#include <cstdint>
#include <stdexcept>

#include "decision_engine.hpp"

// cppcheck-suppress unusedFunction symbolName=LLVMFuzzerTestOneInput
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *data, std::size_t size) {
  const auto value = dq::json::parse(data, data + size, nullptr, /*allow_exceptions=*/false);
  if (value.is_discarded()) return -1;  // Keep only valid JSON in the corpus.
  try {
    const auto options = dq::parse_options(value);
    // bf16 always implies flash attention, as in the CLI.
    if (options.bf16 && !options.flash) __builtin_trap();
    (void)dq::checkpoint_path("models/laya", options);
  } catch (const std::invalid_argument &) {
    return 0;  // Rejected input is the expected outcome for most of the space.
  }
  return 0;
}
