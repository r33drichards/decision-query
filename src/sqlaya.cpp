// SQLite extension exposing Laya typed decisions as SQL functions.
//
// One Laya checkpoint stays resident per process. Calls into the resident agent
// are serialized, as the laya runtime requires.
#include "sqlaya.h"

#include "sqlite3ext.h"
SQLITE_EXTENSION_INIT1

#include <cstdlib>
#include <filesystem>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>

#include "laya/runtime.hpp"

#ifndef SQLAYA_CUDA_DEFAULT
#  define SQLAYA_CUDA_DEFAULT 0
#endif

namespace {
  using json = laya::json;

  constexpr unsigned JSON_SUBTYPE = 74;  // 'J', shared with SQLite's json1 functions.
  constexpr const char *NOT_LOADED
      = "No Laya model loaded; call laya_load(dir) or set LAYA_MODEL_DIR";

  struct load_options {
    std::string variant = "english";
    bool cuda = SQLAYA_CUDA_DEFAULT != 0;
    bool bf16 = false, flash = false, tensor_core = false;
  };

  load_options parse_options(const json &value) {
    load_options options;
    if (value.is_null()) return options;
    if (!value.is_object()) throw std::invalid_argument("laya options must be a JSON object");
    for (const auto &[key, item] : value.items()) {
      if (key == "variant") {
        if (!item.is_string()) throw std::invalid_argument("variant must be a string");
        options.variant = item.get<std::string>();
        if (options.variant != "english" && options.variant != "multilingual"
            && options.variant != "typed-decisions")
          throw std::invalid_argument("Unknown model variant: " + options.variant);
        continue;
      }
      bool *flag = key == "cuda"          ? &options.cuda
                   : key == "bf16"        ? &options.bf16
                   : key == "flash"       ? &options.flash
                   : key == "tensor_core" ? &options.tensor_core
                                          : nullptr;
      if (!flag) throw std::invalid_argument("Unknown laya option: " + key);
      if (item.is_boolean())
        *flag = item.get<bool>();
      else if (item.is_number_integer())
        *flag = item.get<long long>() != 0;
      else
        throw std::invalid_argument("laya option " + key + " must be a boolean");
    }
    if (options.bf16) options.flash = true;  // Mirrors the CLI's --bf16 behaviour.
    return options;
  }

  std::filesystem::path checkpoint_path(const std::string &directory, const load_options &options) {
    std::filesystem::path path(directory);
    if (options.variant != "english") path /= options.variant;
    return path;
  }

  struct engine {
    std::mutex mutex;
    std::unique_ptr<laya::agent> agent;
    std::string backend;

    static engine &instance() {
      static engine self;
      return self;
    }

    // Replaces the resident model only once the new one has loaded successfully.
    std::string load(const std::string &directory, const json &option_json) {
      const auto options = parse_options(option_json);
      const auto path = checkpoint_path(directory, options);
      if (!std::filesystem::is_directory(path))
        throw std::invalid_argument("Not a checkpoint directory: " + path.string());
      std::lock_guard<std::mutex> lock(mutex);
      auto fresh = std::make_unique<laya::agent>(path, options.cuda, options.bf16, options.flash,
                                                 options.tensor_core);
      backend = fresh->backend_name();
      agent = std::move(fresh);
      return backend;
    }

    std::string backend_name() {
      std::lock_guard<std::mutex> lock(mutex);
      return agent ? backend : std::string();
    }

    // Runs one request through the resident model and returns its answers object.
    json answers(const json &state, const json &questions) {
      std::lock_guard<std::mutex> lock(mutex);
      if (!agent) load_from_environment();
      const json requests = json::array({json{{"state", state}, {"questions", questions}}});
      return agent->predict(requests).at(0).at("answers");
    }

  private:
    // Called with the mutex held.
    void load_from_environment() {
      const char *directory = std::getenv("LAYA_MODEL_DIR");
      const char *option_text = std::getenv("LAYA_OPTIONS");
      const std::string model = directory && *directory ? directory : "models/laya";
      json option_json;
      if (option_text && *option_text) {
        try {
          option_json = json::parse(option_text);
        } catch (const json::exception &e) {
          throw std::invalid_argument(std::string("LAYA_OPTIONS must be valid JSON: ") + e.what());
        }
      }
      const auto options = parse_options(option_json);
      const auto path = checkpoint_path(model, options);
      if (!std::filesystem::is_directory(path)) throw std::runtime_error(NOT_LOADED);
      auto fresh = std::make_unique<laya::agent>(path, options.cuda, options.bf16, options.flash,
                                                 options.tensor_core);
      backend = fresh->backend_name();
      agent = std::move(fresh);
    }
  };

