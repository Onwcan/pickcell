// SPDX-License-Identifier: Apache-2.0
//
// How long does the cell take to stop after the safety runtime says stop?
//
// The answer is not a property of the safety runtime. It is a property of the
// link between them, and this measures both links the cell can be built with,
// against the same source, on the same machine, in the same process.
//
// One safety source stamps the instant it decides. Both links carry that stamp
// through unchanged, so the number reported is the true end-to-end interval
// from decision to reaction -- not a round-trip time, and not a poll latency
// measured from the poll.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "cell/cell.hpp"
#include "cell/http_poll_link.hpp"
#include "cell/safety_link.hpp"
#include "cell/shared_memory_link.hpp"
#include "safeedge/edge/http_server.hpp"

namespace {

using namespace pickcell;

constexpr const char* kRegionName = "/pickcell-reaction-bench";

/// Stands in for the safety runtime.
///
/// Publishes the same state two ways at once -- into a shared-memory seqlock
/// and out of an HTTP endpoint -- so the two links are reading one decision
/// rather than two. Measuring them against separate sources would compare the
/// sources as much as the links.
class SafetySource {
 public:
  explicit SafetySource(std::uint16_t port) : shm_(SharedMemorySafetyLink::create(kRegionName)) {
    current_.sequence = 1;
    current_.torque_permitted = 1;
    current_.asserted_monotonic_ns = nowNanos();
    publish();

    server_.route("/safety", [this]() {
      safeedge::edge::HttpResponse response;
      response.content_type = "text/plain";
      // Read under the same discipline the seqlock gives the other link.
      const std::uint64_t sequence = http_sequence_.load(std::memory_order_acquire);
      const std::uint64_t asserted = http_asserted_.load(std::memory_order_relaxed);
      const std::uint32_t permitted = http_permitted_.load(std::memory_order_relaxed);
      response.body = std::to_string(sequence) + " " + std::to_string(permitted) + " " +
                      std::to_string(asserted);
      return response;
    });
    started_ = server_.start(port);

    // The writer must keep saying so. Without this the reader sees the publish
    // stamp stop advancing and correctly concludes the source is gone -- the
    // cell would hold, and the benchmark would measure nothing.
    heartbeat_ = std::thread([this] {
      while (heartbeat_running_.load(std::memory_order_relaxed)) {
        shm_.heartbeat();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
      }
    });
  }

  ~SafetySource() {
    heartbeat_running_.store(false, std::memory_order_relaxed);
    if (heartbeat_.joinable()) {
      heartbeat_.join();
    }
  }

  SafetySource(const SafetySource&) = delete;
  SafetySource& operator=(const SafetySource&) = delete;
  SafetySource(SafetySource&&) = delete;
  SafetySource& operator=(SafetySource&&) = delete;

  bool started() const { return started_ && shm_.valid(); }
  const std::string& error() const { return shm_.error(); }

  /// Asserts a stop, stamped at the instant of the decision.
  std::uint64_t assertStop() {
    const std::uint64_t at = nowNanos();
    current_.sequence += 1;
    current_.torque_permitted = 0;
    current_.asserted_monotonic_ns = at;
    publish();
    return at;
  }

  void permit() {
    current_.sequence += 1;
    current_.torque_permitted = 1;
    current_.asserted_monotonic_ns = nowNanos();
    publish();
  }

  std::uint64_t sequence() const { return current_.sequence; }

 private:
  void publish() {
    // HTTP fields first, then the shared-memory slot, then the sequence that
    // gates the HTTP read. Ordering here is not decorative: publishing the
    // sequence last means an HTTP reader never sees a new sequence paired with
    // an old timestamp, which would understate the measured interval.
    http_asserted_.store(current_.asserted_monotonic_ns, std::memory_order_relaxed);
    http_permitted_.store(current_.torque_permitted, std::memory_order_relaxed);
    http_sequence_.store(current_.sequence, std::memory_order_release);
    shm_.publish(current_);
  }

  SharedMemorySafetyLink shm_;
  safeedge::edge::HttpServer server_;
  SafetySignal current_{};
  std::atomic<std::uint64_t> http_sequence_{0};
  std::atomic<std::uint64_t> http_asserted_{0};
  std::atomic<std::uint32_t> http_permitted_{1};
  bool started_ = false;
  std::atomic<bool> heartbeat_running_{true};
  std::thread heartbeat_;
};

struct Summary {
  std::string link;
  std::vector<std::uint64_t> samples;

