// MySQL loadable functions (UDFs) exposing Laya typed decisions.
//
// One Laya checkpoint stays resident per server process and is shared by every
// connection; calls into it are serialized, as the laya runtime requires.
//
// A loadable function can only report an error message from its _init
// callback. So each _init does everything that can fail up front: it checks
// the arguments, parses constant JSON arguments, and loads the fallback model
// when none is resident. What can still fail per row (a JSON column that does
// not parse, an unreachable endpoint) returns NULL, logs to the server error
// log and is kept for dq_last_error().
#include <dlfcn.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

#include "decision_engine.hpp"
#include "udf_registration_types.h"

#ifndef MYSQL_DQ_VERSION
#  define MYSQL_DQ_VERSION "dev"
#endif

namespace {
  using json = laya::json;
  using dq::engine;

  constexpr std::size_t ERRMSG_SIZE = 512;       // MYSQL_ERRMSG_SIZE
  constexpr unsigned NOT_FIXED_DEC = 31;         // Print doubles at full precision.
  constexpr unsigned long SHORT_TEXT = 65535;    // choice, dq_load, dq_backend
  constexpr unsigned long LONG_TEXT = 16777215;  // decide

  // The message of the most recent failed call on this server thread. With the
  // default one-thread-per-connection handling, that is this connection.
  // NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
  thread_local std::string last_error;

  // Per-call data hung off UDF_INIT::ptr: the backing store for string results.
  struct call {
    std::string result;
    bool preloaded = false;  // dq_load with constant arguments already ran in _init.
  };

  call &state_of(UDF_INIT *initid) { return *reinterpret_cast<call *>(initid->ptr); }

  char *new_call() { return reinterpret_cast<char *>(std::make_unique<call>().release()); }

  // --- result and argument character sets -------------------------------------
  //
  // String results of a loadable function are binary by default, which MySQL's
  // JSON functions refuse, and arguments arrive in their column's character
  // set. MySQL 8.0.19 and later expose the mysql_udf_metadata service to change
  // both; it is reached through the server's registry, looked up at run time so
  // the module still loads (with binary results) where it is missing.
  using service_status = int;
  using service_handle = void *;
  // Layouts of the server's service structs; unused members keep the offsets.
  // cppcheck-suppress-begin unusedStructMember
  struct registry_service {
    service_status (*acquire)(const char *name, service_handle *out);
    service_status (*acquire_related)(const char *name, service_handle related,
                                      service_handle *out);
    service_status (*release)(service_handle service);
  };
  struct udf_metadata_service {
    service_status (*argument_get)(UDF_ARGS *, const char *, unsigned, void **);
    service_status (*result_get)(UDF_INIT *, const char *, void **);
    service_status (*argument_set)(UDF_ARGS *, const char *, unsigned, void *);
    service_status (*result_set)(UDF_INIT *, const char *, void *);
  };
  // cppcheck-suppress-end unusedStructMember

  const udf_metadata_service *udf_metadata() {
    static const udf_metadata_service *service = [] {
      using acquire_registry = registry_service *(*)();
      // mysql_plugin_registry_acquire(), exported by mysqld with C++ linkage.
      auto *acquire = reinterpret_cast<acquire_registry>(
          dlsym(RTLD_DEFAULT, "_Z29mysql_plugin_registry_acquirev"));
      if (!acquire) return static_cast<const udf_metadata_service *>(nullptr);
      registry_service *registry = acquire();
      service_handle handle = nullptr;
      if (!registry || registry->acquire("mysql_udf_metadata", &handle) != 0 || !handle)
        return static_cast<const udf_metadata_service *>(nullptr);
      // Held for the life of the process, like the model itself.
      return static_cast<const udf_metadata_service *>(handle);
    }();
    return service;
  }

  // Converts every argument to utf8mb4 and, for text results, the result too.
  void use_utf8mb4(UDF_INIT *initid, UDF_ARGS *args, bool text_result) {
    const auto *metadata = udf_metadata();
    if (!metadata) return;
    static std::array<char, 8> charset{"utf8mb4"};
    for (unsigned i = 0; i < args->arg_count; ++i)
      metadata->argument_set(args, "charset", i, charset.data());
    if (text_result) metadata->result_set(initid, "charset", charset.data());
  }

  // --- argument helpers -------------------------------------------------------

  bool is_null(const UDF_ARGS *args, unsigned index) { return args->args[index] == nullptr; }

  bool any_null(const UDF_ARGS *args) {
    for (unsigned i = 0; i < args->arg_count; ++i)
      if (is_null(args, i)) return true;
    return false;
  }

