#include "ScreenTermCaptureCoordinator.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>

namespace {
using namespace std::chrono_literals;

int failures = 0;

void expect(bool condition, const char *message) {
  if (condition)
    return;
  ++failures;
  std::cerr << "FAIL: " << message << '\n';
}

struct Gate {
  std::mutex mutex;
  std::condition_variable changed;
  bool entered{false};
  bool open{false};

  void waitUntilEntered() {
    std::unique_lock lock(mutex);
    expect(changed.wait_for(lock, 2s, [&] { return entered; }),
           "capture worker should start promptly");
  }

  void release() {
    std::lock_guard lock(mutex);
    open = true;
    changed.notify_all();
  }

  std::optional<ScreenTermCaptureCoordinator::Terms>
  capture(ScreenTermCaptureCoordinator::Terms terms) {
    std::unique_lock lock(mutex);
    entered = true;
    changed.notify_all();
    changed.wait(lock, [&] { return open; });
    return terms;
  }
};

void testSharesInflightAndCachesOnlyExactTarget() {
  ScreenTermCaptureCoordinator coordinator;
  Gate gate;
  std::atomic<int> captures{0};
  auto first = coordinator.request("window-a", [&] {
    ++captures;
    return gate.capture({"PostgreSQL"});
  });
  gate.waitUntilEntered();
  auto shared = coordinator.request("window-a", [&] {
    ++captures;
    return std::optional<ScreenTermCaptureCoordinator::Terms>{{"wrong"}};
  });
  expect(captures.load() == 1,
         "the same in-flight target should share one capture");
  gate.release();
  expect(first.wait_for(2s) == std::future_status::ready,
         "the first capture should complete");
  expect(shared.get() == ScreenTermCaptureCoordinator::Terms{"PostgreSQL"},
         "the shared request should receive the first target's terms");

  auto cached = coordinator.request("window-a", [&] {
    ++captures;
    return std::optional<ScreenTermCaptureCoordinator::Terms>{{"wrong"}};
  });
  expect(cached.wait_for(0ms) == std::future_status::ready,
         "a fresh completed exact-target result should be immediately ready");
  expect(cached.get() == ScreenTermCaptureCoordinator::Terms{"PostgreSQL"},
         "an exact-target cache hit should preserve the completed result");
  expect(captures.load() == 1,
         "a cache hit should not start another OCR capture");

  auto other = coordinator.request("window-b", [&] {
    ++captures;
    return std::optional<ScreenTermCaptureCoordinator::Terms>{{"SQLite"}};
  });
  expect(other.get() == ScreenTermCaptureCoordinator::Terms{"SQLite"},
         "a different target must run its own capture");
  expect(captures.load() == 2,
         "terms cached for one target must never satisfy another target");
}

void testQueuesDifferentTargetInsteadOfDroppingIt() {
  ScreenTermCaptureCoordinator coordinator;
  Gate gate;
  auto first =
      coordinator.request("window-a", [&] { return gate.capture({"Alpha"}); });
  gate.waitUntilEntered();

  std::atomic<int> secondCaptures{0};
  auto second = coordinator.request("window-b", [&] {
    ++secondCaptures;
    return std::optional<ScreenTermCaptureCoordinator::Terms>{{"Beta"}};
  });
  expect(
      second.wait_for(20ms) == std::future_status::timeout,
      "a different target should queue rather than resolve immediately empty");
  gate.release();
  expect(first.get() == ScreenTermCaptureCoordinator::Terms{"Alpha"},
         "the active target should complete normally");
  expect(second.wait_for(2s) == std::future_status::ready,
         "the queued target should run after the active capture");
  expect(second.get() == ScreenTermCaptureCoordinator::Terms{"Beta"},
         "the queued target should receive only its own terms");
  expect(secondCaptures.load() == 1, "the queued capture should execute once");
}

void testResetInvalidatesQueuedAndCompletedWork() {
  ScreenTermCaptureCoordinator coordinator;
  Gate gate;
  auto active =
      coordinator.request("window-a", [&] { return gate.capture({"stale"}); });
  gate.waitUntilEntered();
  auto queued = coordinator.request("window-b", [] {
    return std::optional<ScreenTermCaptureCoordinator::Terms>{{"must-not-run"}};
  });

  coordinator.reset();
  expect(queued.wait_for(0ms) == std::future_status::ready,
         "reset should resolve queued captures without running them");
  expect(queued.get().empty(), "reset should discard queued terms");
  gate.release();
  expect(active.wait_for(2s) == std::future_status::ready,
         "an invalidated active worker should still finish safely");
  expect(active.get().empty(),
         "an invalidated active result must not escape or be cached");

  auto fresh = coordinator.request("window-a", [] {
    return std::optional<ScreenTermCaptureCoordinator::Terms>{{"fresh"}};
  });
  expect(fresh.get() == ScreenTermCaptureCoordinator::Terms{"fresh"},
         "a new generation should capture rather than reuse invalidated terms");
}

void testPendingWorkIsBoundedAndEvictsOldest() {
  ScreenTermCaptureCoordinator::Options options;
  options.maximumPendingCaptures = 1;
  ScreenTermCaptureCoordinator coordinator(options);
  Gate gate;
  auto active =
      coordinator.request("window-a", [&] { return gate.capture({"Alpha"}); });
  gate.waitUntilEntered();

  std::atomic<int> evictedCaptures{0};
  auto evicted = coordinator.request("window-b", [&] {
    ++evictedCaptures;
    return std::optional<ScreenTermCaptureCoordinator::Terms>{{"Beta"}};
  });
  auto newest = coordinator.request("window-c", [] {
    return std::optional<ScreenTermCaptureCoordinator::Terms>{{"Gamma"}};
  });
  expect(evicted.wait_for(0ms) == std::future_status::ready,
         "a full pending queue should resolve its oldest request promptly");
  expect(evicted.get().empty(),
         "an evicted request should not leak other terms");
  expect(evictedCaptures.load() == 0,
         "an evicted capture should never execute");

  gate.release();
  expect(active.get() == ScreenTermCaptureCoordinator::Terms{"Alpha"},
         "bounded queuing should not disturb the active capture");
  expect(newest.get() == ScreenTermCaptureCoordinator::Terms{"Gamma"},
         "the newest bounded request should execute after the active capture");
}

void testExpiredCaptureIsNeitherReturnedNorCached() {
  ScreenTermCaptureCoordinator::Options options;
  options.maximumCaptureAge = 100ms;
  ScreenTermCaptureCoordinator coordinator(options);
  Gate gate;
  auto stale =
      coordinator.request("window-a", [&] { return gate.capture({"stale"}); });
  gate.waitUntilEntered();
  std::this_thread::sleep_for(200ms);
  gate.release();
  expect(stale.get().empty(),
         "an expired capture should resolve without stale terms");

  std::atomic<int> freshCaptures{0};
  auto fresh = coordinator.request("window-a", [&] {
    ++freshCaptures;
    return std::optional<ScreenTermCaptureCoordinator::Terms>{{"fresh"}};
  });
  expect(fresh.get() == ScreenTermCaptureCoordinator::Terms{"fresh"},
         "an expired result must not be reused from the cache");
  expect(freshCaptures.load() == 1,
         "the exact target should be captured again after expiry");
}
} // namespace

int main() {
  testSharesInflightAndCachesOnlyExactTarget();
  testQueuesDifferentTargetInsteadOfDroppingIt();
  testResetInvalidatesQueuedAndCompletedWork();
  testPendingWorkIsBoundedAndEvictsOldest();
  testExpiredCaptureIsNeitherReturnedNorCached();
  if (failures != 0) {
    std::cerr << failures << " screen-term coordinator test(s) failed\n";
    return EXIT_FAILURE;
  }
  std::cout << "Screen-term capture coordinator tests passed\n";
  return EXIT_SUCCESS;
}