  std::string text_of(sqlite3_value *value) {
    const unsigned char *text = sqlite3_value_text(value);
    return text ? std::string(reinterpret_cast<const char *>(text), sqlite3_value_bytes(value))
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
      sqlite3_result_error(context, "Unknown Laya error", -1);
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

  void laya_version(sqlite3_context *context, int, sqlite3_value **) {
    sqlite3_result_text(context, SQLITE_LAYA_VERSION, -1, SQLITE_STATIC);
  }

  void laya_backend(sqlite3_context *context, int, sqlite3_value **) {
    guarded(context, [&] {
      const auto backend = engine::instance().backend_name();
      if (backend.empty())
        sqlite3_result_null(context);
      else
        result_text(context, backend);
    });
  }

  void laya_load(sqlite3_context *context, int argc, sqlite3_value **argv) {
    guarded(context, [&] {
      if (sqlite3_value_type(argv[0]) == SQLITE_NULL)
        throw std::invalid_argument("laya_load requires a checkpoint directory");
      json options;
      if (argc > 1 && sqlite3_value_type(argv[1]) != SQLITE_NULL)
        options = parse_json_argument(argv[1], "laya_load options");
      result_text(context, engine::instance().load(text_of(argv[0]), options));
    });
  }

  void laya_noul(sqlite3_context *context, int argc, sqlite3_value **argv) {
    guarded(context, [&] {
      if (any_null(argc, argv)) return sqlite3_result_null(context);
      json question = {{"type", "noul"}, {"instructions", state_or_text(argv[1])}};
      if (argc > 2) question["criteria"] = parse_json_argument(argv[2], "laya_noul criteria");
      sqlite3_result_double(context,
                            single_answer(argv[0], std::move(question)).at("noul").get<double>());
    });
  }

  void laya_choice(sqlite3_context *context, int argc, sqlite3_value **argv) {
    guarded(context, [&] {
      if (any_null(argc, argv)) return sqlite3_result_null(context);
      json question = {{"type", "choice"},
                       {"instructions", state_or_text(argv[1])},
                       {"criteria", parse_json_argument(argv[2], "laya_choice criteria")}};
      result_text(context,
                  single_answer(argv[0], std::move(question)).at("choice").get<std::string>());
    });
  }

  void laya_score(sqlite3_context *context, int argc, sqlite3_value **argv) {
    guarded(context, [&] {
      if (any_null(argc, argv)) return sqlite3_result_null(context);
      json question = {{"type", "score"},
                       {"instructions", state_or_text(argv[1])},
                       {"criteria", parse_json_argument(argv[2], "laya_score criteria")}};
      sqlite3_result_double(context,
                            single_answer(argv[0], std::move(question)).at("score").get<double>());
    });
  }

  void laya_answers(sqlite3_context *context, int argc, sqlite3_value **argv) {
    guarded(context, [&] {
      if (any_null(argc, argv)) return sqlite3_result_null(context);
      const json questions = parse_json_argument(argv[1], "laya questions");
      if (!questions.is_object() || questions.empty())
        throw std::invalid_argument("laya questions must be a nonempty JSON object");
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
    sqlite3_laya_init(sqlite3 *db, char **pzErrMsg, const sqlite3_api_routines *pApi) {
  SQLITE_EXTENSION_INIT2(pApi);
  (void)pzErrMsg;
  const int inference = SQLITE_UTF8 | SQLITE_DETERMINISTIC | subtype_flags();
  struct entry {
    const char *name;
    int argc;
    int flags;
    void (*function)(sqlite3_context *, int, sqlite3_value **);
  };
  const entry entries[] = {
      {"laya_version", 0, SQLITE_UTF8 | SQLITE_DETERMINISTIC | SQLITE_INNOCUOUS, laya_version},
      {"laya_backend", 0, SQLITE_UTF8, laya_backend},
      {"laya_load", 1, SQLITE_UTF8 | SQLITE_DIRECTONLY, laya_load},
      {"laya_load", 2, SQLITE_UTF8 | SQLITE_DIRECTONLY, laya_load},
      {"laya_noul", 2, inference, laya_noul},
      {"laya_noul", 3, inference, laya_noul},
      {"laya_choice", 3, inference, laya_choice},
      {"laya_score", 3, inference, laya_score},
      {"laya", 2, inference | result_subtype_flag(), laya_answers},
  };
  for (const auto &item : entries) {
    const int rc = sqlite3_create_function_v2(db, item.name, item.argc, item.flags, nullptr,
                                              item.function, nullptr, nullptr, nullptr);
    if (rc != SQLITE_OK) return rc;
  }
  return SQLITE_OK;
}
