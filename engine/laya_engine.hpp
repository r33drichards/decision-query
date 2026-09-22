// Shared model engine for the SQLite and PostgreSQL extensions.
//
// One Laya checkpoint stays resident per process. Calls into the resident agent
// are serialized, as the laya runtime requires.
#pragma once

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

namespace sqlaya {
  using json = laya::json;

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
    // Loads from the fallback directory and options, then the environment, when
    // no model is resident.
    json answers(const json &state, const json &questions, const std::string &fallback_dir = {},
                 const std::string &fallback_options = {}) {
      std::lock_guard<std::mutex> lock(mutex);
      if (!agent) load_fallback(fallback_dir, fallback_options);
      const json requests = json::array({json{{"state", state}, {"questions", questions}}});
      return agent->predict(requests).at(0).at("answers");
    }

  private:
    // Called with the mutex held.
    void load_fallback(const std::string &fallback_dir, const std::string &fallback_options) {
      const char *directory = std::getenv("LAYA_MODEL_DIR");
      const char *option_text = std::getenv("LAYA_OPTIONS");
      const std::string model = !fallback_dir.empty()     ? fallback_dir
                                : directory && *directory ? std::string(directory)
                                                          : std::string("models/laya");
      const std::string options_text = !fallback_options.empty()     ? fallback_options
                                       : option_text && *option_text ? std::string(option_text)
                                                                     : std::string();
      json option_json;
      if (!options_text.empty()) {
        try {
          option_json = json::parse(options_text);
        } catch (const json::exception &e) {
          throw std::invalid_argument(std::string("Model options must be valid JSON: ") + e.what());
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
}  // namespace sqlaya
