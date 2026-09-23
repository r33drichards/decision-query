// Runs the SQL functions through a real SQLite connection with the static
// library registered as an auto extension.
//
// SQLITE_CORE stops sqlite3ext.h (included by decision_query.h) from rerouting
// this file's sqlite3_* calls through the extension API table.
#define SQLITE_CORE 1
#include <sqlite3.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <cstdlib>
#include <string>

#include "decision_query.h"

using Catch::Matchers::ContainsSubstring;
using Catch::Matchers::StartsWith;

namespace {
  // The outcome of a single-row, single-column query.
  struct result {
    int type = SQLITE_NULL;
    std::string text;
    std::string error;
  };

  class database {
  public:
    database() {
      sqlite3_auto_extension(reinterpret_cast<void (*)()>(sqlite3_decisionquery_init));
      REQUIRE(sqlite3_open(":memory:", &db_) == SQLITE_OK);
    }
    ~database() {
      sqlite3_close(db_);
      sqlite3_reset_auto_extension();
    }
    database(const database &) = delete;
    database &operator=(const database &) = delete;
    database(database &&) = delete;
    database &operator=(database &&) = delete;

    result query(const std::string &sql) {
      sqlite3_stmt *statement = nullptr;
      REQUIRE(sqlite3_prepare_v2(db_, sql.c_str(), -1, &statement, nullptr) == SQLITE_OK);
      result out;
      const int rc = sqlite3_step(statement);
      if (rc == SQLITE_ROW) {
        out.type = sqlite3_column_type(statement, 0);
        if (const auto *text = sqlite3_column_text(statement, 0))
          out.text = reinterpret_cast<const char *>(text);
      } else if (rc != SQLITE_DONE) {
        out.error = sqlite3_errmsg(db_);
      }
      sqlite3_finalize(statement);
      return out;
    }

  private:
    sqlite3 *db_ = nullptr;
  };
}  // namespace

TEST_CASE("dq_version reports the build version", "[sqlite]") {
  database db;
  CHECK_THAT(db.query("select dq_version()").text, StartsWith("v"));
}

TEST_CASE("functions return NULL for NULL arguments", "[sqlite]") {
  database db;
  for (const char *sql : {"select noul(NULL, 'q')", "select noul('s', NULL)",
                          "select noul('s', 'q', NULL)", "select choice('s', 'q', NULL)",
                          "select score(NULL, 'q', '[\"a\"]')", "select decide('s', NULL)"}) {
    INFO(sql);
    const auto out = db.query(sql);
    CHECK(out.error.empty());
    CHECK(out.type == SQLITE_NULL);
  }
}

TEST_CASE("functions reject malformed JSON arguments", "[sqlite]") {
  database db;
  CHECK_THAT(db.query("select noul('s', 'q', 'nope')").error,
             ContainsSubstring("noul criteria must be valid JSON"));
  CHECK_THAT(db.query("select choice('s', 'q', '{')").error,
             ContainsSubstring("choice criteria must be valid JSON"));
  CHECK_THAT(db.query("select score('s', 'q', '[1,')").error,
             ContainsSubstring("score criteria must be valid JSON"));
  CHECK_THAT(db.query("select decide('s', 'nope')").error,
             ContainsSubstring("laya questions must be valid JSON"));
  CHECK_THAT(db.query("select decide('s', '[1]')").error,
             ContainsSubstring("nonempty JSON object"));
}

TEST_CASE("dq_load validates its arguments", "[sqlite]") {
  database db;
  CHECK_THAT(db.query("select dq_load(NULL)").error,
             ContainsSubstring("requires a checkpoint directory"));
  CHECK_THAT(db.query("select dq_load('/nonexistent/checkpoint')").error,
             ContainsSubstring("Not a checkpoint directory"));
  CHECK_THAT(db.query("select dq_load('/nonexistent/checkpoint', '{\"gpu\": true}')").error,
             ContainsSubstring("Unknown option"));
  CHECK_THAT(db.query("select dq_load('/nonexistent/checkpoint', 'nope')").error,
             ContainsSubstring("must be valid JSON"));
}

TEST_CASE("inference without a backend explains how to load one", "[sqlite]") {
  // The tests run on one thread, so changing the environment here is safe.
  setenv("DQ_MODEL_DIR", "/nonexistent/checkpoint", 1);  // NOLINT(concurrency-mt-unsafe)
  unsetenv("DQ_OPTIONS");                                // NOLINT(concurrency-mt-unsafe)
  database db;
  CHECK(db.query("select dq_backend()").type == SQLITE_NULL);
  CHECK_THAT(db.query("select noul('s', 'q')").error,
             ContainsSubstring("No decision backend loaded"));
  CHECK_THAT(db.query("select decide(json('{\"a\": 1}'), '{\"q\": {\"type\": \"noul\"}}')").error,
             ContainsSubstring("No decision backend loaded"));
}