  std::uint64_t percentile(double fraction) const {
    if (samples.empty()) {
      return 0;
    }
    std::vector<std::uint64_t> sorted = samples;
    std::sort(sorted.begin(), sorted.end());
    const auto index = static_cast<std::size_t>(fraction * static_cast<double>(sorted.size() - 1));
    return sorted[index];
  }
};

/// Runs the cell against one link and trips the safety source repeatedly.
Summary measure(SafetySource& source, SafetyLink& link, int trials,
                std::chrono::milliseconds settle) {
  Summary summary;
  summary.link = std::string(link.name());

  Cell::Config config;
  config.period_ns = 1'000'000;  // 1 kHz cell control loop
  Cell cell(link, config);

  std::atomic<bool> running{true};
  std::thread control([&] {
    while (running.load(std::memory_order_relaxed)) {
      cell.step(nowNanos());
      std::this_thread::sleep_for(std::chrono::microseconds(config.period_ns / 1000));
    }
  });

  // The trip instant is randomised within the settle window. Tripping at a
  // fixed offset would phase-lock with the poller and report whichever point of
  // the poll interval was chosen -- a number that could be made to say anything.
  std::mt19937 rng(0xC0FFEE);

  for (int trial = 0; trial < trials; ++trial) {
    source.permit();
    std::this_thread::sleep_for(settle);

    std::uniform_int_distribution<int> jitter(0, static_cast<int>(settle.count()));
    std::this_thread::sleep_for(std::chrono::milliseconds(jitter(rng)));

    const std::uint64_t before = cell.last_stop_reaction_ns();
    source.assertStop();

    // Wait for the cell to notice, with a generous ceiling so a broken link
    // fails the run rather than hanging it.
    const auto deadline = Clock::now() + std::chrono::seconds(5);
    while (Clock::now() < deadline) {
      if (cell.has_stop_measurement() && cell.last_stop_reaction_ns() != before) {
        break;
      }
      std::this_thread::sleep_for(std::chrono::microseconds(200));
    }

    if (cell.has_stop_measurement() && cell.last_stop_reaction_ns() != before) {
      summary.samples.push_back(cell.last_stop_reaction_ns());
    } else {
      std::printf("  trial %d: no reaction within 5 s\n", trial);
    }
  }

  running.store(false);
  control.join();
  return summary;
}

void report(const Summary& summary) {
  if (summary.samples.empty()) {
    std::printf("%-28s no samples\n", summary.link.c_str());
    return;
  }
  const auto us = [](std::uint64_t ns) { return static_cast<double>(ns) / 1000.0; };
  std::printf("%-28s n=%-4zu  min %9.1f  p50 %9.1f  p99 %9.1f  max %9.1f\n",
              summary.link.c_str(), summary.samples.size(),
              us(summary.percentile(0.0)), us(summary.percentile(0.50)),
              us(summary.percentile(0.99)), us(summary.percentile(1.0)));
}

}  // namespace

int main(int argc, char** argv) {
  int trials = 30;
  int poll_ms = 100;
  for (int i = 1; i + 1 < argc; i += 2) {
    if (std::strcmp(argv[i], "--trials") == 0) {
      trials = std::atoi(argv[i + 1]);
    } else if (std::strcmp(argv[i], "--poll-ms") == 0) {
      poll_ms = std::atoi(argv[i + 1]);
    }
  }
  if (trials <= 0 || poll_ms <= 0) {
    std::printf("usage: pickcell-reaction [--trials N] [--poll-ms M]\n");
    return 2;
  }

  constexpr std::uint16_t kPort = 19110;
  SafetySource source(kPort);
  if (!source.started()) {
    std::printf("could not start the safety source: %s\n", source.error().c_str());
    std::printf("shared memory needs /dev/shm; the HTTP endpoint needs port %u free.\n",
                static_cast<unsigned>(kPort));
    return 1;
  }

  std::printf("## Safety reaction time, decision to cell stop\n\n");
  std::printf("One source, two links, %d trials each. The cell runs a 1 kHz control\n", trials);
  std::printf("loop; the trip instant is randomised so the result is not an artefact\n");
  std::printf("of phase-locking with the poller. Microseconds.\n\n");

  std::vector<Summary> summaries;

  {
    auto shm = SharedMemorySafetyLink::openExisting(kRegionName);
    if (!shm.valid()) {
      std::printf("shared-memory link unavailable: %s\n", shm.error().c_str());
      return 1;
    }
    summaries.push_back(measure(source, shm, trials, std::chrono::milliseconds(30)));
  }

  {
    HttpPollSafetyLink http("127.0.0.1", kPort, "/safety",
                            std::chrono::milliseconds(poll_ms));
    summaries.push_back(
        measure(source, http, trials, std::chrono::milliseconds(poll_ms + 20)));
  }

  for (const auto& summary : summaries) {
    report(summary);
  }

  if (summaries.size() == 2 && !summaries[0].samples.empty() &&
      !summaries[1].samples.empty()) {
    const double shm_p50 = static_cast<double>(summaries[0].percentile(0.50));
    const double http_p50 = static_cast<double>(summaries[1].percentile(0.50));
    std::printf("\nThe poll interval is %d ms. Median reaction through the polled\n",
                poll_ms);
    std::printf("link is %.0fx the shared-memory link (%.1f ms against %.3f ms).\n",
                shm_p50 > 0.0 ? http_p50 / shm_p50 : 0.0, http_p50 / 1e6, shm_p50 / 1e6);
    std::printf("\nNeither number describes the safety runtime. Both describe the\n");
    std::printf("integration, and the integration is what decides whether the machine\n");
    std::printf("stops in time.\n");
  }

  return 0;
}