  std::string text_of(const UDF_ARGS *args, unsigned index) {
    return {args->args[index], args->lengths[index]};
  }

  // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
  json parse_json(const std::string &text, const std::string &name) {
    try {
      return json::parse(text);
    } catch (const json::exception &e) {
      throw std::invalid_argument(name + " must be valid JSON: " + e.what());
    }
  }

  // MySQL passes JSON values to a loadable function as plain strings, so a
  // state that is a JSON object is passed to the model as structured data,
  // matching a JSON_OBJECT(...) argument. Anything else is text.
  json state_value(const std::string &text) {
    const auto start = text.find_first_not_of(" \t\r\n");
    if (start != std::string::npos && text[start] == '{') {
      json parsed = json::parse(text, nullptr, false);
      if (parsed.is_object()) return parsed;
    }
    return json(text);  // NOLINT(modernize-return-braced-init-list): braces build an array.
  }

  json questions_value(const std::string &text) {
    json questions = parse_json(text, "questions");
    if (!questions.is_object() || questions.empty())
      throw std::invalid_argument("questions must be a nonempty JSON object");
    return questions;
  }

  // dq_load options from SQL. key_file is refused: every account can call a
  // loadable function, and the option would let one make the server read any
  // file it can open and send the contents to an endpoint of its choosing.
  json load_options(const UDF_ARGS *args) {
    if (args->arg_count < 2 || is_null(args, 1)) return {};
    json options = parse_json(text_of(args, 1), "dq_load options");
    if (options.is_object() && options.contains("key_file"))
      throw std::invalid_argument(
          "key_file is not accepted from SQL on MySQL; set DQ_API_KEY_FILE in the "
          "server's environment instead");
    return options;
  }

  // --- error handling ---------------------------------------------------------

  // Copies a message into the buffer MySQL gives an _init callback.
  void set_message(char *message, const std::string &text) {
    const std::size_t size = std::min(text.size(), ERRMSG_SIZE - 1);
    std::memcpy(message, text.data(), size);
    message[size] = '\0';
  }

  // Runs an _init body; on failure writes the message for the client and
  // returns true, as MySQL expects.
  template <typename Body> bool init_guard(char *message, Body &&body) {
    try {
      std::forward<Body>(body)();
      return false;
    } catch (const std::exception &e) {
      set_message(message, e.what());
    } catch (...) {
      set_message(message, "Unknown error");
    }
    return true;
  }

  // Runs a per-row body; on failure the row's value is NULL and the message
  // goes to the error log and dq_last_error(). MySQL does not call the function
  // again for the rest of the statement.
  template <typename Body> void row_guard(const char *function, unsigned char *error, Body &&body) {
    try {
      std::forward<Body>(body)();
    } catch (const std::exception &e) {
      last_error = std::string(function) + ": " + e.what();
      *error = 1;
    } catch (...) {
      last_error = std::string(function) + ": Unknown error";
      *error = 1;
    }
    if (*error) std::cerr << "decision_query: " << last_error << '\n';
  }

  // Shared _init for the inference functions.
  bool init_inference(UDF_INIT *initid, UDF_ARGS *args, char *message, const char *name,
                      unsigned min_args, unsigned max_args, bool text_result,
                      unsigned long max_length) {
    return init_guard(message, [&] {
      if (args->arg_count < min_args || args->arg_count > max_args) {
        const std::string arity
            = min_args == max_args ? std::to_string(min_args)
                                   : std::to_string(min_args) + " or " + std::to_string(max_args);
        throw std::invalid_argument(std::string(name) + "() takes " + arity + " arguments");
      }
      for (unsigned i = 0; i < args->arg_count; ++i) args->arg_type[i] = STRING_RESULT;
      use_utf8mb4(initid, args, text_result);
      // Constant JSON arguments are checked now, where the error can be reported.
      if (std::strcmp(name, "decide") == 0) {
        if (!is_null(args, 1)) questions_value(text_of(args, 1));
      } else if (args->arg_count > 2 && !is_null(args, 2)) {
        parse_json(text_of(args, 2), std::string(name) + " criteria");
      }
      engine::instance().ensure_loaded();
      initid->maybe_null = true;
      initid->const_item = false;
      initid->decimals = NOT_FIXED_DEC;
      initid->max_length = max_length;
      initid->ptr = new_call();
    });
  }

  void deinit(UDF_INIT *initid) {
    std::unique_ptr<call>(reinterpret_cast<call *>(initid->ptr)).reset();
    initid->ptr = nullptr;
  }

