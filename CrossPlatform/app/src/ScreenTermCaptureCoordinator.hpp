#pragma once

#include <chrono>
#include <cstddef>
#include <functional>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <vector>

/// Serializes screen-term capture without making a later dictation inherit
/// context from a different window. Completed, filtered term lists are cached
/// briefly by an opaque press-time target key; pixels and raw OCR text are
/// never retained here.
class ScreenTermCaptureCoordinator final {
public:
  using Terms = std::vector<std::string>;
  using Capture = std::function<std::optional<Terms>()>;

  struct Options {
    std::size_t maximumCacheEntries{4};
    std::size_t maximumPendingCaptures{4};
    std::chrono::milliseconds cacheLifetime{2000};
    std::chrono::milliseconds maximumCaptureAge{5000};
  };

  ScreenTermCaptureCoordinator();
  explicit ScreenTermCaptureCoordinator(Options options);
  ~ScreenTermCaptureCoordinator();

  ScreenTermCaptureCoordinator(const ScreenTermCaptureCoordinator &) = delete;
  ScreenTermCaptureCoordinator &
  operator=(const ScreenTermCaptureCoordinator &) = delete;

  /// Returns a ready future for a fresh exact-target cache hit, shares an
  /// existing exact-target request, or queues a bounded capture. A failed,
  /// expired, superseded, or evicted request resolves to an empty term list.
  [[nodiscard]] std::shared_future<Terms> request(std::string targetKey,
                                                  Capture capture);

  /// Invalidates cached and queued work. An already-running capture is allowed
  /// to finish on its detached worker, but its result cannot enter the cache.
  void reset() noexcept;

  // Public only as an opaque implementation type so translation-unit helper
  // functions can operate on shared state after the coordinator is destroyed.
  struct State;

private:
  std::shared_ptr<State> state_;
};
