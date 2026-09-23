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
#include <httplib.h>

#ifndef SQLAYA_CUDA_DEFAULT
#  define SQLAYA_CUDA_DEFAULT 0
#endif

namespace sqlaya {
  using json = laya::json;

  constexpr const char *NOT_LOADED
      = "No Laya model loaded; call laya_load(dir) or set LAYA_MODEL_DIR";

  struct load_options {
    std::string variant = "english";
    std::string key, model;
    bool cuda = SQLAYA_CUDA_DEFAULT != 0;
    bool metal = SQLAYA_METAL_DEFAULT != 0;
    bool bf16 = false, flash = false, tensor_core = false;
  };

  load_options parse_options(const json &value) {
    load_options options;
    if (value.is_null()) return options;
    if (!value.is_object()) throw std::invalid_argument("laya options must be a JSON object");
    for (const auto &[key, item] : value.items()) {
      if (key == "key" || key == "model") {
        if (!item.is_string()) throw std::invalid_argument(key + " must be a string");
        (key == "key" ? options.key : options.model) = item.get<std::string>();
        continue;
      }
      if (key == "variant") {
        if (!item.is_string()) throw std::invalid_argument("variant must be a string");
        options.variant = item.get<std::string>();
        if (options.variant != "english" && options.variant != "multilingual"
            && options.variant != "typed-decisions")
          throw std::invalid_argument("Unknown model variant: " + options.variant);
        continue;
      }
      bool *flag = key == "cuda"          ? &options.cuda
                   : key == "metal"       ? &options.metal
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

  // A decision backend answers typed questions about a state. The local backend
  // runs a Laya checkpoint in-process; the HTTP backend forwards to any service
  // speaking the System One request shape (TypeSafe Jev, reflex, djev).
  struct backend_base {
    virtual ~backend_base() = default;
    virtual json predict(const json &requests) = 0;
    virtual std::string name() const = 0;
  };

  struct local_backend : backend_base {
    std::unique_ptr<laya::agent> agent;
    std::string backend_name_;
    local_backend(const std::filesystem::path &path, const load_options &o)
        : agent(std::make_unique<laya::agent>(path, o.cuda, o.bf16, o.flash, o.tensor_core, o.metal)),
          backend_name_(agent->backend_name()) {}
    json predict(const json &requests) override { return agent->predict(requests); }
    std::string name() const override { return backend_name_; }
  };

  struct http_backend : backend_base {
    std::string url, key, model, host, path;
    std::string reported = "remote";
    http_backend(std::string endpoint, std::string api_key, std::string model_name)
        : url(std::move(endpoint)), key(std::move(api_key)), model(std::move(model_name)) {
      // split scheme://host[:port]/path
      auto scheme = url.find("://");
      if (scheme == std::string::npos) throw std::invalid_argument("Endpoint must be a URL: " + url);
      auto rest = url.find('/', scheme + 3);
      host = rest == std::string::npos ? url : url.substr(0, rest);
      path = rest == std::string::npos ? "/" : url.substr(rest);
    }
    json predict(const json &requests) override {
      // One state per call: the System One shape is a single state plus questions.
      const auto &r = requests.at(0);
      json body{{"state", r.at("state")}, {"questions", r.at("questions")}};
      // The System One shape requires a model name. "jev-latest" is the reference
      // implementation's default; reflex/djev users override with the model option.
      body["model"] = model.empty() ? std::string("jev-latest") : model;
      httplib::Client cli(host.c_str());
      cli.set_read_timeout(120, 0);
      cli.set_connection_timeout(30, 0);
      // Content-Type is supplied by Post()'s final argument; setting it here too
      // sends the header twice and the server parses the body as a string.
      httplib::Headers headers{{"User-Agent", "sqlaya/" SQLITE_LAYA_VERSION}};
      if (!key.empty()) headers.emplace("Authorization", "Bearer " + key);
      auto res = cli.Post(path.c_str(), headers, body.dump(), "application/json");
      if (!res)
        throw std::runtime_error("Decision endpoint unreachable: " + url);
      if (res->status < 200 || res->status >= 300)
        throw std::runtime_error("Decision endpoint returned HTTP " + std::to_string(res->status) +
                                 ": " + res->body.substr(0, 200));
      json parsed = json::parse(res->body);
      if (parsed.contains("model") && parsed["model"].is_string())
        reported = parsed["model"].get<std::string>();
      if (!parsed.contains("answers"))
        throw std::runtime_error("Decision endpoint returned no answers object");
      return json::array({json{{"answers", parsed.at("answers")}}});
    }
    std::string name() const override { return reported; }
  };

  struct engine {
    std::mutex mutex;
    std::unique_ptr<backend_base> agent;
    std::string backend;

    static engine &instance() {
      static engine self;
      return self;
    }

    // Replaces the resident model only once the new one has loaded successfully.
    std::string load(const std::string &directory, const json &option_json) {
      const auto options = parse_options(option_json);
      std::unique_ptr<backend_base> fresh;
      if (directory.rfind("http://", 0) == 0 || directory.rfind("https://", 0) == 0) {
        // Credentials come from the environment by default so they stay out of
        // SQL text, and therefore out of shell history.
        std::string key = options.key;
        if (key.empty())
          if (const char *env = std::getenv("LAYA_API_KEY")) key = env;
        fresh = std::make_unique<http_backend>(directory, key, options.model);
      } else {
        const auto path = checkpoint_path(directory, options);
        if (!std::filesystem::is_directory(path))
          throw std::invalid_argument("Not a checkpoint directory: " + path.string());
        fresh = std::make_unique<local_backend>(path, options);
      }
      std::lock_guard<std::mutex> lock(mutex);
      backend = fresh->name();
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
      std::unique_ptr<backend_base> fresh;
      if (model.rfind("http://", 0) == 0 || model.rfind("https://", 0) == 0) {
        std::string k = options.key;
        if (k.empty())
          if (const char *env = std::getenv("LAYA_API_KEY")) k = env;
        fresh = std::make_unique<http_backend>(model, k, options.model);
      } else {
        const auto path = checkpoint_path(model, options);
        if (!std::filesystem::is_directory(path)) throw std::runtime_error(NOT_LOADED);
        fresh = std::make_unique<local_backend>(path, options);
      }
      backend = fresh->name();
      agent = std::move(fresh);
    }
  };
}  // namespace sqlaya
