// SQLite extension exposing Laya typed decisions as SQL functions.
//
// One Laya checkpoint stays resident per process. Calls into the resident agent
// are serialized, as the laya runtime requires.
#include "decision_query.h"

#include "sqlite3ext.h"
SQLITE_EXTENSION_INIT1

#include <stdexcept>
#include <string>
#include <utility>

#include "decision_engine.hpp"

namespace {
  using json = laya::json;
  using dq::engine;

  constexpr unsigned JSON_SUBTYPE = 74;  // 'J', shared with SQLite's json1 functions.

  std::string text_of(sqlite3_value *value) {
    const unsigned char *text = sqlite3_value_text(value);
    return text ? std::string(reinterpret_cast<const char *>(text),
                              static_cast<std::size_t>(sqlite3_value_bytes(value)))
                : std::string();
  }

  // Values produced by SQLite's JSON functions carry a subtype and are passed as
  // structured JSON; everything else is passed as text.
  json state_or_text(sqlite3_value *value) {
    if (sqlite3_value_subtype(value) == JSON_SUBTYPE) return json::parse(text_of(value));
    return json(text_of(value));
  }

  json parse_json_argument(sqlite3_value *value, const char *name) {
    try {
      return json::parse(text_of(value));
    } catch (const json::exception &e) {
      throw std::invalid_argument(std::string(name) + " must be valid JSON: " + e.what());
    }
  }

  bool any_null(int argc, sqlite3_value **argv) {
    for (int i = 0; i < argc; ++i)
      if (sqlite3_value_type(argv[i]) == SQLITE_NULL) return true;
    return false;
  }

  template <typename Body> void guarded(sqlite3_context *context, Body &&body) {
    try {
      body();
    } catch (const std::exception &e) {
      sqlite3_result_error(context, e.what(), -1);
    } catch (...) {
      sqlite3_result_error(context, "Unknown error", -1);
    }
  }

  void result_text(sqlite3_context *context, const std::string &text) {
    sqlite3_result_text(context, text.c_str(), static_cast<int>(text.size()), SQLITE_TRANSIENT);
  }

  // Evaluates one question against a state and returns its answer object.
  json single_answer(sqlite3_value *state, json question) {
    json questions = json::object();
    questions["q"] = std::move(question);
    return engine::instance().answers(state_or_text(state), questions).at("q");
  }

  void dq_version(sqlite3_context *context, int, sqlite3_value **) {
    sqlite3_result_text(context, SQLITE_DQ_VERSION, -1, SQLITE_STATIC);
  }

  void dq_backend(sqlite3_context *context, int, sqlite3_value **) {
    guarded(context, [&] {
      const auto backend = engine::instance().backend_name();
      if (backend.empty())
        sqlite3_result_null(context);
      else
        result_text(context, backend);
    });
  }

  void dq_load(sqlite3_context *context, int argc, sqlite3_value **argv) {
    guarded(context, [&] {
      if (sqlite3_value_type(argv[0]) == SQLITE_NULL)
        throw std::invalid_argument("dq_load requires a checkpoint directory");
      json options;
      if (argc > 1 && sqlite3_value_type(argv[1]) != SQLITE_NULL)
        options = parse_json_argument(argv[1], "dq_load options");
      result_text(context, engine::instance().load(text_of(argv[0]), options));
    });
  }

  void noul(sqlite3_context *context, int argc, sqlite3_value **argv) {
    guarded(context, [&] {
      if (any_null(argc, argv)) return sqlite3_result_null(context);
      json question = {{"type", "noul"}, {"instructions", state_or_text(argv[1])}};
      if (argc > 2) question["criteria"] = parse_json_argument(argv[2], "noul criteria");
      const json answer = single_answer(argv[0], std::move(question));
      sqlite3_result_double(context, answer.at("noul").get<double>());
    });
  }

  void choice(sqlite3_context *context, int argc, sqlite3_value **argv) {
    guarded(context, [&] {
      if (any_null(argc, argv)) return sqlite3_result_null(context);
      json question = {{"type", "choice"},
                       {"instructions", state_or_text(argv[1])},
                       {"criteria", parse_json_argument(argv[2], "choice criteria")}};
      const json answer = single_answer(argv[0], std::move(question));
      result_text(context, answer.at("choice").get<std::string>());
    });
  }

  void score(sqlite3_context *context, int argc, sqlite3_value **argv) {
    guarded(context, [&] {
      if (any_null(argc, argv)) return sqlite3_result_null(context);
      json question = {{"type", "score"},
                       {"instructions", state_or_text(argv[1])},
                       {"criteria", parse_json_argument(argv[2], "score criteria")}};
      const json answer = single_answer(argv[0], std::move(question));
      sqlite3_result_double(context, answer.at("score").get<double>());
    });
  }

  void decide_answers(sqlite3_context *context, int argc, sqlite3_value **argv) {
    guarded(context, [&] {
      if (any_null(argc, argv)) return sqlite3_result_null(context);
      const json questions = parse_json_argument(argv[1], "questions");
      if (!questions.is_object() || questions.empty())
        throw std::invalid_argument("questions must be a nonempty JSON object");
      result_text(context, engine::instance().answers(state_or_text(argv[0]), questions).dump());
#ifdef SQLITE_RESULT_SUBTYPE
      sqlite3_result_subtype(context, JSON_SUBTYPE);
#endif
    });
  }

  int subtype_flags() {
    int flags = 0;
#ifdef SQLITE_SUBTYPE
    flags |= SQLITE_SUBTYPE;
#endif
    return flags;
  }

  int result_subtype_flag() {
#ifdef SQLITE_RESULT_SUBTYPE
    return SQLITE_RESULT_SUBTYPE;
#else
    return 0;
#endif
  }
}  // namespace

extern "C"
#ifdef _WIN32
    __declspec(dllexport)
#else
    __attribute__((visibility("default")))
#endif
    int
    sqlite3_decisionquery_init(sqlite3 *db, char **pzErrMsg, const sqlite3_api_routines *pApi) {
  SQLITE_EXTENSION_INIT2(pApi);
  (void)pzErrMsg;
  // cppcheck-suppress badBitmaskCheck ; subtype_flags() is 0 only on older SQLite.
  const int inference = SQLITE_UTF8 | SQLITE_DETERMINISTIC | subtype_flags();
  struct entry {
    const char *name;
    int argc;
    int flags;
    void (*function)(sqlite3_context *, int, sqlite3_value **);
  };
  const entry entries[] = {
      {"dq_version", 0, SQLITE_UTF8 | SQLITE_DETERMINISTIC | SQLITE_INNOCUOUS, dq_version},
      {"dq_backend", 0, SQLITE_UTF8, dq_backend},
      {"dq_load", 1, SQLITE_UTF8 | SQLITE_DIRECTONLY, dq_load},
      {"dq_load", 2, SQLITE_UTF8 | SQLITE_DIRECTONLY, dq_load},
      {"noul", 2, inference, noul},
      {"noul", 3, inference, noul},
      {"choice", 3, inference, choice},
      {"score", 3, inference, score},
      // cppcheck-suppress badBitmaskCheck ; result_subtype_flag() is 0 only on older SQLite.
      {"decide", 2, inference | result_subtype_flag(), decide_answers},
  };
  for (const auto &item : entries) {
    const int rc = sqlite3_create_function_v2(db, item.name, item.argc, item.flags, nullptr,
                                              item.function, nullptr, nullptr, nullptr);
    if (rc != SQLITE_OK) return rc;
  }
  return SQLITE_OK;
}
