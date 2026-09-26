// C++ side of the bridge: wraps the shared engine and converts exceptions to
// error strings.
#include "bridge.h"

#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <utility>

#include "decision_engine.hpp"

namespace {
using json = laya::json;

char *copy(const std::string &text) {
  char *result = static_cast<char *>(std::malloc(text.size() + 1));
  if (result)
    std::memcpy(result, text.c_str(), text.size() + 1);
  return result;
}

json parse(const char *text, const std::string &name) {
  try {
    return json::parse(text ? text : "");
  } catch (const json::exception &e) {
    throw std::invalid_argument(name + " must be valid JSON: " + e.what());
  }
}

json value_of(const char *text, int is_json, const std::string &name) {
  return is_json ? parse(text, name) : json(text ? text : "");
}

template <typename Body>
int guarded(char **error, Body &&body) {
  try {
    body();
    return 0;
  } catch (const std::exception &e) {
    *error = copy(e.what());
  } catch (...) {
    *error = copy("Unknown error");
  }
  return 1;
}
}  // namespace

extern "C" int pgdq_evaluate(const pgdq_request *request,
                               pgdq_response *response) {
  *response = pgdq_response{};
  return guarded(&response->error, [&] {
    auto &engine = dq::engine::instance();
    const std::string model_dir = request->model_dir ? request->model_dir : "";
    const std::string options = request->options ? request->options : "";
    const json state =
        value_of(request->state, request->state_is_json, "state");
    if (!request->type) {
      const json questions = parse(request->questions, "questions");
      if (!questions.is_object() || questions.empty())
        throw std::invalid_argument(
            "questions must be a nonempty JSON object");
      response->text =
          copy(engine.answers(state, questions, model_dir, options).dump());
      return;
    }
    const std::string type = request->type;
    json question = {{"type", type},
                     {"instructions",
                      value_of(request->instructions,
                               request->instructions_is_json, "instructions")}};
    if (request->criteria)
      question["criteria"] =
          parse(request->criteria, type + " criteria");
    json questions = json::object();
    questions["q"] = std::move(question);
    const json answer =
        engine.answers(state, questions, model_dir, options).at("q");
    if (type == "choice")
      response->text = copy(answer.at("choice").get<std::string>());
    else
      response->number = answer.at(type).get<double>();
  });
}

extern "C" int pgdq_load(const char *directory, const char *options_json,
                           char **backend, char **error) {
  *backend = nullptr;
  *error = nullptr;
  return guarded(error, [&] {
    if (!directory)
      throw std::invalid_argument("dq_load requires a checkpoint directory");
    json options;
    if (options_json && *options_json)
      options = parse(options_json, "dq_load options");
    *backend = copy(dq::engine::instance().load(directory, options));
  });
}

extern "C" char *pgdq_backend(void) {
  char *result = nullptr;
  guarded(&result, [&] {
    const auto backend = dq::engine::instance().backend_name();
    result = backend.empty() ? nullptr : copy(backend);
  });
  return result;
}

extern "C" void pgdq_free(char *pointer) {
  std::free(pointer);
}
