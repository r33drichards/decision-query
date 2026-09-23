// Fuzzes the SQL functions' argument handling through a real SQLite connection.
// No backend is loadable, so every call ends in an error or NULL; the target is
// the argument parsing, subtype handling and error paths before inference.
#define SQLITE_CORE 1  // Keep this file's sqlite3_* calls off the extension API table.
#include <fuzzer/FuzzedDataProvider.h>
#include <sqlite3.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string>

#include "decision_query.h"

namespace {
  // Each statement takes three text arguments; json(?) passes a value with the
  // JSON subtype, as SQLite's own JSON functions would.
  constexpr std::array STATEMENTS = {
      "select noul(?1, ?2)",
      "select noul(json(?1), ?2, ?3)",
      "select choice(?1, ?2, ?3)",
      "select score(json(?1), json(?2), ?3)",
      "select decide(?1, ?2)",
      "select decide(json(?1), ?2)",
      "select dq_load('/nonexistent/dq-fuzz/' || ?1, ?2)",
  };

  // One connection for the whole run, opened on the first input. The fuzzer is
  // single threaded, so changing the environment here is safe.
  sqlite3 *connection() {
    static sqlite3 *const db = [] {
      setenv("DQ_MODEL_DIR", "/nonexistent/dq-fuzz", 1);  // NOLINT(concurrency-mt-unsafe)
      unsetenv("DQ_OPTIONS");                             // NOLINT(concurrency-mt-unsafe)
      sqlite3_auto_extension(reinterpret_cast<void (*)()>(sqlite3_decisionquery_init));
      sqlite3 *opened = nullptr;
      if (sqlite3_open(":memory:", &opened) != SQLITE_OK) std::abort();
      return opened;
    }();
    return db;
  }
}  // namespace

// cppcheck-suppress unusedFunction symbolName=LLVMFuzzerTestOneInput
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *data, std::size_t size) {
  FuzzedDataProvider provider(data, size);
  const char *sql = provider.PickValueInArray(STATEMENTS);
  sqlite3_stmt *statement = nullptr;
  if (sqlite3_prepare_v2(connection(), sql, -1, &statement, nullptr) != SQLITE_OK) std::abort();
  for (int i = 1; i <= 3; ++i) {
    if (provider.ConsumeBool()) continue;  // Leave unbound, i.e. NULL.
    const std::string text = provider.ConsumeRandomLengthString(4096);
    sqlite3_bind_text(statement, i, text.data(), static_cast<int>(text.size()), SQLITE_TRANSIENT);
  }
  const int rc = sqlite3_step(statement);
  // Without a backend nothing can produce a row except a NULL short circuit.
  if (rc == SQLITE_ROW && sqlite3_column_type(statement, 0) != SQLITE_NULL) std::abort();
  sqlite3_finalize(statement);
  return 0;
}
