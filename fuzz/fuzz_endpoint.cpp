// Fuzzes the endpoint URL split in dq::http_backend. No request is sent.
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>

#include "decision_engine.hpp"

// cppcheck-suppress unusedFunction symbolName=LLVMFuzzerTestOneInput
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *data, std::size_t size) {
  const std::string url(reinterpret_cast<const char *>(data), size);
  try {
    const dq::http_backend backend(url, "", "");
    // host + path reassembles the URL, or the URL had no path and path is "/".
    if (backend.host + backend.path != url && !(backend.path == "/" && backend.host == url))
      __builtin_trap();
  } catch (const std::invalid_argument &) {
    return 0;  // Rejected input is the expected outcome for most of the space.
  }
  return 0;
}