  char *return_text(UDF_INIT *initid, std::string text, unsigned long *length) {
    auto &state = state_of(initid);
    state.result = std::move(text);
    *length = state.result.size();
    return state.result.data();
  }

  // Evaluates one question against a state and returns its answer object.
  json single_answer(const UDF_ARGS *args, const char *type) {
    json question = {{"type", type}, {"instructions", text_of(args, 1)}};
    if (args->arg_count > 2)
      question["criteria"] = parse_json(text_of(args, 2), std::string(type) + " criteria");
    json questions = json::object();
    questions["q"] = std::move(question);
    return engine::instance().answers(state_value(text_of(args, 0)), questions).at("q");
  }
}  // namespace

#define DQ_EXPORT extern "C" __attribute__((visibility("default")))

// The signatures below are fixed by MySQL's loadable-function interface.
// NOLINTBEGIN(bugprone-easily-swappable-parameters,readability-named-parameter,hicpp-named-parameter)

// --- dq_version() ---------------------------------------------------------------

DQ_EXPORT bool dq_version_init(UDF_INIT *initid, UDF_ARGS *args, char *message) {
  if (args->arg_count != 0) {
    set_message(message, "dq_version() takes no arguments");
    return true;
  }
  use_utf8mb4(initid, args, true);
  initid->maybe_null = false;
  initid->const_item = true;
  initid->max_length = sizeof(MYSQL_DQ_VERSION) - 1;
  return false;
}

DQ_EXPORT char *dq_version(UDF_INIT *, UDF_ARGS *, char *, unsigned long *length, unsigned char *,
                           unsigned char *) {
  static std::string version = MYSQL_DQ_VERSION;
  *length = version.size();
  return version.data();
}

// --- dq_backend() ---------------------------------------------------------------

DQ_EXPORT bool dq_backend_init(UDF_INIT *initid, UDF_ARGS *args, char *message) {
  return init_guard(message, [&] {
    if (args->arg_count != 0) throw std::invalid_argument("dq_backend() takes no arguments");
    use_utf8mb4(initid, args, true);
    initid->maybe_null = true;
    initid->const_item = false;
    initid->max_length = SHORT_TEXT;
    initid->ptr = new_call();
  });
}

DQ_EXPORT void dq_backend_deinit(UDF_INIT *initid) { deinit(initid); }

DQ_EXPORT char *dq_backend(UDF_INIT *initid, UDF_ARGS *, char *, unsigned long *length,
                           unsigned char *is_null_result, unsigned char *error) {
  char *result = nullptr;
  row_guard("dq_backend", error, [&] {
    auto backend = engine::instance().backend_name();
    if (backend.empty())
      *is_null_result = 1;
    else
      result = return_text(initid, std::move(backend), length);
  });
  return result;
}

// --- dq_last_error() ------------------------------------------------------------

DQ_EXPORT bool dq_last_error_init(UDF_INIT *initid, UDF_ARGS *args, char *message) {
  return init_guard(message, [&] {
    if (args->arg_count != 0) throw std::invalid_argument("dq_last_error() takes no arguments");
    use_utf8mb4(initid, args, true);
    initid->maybe_null = true;
    initid->const_item = false;
    initid->max_length = SHORT_TEXT;
    initid->ptr = new_call();
  });
}

DQ_EXPORT void dq_last_error_deinit(UDF_INIT *initid) { deinit(initid); }

DQ_EXPORT char *dq_last_error(UDF_INIT *initid, UDF_ARGS *, char *, unsigned long *length,
                              unsigned char *is_null_result, unsigned char *) {
  if (last_error.empty()) {
    *is_null_result = 1;
    return nullptr;
  }
  return return_text(initid, last_error, length);
}

// --- dq_load(directory_or_url [, options]) ---------------------------------------

DQ_EXPORT bool dq_load_init(UDF_INIT *initid, UDF_ARGS *args, char *message) {
  return init_guard(message, [&] {
    if (args->arg_count < 1 || args->arg_count > 2)
      throw std::invalid_argument("dq_load() takes 1 or 2 arguments");
    for (unsigned i = 0; i < args->arg_count; ++i) args->arg_type[i] = STRING_RESULT;
    use_utf8mb4(initid, args, true);
    auto state = std::make_unique<call>();
    // With constant arguments, load now so a failure is reported as an error
    // rather than a NULL.
    const bool constant = !is_null(args, 0) && (args->arg_count < 2 || !is_null(args, 1));
    if (constant) {
      state->result = engine::instance().load(text_of(args, 0), load_options(args));
      state->preloaded = true;
    }
    initid->maybe_null = true;
    initid->const_item = false;
    initid->max_length = SHORT_TEXT;
    initid->ptr = reinterpret_cast<char *>(state.release());
  });
}

