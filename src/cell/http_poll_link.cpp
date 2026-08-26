// SPDX-License-Identifier: Apache-2.0
#include "cell/http_poll_link.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace pickcell {
namespace {

/// Closes a socket on the way out of any scope, including an early return.
class SocketGuard {
 public:
  explicit SocketGuard(int fd) noexcept : fd_(fd) {}
  ~SocketGuard() {
    if (fd_ >= 0) {
      ::close(fd_);
    }
  }
  SocketGuard(const SocketGuard&) = delete;
  SocketGuard& operator=(const SocketGuard&) = delete;
  SocketGuard(SocketGuard&&) = delete;
  SocketGuard& operator=(SocketGuard&&) = delete;

  int get() const noexcept { return fd_; }

 private:
  int fd_;
};

/// Parses `sequence torque_permitted asserted_ns` from the response body.
///
/// A deliberately trivial format. The point of this link is to measure what
/// polling costs, and a JSON parser in the middle would measure the parser as
/// well -- while making the comparison against the shared-memory link unfair in
/// a way that flattered the conclusion.
bool parseBody(const char* body, SafetySignal& out) {
  unsigned long long sequence = 0;
  unsigned long long permitted = 0;
  unsigned long long asserted = 0;
  if (std::sscanf(body, "%llu %llu %llu", &sequence, &permitted, &asserted) != 3) {
    return false;
  }
  out.sequence = sequence;
  out.torque_permitted = static_cast<std::uint32_t>(permitted);
  out.asserted_monotonic_ns = asserted;
  return true;
}

}  // namespace

HttpPollSafetyLink::HttpPollSafetyLink(std::string host, std::uint16_t port,
                                       std::string path,
                                       std::chrono::milliseconds period, Format format)
    : host_(std::move(host)),
      port_(port),
      path_(std::move(path)),
      period_(period),
      format_(format),
      name_("HTTP poll every " + std::to_string(period.count()) + " ms") {
  latest_.store(SafetySignal{});
  poller_ = std::thread([this] { pollLoop(); });
}

HttpPollSafetyLink::~HttpPollSafetyLink() {
  running_.store(false, std::memory_order_relaxed);
  if (poller_.joinable()) {
    poller_.join();
  }
}

bool HttpPollSafetyLink::fetchOnce(SafetySignal& out) {
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    return false;
  }
  const SocketGuard guard(fd);

  timeval timeout{};
  timeout.tv_sec = 1;
  (void)::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  (void)::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

  // Nagle would add up to 40 ms to a small request on some paths. Measuring the
  // poll period's contribution is the point; adding an avoidable delay on top
  // would overstate it.
  int one = 1;
  (void)::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(port_);

  // inet_pton only accepts a numeric address. The first version of this used it
  // alone, which worked against 127.0.0.1 in the benchmark and failed every
  // single poll under docker compose, where the safety runtime is reached by its
  // service name. getaddrinfo handles both.
  //
  // The cell was right about what to do with the failure -- it held, and
  // pickcell_cycles_without_safety climbed -- which is how the bug was found
  // rather than mistaken for a quiet machine.
  if (::inet_pton(AF_INET, host_.c_str(), &address.sin_addr) != 1) {
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* resolved = nullptr;
    if (::getaddrinfo(host_.c_str(), nullptr, &hints, &resolved) != 0 ||
        resolved == nullptr) {
      return false;
    }
    address.sin_addr =
        reinterpret_cast<sockaddr_in*>(resolved->ai_addr)->sin_addr;
    ::freeaddrinfo(resolved);
  }

  if (::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
    return false;
  }

  const std::string request = "GET " + path_ + " HTTP/1.1\r\nHost: " + host_ +
                              "\r\nConnection: close\r\n\r\n";
  if (::write(fd, request.data(), request.size()) !=
      static_cast<ssize_t>(request.size())) {
    return false;
  }

  std::array<char, 512> response{};
  std::size_t filled = 0;
  while (filled + 1 < response.size()) {
    const ssize_t received =
        ::read(fd, response.data() + filled, response.size() - filled - 1);
    if (received <= 0) {
      break;
    }
    filled += static_cast<std::size_t>(received);
  }
  if (filled == 0) {
    return false;
  }
  response[filled] = '\0';

  const bool ok = std::strstr(response.data(), " 200 ") != nullptr;

  if (format_ == Format::kReadinessProbe) {
    // No decision timestamp exists on the wire, so the only stamp available is
    // the moment of observation. Any reaction time computed from it comes out
    // near zero -- which does not mean the link is fast, it means the link is
    // unmeasurable. The distinction is the point.
    out.torque_permitted = ok ? 1U : 0U;
    out.asserted_monotonic_ns = nowNanos();
    return true;
  }

  if (!ok) {
    return false;
  }
  const char* body = std::strstr(response.data(), "\r\n\r\n");
  if (body == nullptr) {
    return false;
  }
  return parseBody(body + 4, out);
}

void HttpPollSafetyLink::pollLoop() {
  while (running_.load(std::memory_order_relaxed)) {
    SafetySignal signal;
    requests_attempted_.fetch_add(1, std::memory_order_relaxed);
    if (fetchOnce(signal)) {
      // A readiness probe carries no sequence number, so one is synthesised on
      // each change of verdict: the cell reacts to edges, and without a
      // sequence every poll would look like a fresh assertion.
      if (format_ == Format::kReadinessProbe) {
        SafetySignal previous;
        const bool had = latest_.tryLoad(previous);
        if (!had || previous.torque_permitted != signal.torque_permitted) {
          signal.sequence = ++readiness_sequence_;
        } else {
          signal.sequence = previous.sequence;
          signal.asserted_monotonic_ns = previous.asserted_monotonic_ns;
        }
      }
      latest_.store(signal);
      last_success_ns_.store(nowNanos(), std::memory_order_release);
      ever_received_.store(true, std::memory_order_release);
    } else {
      requests_failed_.fetch_add(1, std::memory_order_relaxed);
      // A failed poll is not a report of safety. The last known value stands
      // until it goes stale, and judging staleness belongs to the caller, which
      // is the only place that knows what the cell is doing.
    }

    // Sleep in short slices so shutdown does not wait out a whole period.
    const auto deadline = Clock::now() + period_;
    while (running_.load(std::memory_order_relaxed) && Clock::now() < deadline) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }
}

SafetyView HttpPollSafetyLink::poll() {
  if (!ever_received_.load(std::memory_order_acquire)) {
    // Never heard from the runtime. Not a statement that torque is permitted.
    return SafetyView{};
  }

  SafetySignal signal;
  if (!latest_.tryLoad(signal)) {
    return last_good_;
  }

  SafetyView view;
  view.valid = true;
  view.torque_permitted = signal.torque_permitted != 0;
  view.asserted_monotonic_ns = signal.asserted_monotonic_ns;
  // The moment the information was obtained, NOT the moment it was asked for.
  //
  // This was nowNanos() at first, which made a cached value look freshly
  // confirmed on every call. Stopping the safety runtime under docker compose
  // then left the cell running on a permit nobody had reissued -- absence of
  // information reading as permission, which is the failure direction that must
  // never happen. Reporting the true age lets the caller notice.
  view.observed_monotonic_ns = last_success_ns_.load(std::memory_order_acquire);
  view.sequence = signal.sequence;
  last_good_ = view;
  return view;
}

}  // namespace pickcell
