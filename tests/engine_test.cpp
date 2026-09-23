// Unit tests for the option parsing and key handling in engine/decision_engine.hpp.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <stdexcept>

#include "decision_engine.hpp"

using Catch::Matchers::ContainsSubstring;
using dq::json;

TEST_CASE("parse_options defaults on null", "[engine]") {
  const auto options = dq::parse_options(json());
  CHECK(options.variant == "english");
  CHECK(options.key.empty());
  CHECK_FALSE(options.bf16);
  CHECK_FALSE(options.flash);
}

TEST_CASE("parse_options reads every key", "[engine]") {
  const auto options = dq::parse_options(json::parse(R"({
    "variant": "multilingual", "key": "k", "model": "m", "key_file": "f",
    "cuda": false, "metal": 0, "bf16": true, "tensor_core": 1})"));
  CHECK(options.variant == "multilingual");
  CHECK(options.key == "k");
  CHECK(options.model == "m");
  CHECK(options.key_file == "f");
  CHECK_FALSE(options.cuda);
  CHECK_FALSE(options.metal);
  CHECK(options.bf16);
  CHECK(options.flash);  // bf16 implies flash, as in the CLI.
  CHECK(options.tensor_core);
}

TEST_CASE("parse_options rejects malformed input", "[engine]") {
  CHECK_THROWS_WITH(dq::parse_options(json::array()), ContainsSubstring("must be a JSON object"));
  CHECK_THROWS_WITH(dq::parse_options(json::parse(R"({"gpu": true})")),
                    ContainsSubstring("Unknown option: gpu"));
  CHECK_THROWS_WITH(dq::parse_options(json::parse(R"({"variant": "french"})")),
                    ContainsSubstring("Unknown model variant"));
  CHECK_THROWS_WITH(dq::parse_options(json::parse(R"({"variant": 1})")),
                    ContainsSubstring("variant must be a string"));
  CHECK_THROWS_WITH(dq::parse_options(json::parse(R"({"key": 1})")),
                    ContainsSubstring("key must be a string"));
  CHECK_THROWS_WITH(dq::parse_options(json::parse(R"({"bf16": "yes"})")),
                    ContainsSubstring("must be a boolean"));
}

TEST_CASE("checkpoint_path appends non-english variants", "[engine]") {
  dq::load_options options;
  CHECK(dq::checkpoint_path("models/laya", options) == std::filesystem::path("models/laya"));
  options.variant = "typed-decisions";
  CHECK(dq::checkpoint_path("models/laya", options)
        == std::filesystem::path("models/laya/typed-decisions"));
}

TEST_CASE("read_key_file trims trailing whitespace", "[engine]") {
  const auto path = std::filesystem::temp_directory_path() / "dq_tests_key";
  {
    std::ofstream out(path);
    out << "secret \t\r\n";
  }
  CHECK(dq::read_key_file(path.string()) == "secret");
  {
    std::ofstream out(path);
    out << "\n\n";
  }
  CHECK_THROWS_WITH(dq::read_key_file(path.string()), ContainsSubstring("key file is empty"));
  std::filesystem::remove(path);
  CHECK_THROWS_WITH(dq::read_key_file(path.string()), ContainsSubstring("Cannot read key file"));
}

TEST_CASE("engine::load rejects a missing checkpoint and keeps no backend", "[engine]") {
  dq::engine engine;
  CHECK_THROWS_WITH(engine.load("/nonexistent/checkpoint", json()),
                    ContainsSubstring("Not a checkpoint directory"));
  CHECK(engine.backend_name().empty());
}

TEST_CASE("http_backend splits the endpoint URL", "[engine]") {
  const dq::http_backend with_path("https://example.com:8443/v1/decide", "", "");
  CHECK(with_path.host == "https://example.com:8443");
  CHECK(with_path.path == "/v1/decide");
  const dq::http_backend bare("http://example.com", "", "");
  CHECK(bare.host == "http://example.com");
  CHECK(bare.path == "/");
  CHECK_THROWS_WITH(dq::http_backend("example.com", "", ""), ContainsSubstring("must be a URL"));
}
