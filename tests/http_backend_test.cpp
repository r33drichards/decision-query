// Exercises the HTTP decision backend end to end against an in-process server,
// the one inference path that needs no checkpoint.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <string>
#include <thread>
#include <utility>

#include "decision_engine.hpp"

using Catch::Matchers::ContainsSubstring;
using dq::json;

namespace {
  // A System One endpoint on an ephemeral localhost port that records the last request.
  struct fake_endpoint {
    httplib::Server server;
    std::thread thread;
    int port = 0;
    json last_body;
    std::string last_authorization;
    int status = 200;
    std::string response = R"({"model": "fake-1", "answers": {"q": {"noul": 0.75}}})";

    fake_endpoint() {
      server.Post("/decide", [this](const httplib::Request &request, httplib::Response &reply) {
        last_body = json::parse(request.body);
        last_authorization = request.get_header_value("Authorization");
        reply.status = status;
        reply.set_content(response, "application/json");
      });
      port = server.bind_to_any_port("127.0.0.1");
      thread = std::thread([this] { server.listen_after_bind(); });
      server.wait_until_ready();
    }
    ~fake_endpoint() {
      server.stop();
      thread.join();
    }
    fake_endpoint(const fake_endpoint &) = delete;
    fake_endpoint &operator=(const fake_endpoint &) = delete;
    fake_endpoint(fake_endpoint &&) = delete;
    fake_endpoint &operator=(fake_endpoint &&) = delete;

    // Sets what the next request is answered with.
    void reply_with(int code, std::string body) {
      status = code;
      response = std::move(body);
    }

    std::string url() const { return "http://127.0.0.1:" + std::to_string(port) + "/decide"; }
  };

  json questions() { return json::parse(R"({"q": {"type": "noul", "instructions": "Refund?"}})"); }
}  // namespace

TEST_CASE("http backend round trip", "[http]") {
  fake_endpoint endpoint;
  dq::engine engine;
  CHECK(engine.load(endpoint.url(), json::parse(R"({"key": "sekrit"})")) == "remote");

  const json answers = engine.answers("Please refund me.", questions());
  CHECK(answers.at("q").at("noul").get<double>() == 0.75);
  // The backend records the model the endpoint reports; engine::backend_name()
  // still returns the name cached at load time.
  CHECK(engine.agent->name() == "fake-1");
  CHECK(endpoint.last_authorization == "Bearer sekrit");
  CHECK(endpoint.last_body.at("state") == "Please refund me.");
  CHECK(endpoint.last_body.at("questions") == questions());
  CHECK(endpoint.last_body.at("model") == "jev-latest");
}

TEST_CASE("http backend forwards the model option", "[http]") {
  fake_endpoint endpoint;
  dq::engine engine;
  engine.load(endpoint.url(), json::parse(R"({"model": "reflex-small"})"));
  engine.answers("state", questions());
  CHECK(endpoint.last_body.at("model") == "reflex-small");
  CHECK(endpoint.last_authorization.empty());
}

TEST_CASE("http backend reports endpoint failures", "[http]") {
  fake_endpoint endpoint;
  dq::engine engine;
  engine.load(endpoint.url(), json());

  endpoint.reply_with(503, "overloaded");
  CHECK_THROWS_WITH(engine.answers("state", questions()),
                    ContainsSubstring("returned HTTP 503: overloaded"));

  endpoint.reply_with(200, R"({"model": "fake-1"})");
  CHECK_THROWS_WITH(engine.answers("state", questions()), ContainsSubstring("no answers object"));

  endpoint.reply_with(200, "not json");
  CHECK_THROWS_AS(engine.answers("state", questions()), json::parse_error);
}

TEST_CASE("http backend reports an unreachable endpoint", "[http]") {
  int port = 0;
  {
    const fake_endpoint closed;
    port = closed.port;
  }
  dq::engine engine;
  engine.load("http://127.0.0.1:" + std::to_string(port) + "/decide", json());
  CHECK_THROWS_WITH(engine.answers("state", questions()), ContainsSubstring("unreachable"));
}