DQ_EXPORT void dq_load_deinit(UDF_INIT *initid) { deinit(initid); }

DQ_EXPORT char *dq_load(UDF_INIT *initid, UDF_ARGS *args, char *, unsigned long *length,
                        unsigned char *is_null_result, unsigned char *error) {
  if (state_of(initid).preloaded) return return_text(initid, state_of(initid).result, length);
  char *result = nullptr;
  row_guard("dq_load", error, [&] {
    if (is_null(args, 0)) throw std::invalid_argument("dq_load requires a checkpoint directory");
    result = return_text(initid, engine::instance().load(text_of(args, 0), load_options(args)),
                         length);
  });
  if (*error) *is_null_result = 1;
  return result;
}

// --- noul(state, instructions [, criteria]) ---------------------------------------

DQ_EXPORT bool noul_init(UDF_INIT *initid, UDF_ARGS *args, char *message) {
  return init_inference(initid, args, message, "noul", 2, 3, false, 0);
}

DQ_EXPORT void noul_deinit(UDF_INIT *initid) { deinit(initid); }

// cppcheck-suppress constParameterPointer ; the signature is MySQL's.
DQ_EXPORT double noul(UDF_INIT *, UDF_ARGS *args, unsigned char *is_null_result,
                      unsigned char *error) {
  double result = 0;
  if (any_null(args)) {
    *is_null_result = 1;
    return result;
  }
  row_guard("noul", error, [&] { result = single_answer(args, "noul").at("noul").get<double>(); });
  return result;
}

// --- score(state, instructions, criteria) -----------------------------------------

DQ_EXPORT bool score_init(UDF_INIT *initid, UDF_ARGS *args, char *message) {
  return init_inference(initid, args, message, "score", 3, 3, false, 0);
}

DQ_EXPORT void score_deinit(UDF_INIT *initid) { deinit(initid); }

// cppcheck-suppress constParameterPointer ; the signature is MySQL's.
DQ_EXPORT double score(UDF_INIT *, UDF_ARGS *args, unsigned char *is_null_result,
                       unsigned char *error) {
  double result = 0;
  if (any_null(args)) {
    *is_null_result = 1;
    return result;
  }
  row_guard("score", error,
            [&] { result = single_answer(args, "score").at("score").get<double>(); });
  return result;
}

// --- choice(state, instructions, criteria) ----------------------------------------

DQ_EXPORT bool choice_init(UDF_INIT *initid, UDF_ARGS *args, char *message) {
  return init_inference(initid, args, message, "choice", 3, 3, true, SHORT_TEXT);
}

DQ_EXPORT void choice_deinit(UDF_INIT *initid) { deinit(initid); }

// cppcheck-suppress constParameterPointer ; the signature is MySQL's.
DQ_EXPORT char *choice(UDF_INIT *initid, UDF_ARGS *args, char *, unsigned long *length,
                       unsigned char *is_null_result, unsigned char *error) {
  char *result = nullptr;
  if (any_null(args)) {
    *is_null_result = 1;
    return result;
  }
  row_guard("choice", error, [&] {
    result = return_text(initid, single_answer(args, "choice").at("choice").get<std::string>(),
                         length);
  });
  if (*error) *is_null_result = 1;
  return result;
}

// --- decide(state, questions) -----------------------------------------------------

DQ_EXPORT bool decide_init(UDF_INIT *initid, UDF_ARGS *args, char *message) {
  return init_inference(initid, args, message, "decide", 2, 2, true, LONG_TEXT);
}

DQ_EXPORT void decide_deinit(UDF_INIT *initid) { deinit(initid); }

// cppcheck-suppress constParameterPointer ; the signature is MySQL's.
DQ_EXPORT char *decide(UDF_INIT *initid, UDF_ARGS *args, char *, unsigned long *length,
                       unsigned char *is_null_result, unsigned char *error) {
  char *result = nullptr;
  if (any_null(args)) {
    *is_null_result = 1;
    return result;
  }
  row_guard("decide", error, [&] {
    const json questions = questions_value(text_of(args, 1));
    result = return_text(
        initid, engine::instance().answers(state_value(text_of(args, 0)), questions).dump(),
        length);
  });
  if (*error) *is_null_result = 1;
  return result;
}
// NOLINTEND(bugprone-easily-swappable-parameters,readability-named-parameter,hicpp-named-parameter)
