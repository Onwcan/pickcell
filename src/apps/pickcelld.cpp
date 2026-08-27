// SPDX-License-Identifier: Apache-2.0
//
// pickcelld -- the cell controller as a long-lived service.
//
// Runs the pick-and-place cycle, takes its safety verdict from the safeedge
// runtime over HTTP, and publishes cell state as Prometheus metrics.
//
// The safety link here is a readiness probe against safeedge's /readyz, which
// is how these things are normally wired and is the slow path this repository
// measures. `pickcell-reaction` quantifies what that choice costs; this is the
// choice being costed.

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#include "cell/cell.hpp"
#include "cell/http_poll_link.hpp"
#include "safeedge/edge/http_server.hpp"

namespace {

using namespace pickcell;

volatile std::sig_atomic_t g_shutdown_requested = 0;

extern "C" void onSignal(int) { g_shutdown_requested = 1; }

long environmentLong(const char* name, long fallback) {
  const char* raw = std::getenv(name);
  if (raw == nullptr || *raw == '\0') {
    return fallback;
  }
  char* end = nullptr;
  const long parsed = std::strtol(raw, &end, 10);
  return (end != nullptr && *end == '\0') ? parsed : fallback;
}

std::string environmentString(const char* name, const char* fallback) {
  const char* raw = std::getenv(name);
  return (raw != nullptr && *raw != '\0') ? std::string(raw) : std::string(fallback);
}

void logEvent(const char* level, const std::string& message) {
  std::printf(R"({"level":"%s","msg":"%s"})"
              "\n",
              level, message.c_str());
  std::fflush(stdout);
}

}  // namespace

int main() {
  // PID 1 in a container has no default signal dispositions: without these,
  // `docker compose down` waits ten seconds and then SIGKILLs, and a cell that
  // is SIGKILLed never gets to stop commanding motion.
  std::signal(SIGTERM, onSignal);
  std::signal(SIGINT, onSignal);

  const std::string safety_host = environmentString("PICKCELL_SAFETY_HOST", "127.0.0.1");
  const auto safety_port =
      static_cast<std::uint16_t>(environmentLong("PICKCELL_SAFETY_PORT", 9100));
  const std::string safety_path = environmentString("PICKCELL_SAFETY_PATH", "/safety");

  // Which format the endpoint speaks. `stamped` carries the decision instant and
  // makes the reaction time measurable; `readiness` is the 200/503 probe, kept
  // because plenty of runtimes offer nothing else and the cell must still work
  // against them -- it simply cannot report how long it took.
  const std::string safety_format = environmentString("PICKCELL_SAFETY_FORMAT", "stamped");
  const auto format = safety_format == "readiness"
                          ? HttpPollSafetyLink::Format::kReadinessProbe
                          : HttpPollSafetyLink::Format::kStamped;
  const auto poll_ms = environmentLong("PICKCELL_SAFETY_POLL_MS", 100);
  const auto metrics_port =
      static_cast<std::uint16_t>(environmentLong("PICKCELL_METRICS_PORT", 9200));

  logEvent("info", "polling " + safety_host + ":" + std::to_string(safety_port) +
                       safety_path + " every " + std::to_string(poll_ms) + " ms (" +
                       safety_format + ")");

  HttpPollSafetyLink safety(safety_host, safety_port, safety_path,
                            std::chrono::milliseconds(poll_ms), format);

  Cell::Config config;
  config.period_ns = static_cast<std::uint64_t>(environmentLong("PICKCELL_PERIOD_US", 1000)) * 1000;
  Cell cell(safety, config);

  std::atomic<bool> running{true};
  std::thread control([&] {
    while (running.load(std::memory_order_relaxed)) {
      cell.step(nowNanos());
      std::this_thread::sleep_for(std::chrono::nanoseconds(config.period_ns));
    }
  });

  safeedge::edge::HttpServer server;

  server.route("/healthz", [&running]() {
    safeedge::edge::HttpResponse response;
    response.body = running.load() ? "ok\n" : "stopping\n";
    response.status = running.load() ? 200 : 503;
    return response;
  });

  server.route("/metrics", [&cell, &safety]() {
    safeedge::edge::HttpResponse response;
    response.content_type = "text/plain; version=0.0.4; charset=utf-8";
    std::string body;
    body += "# HELP pickcell_cycles_completed Pick-and-place cycles finished.\n";
    body += "# TYPE pickcell_cycles_completed counter\n";
    body += "pickcell_cycles_completed " + std::to_string(cell.cycles_completed()) + "\n";

    body += "# HELP pickcell_held_by_safety Whether the cell is currently held.\n";
    body += "# TYPE pickcell_held_by_safety gauge\n";
    body += "pickcell_held_by_safety " +
            std::to_string(cell.phase() == CellPhase::kHeldBySafety ? 1 : 0) + "\n";

    // Exported because a cell that has never heard from the safety runtime is
    // not the same as one that has been told it may not move, and a dashboard
    // that cannot tell them apart will read a dead link as a safe machine.
    body += "# HELP pickcell_cycles_without_safety Cycles with no usable safety verdict.\n";
    body += "# TYPE pickcell_cycles_without_safety counter\n";
    body += "pickcell_cycles_without_safety " +
            std::to_string(cell.cycles_without_safety()) + "\n";

    // Separate from cycles_without_safety on purpose. "Never heard from the
    // runtime" and "heard from it, but too long ago to still believe it" are
    // different faults with different causes, and a single counter would hide
    // which one is happening.
    body += "# HELP pickcell_cycles_on_stale_safety Cycles held because the permit expired.\n";
    body += "# TYPE pickcell_cycles_on_stale_safety counter\n";
    body += "pickcell_cycles_on_stale_safety " +
            std::to_string(cell.cycles_on_stale_safety()) + "\n";

    // Measurable only because the endpoint carries the decision instant. Against
    // a plain readiness probe this stays 0 — which does not mean the cell is
    // infinitely fast, it means nothing was measured.
    body += "# HELP pickcell_last_stop_reaction_seconds Decision to cell stop.\n";
    body += "# TYPE pickcell_last_stop_reaction_seconds gauge\n";
    body += "pickcell_last_stop_reaction_seconds " +
            std::to_string(static_cast<double>(cell.last_stop_reaction_ns()) / 1e9) + "\n";

    body += "# HELP pickcell_safety_poll_failures Failed safety polls.\n";
    body += "# TYPE pickcell_safety_poll_failures counter\n";
    body += "pickcell_safety_poll_failures " + std::to_string(safety.requests_failed()) + "\n";

    body += "# HELP pickcell_safety_poll_total Attempted safety polls.\n";
    body += "# TYPE pickcell_safety_poll_total counter\n";
    body += "pickcell_safety_poll_total " + std::to_string(safety.requests_attempted()) + "\n";
    response.body = body;
    return response;
  });

  if (!server.start(metrics_port)) {
    logEvent("fatal", "metrics server failed to start: " + server.error());
    running.store(false);
    control.join();
    return 3;
  }
  logEvent("info", "metrics listening on " + std::to_string(metrics_port));

  while (g_shutdown_requested == 0) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }

  logEvent("info", "shutting down");
  running.store(false);
  control.join();
  server.stop();
  return 0;
}
