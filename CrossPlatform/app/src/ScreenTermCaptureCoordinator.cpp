#include "ScreenTermCaptureCoordinator.hpp"

#include <algorithm>
#include <cstdint>
#include <deque>
#include <mutex>
#include <thread>
#include <utility>

namespace {
using Clock = std::chrono::steady_clock;
using Terms = ScreenTermCaptureCoordinator::Terms;

std::shared_future<Terms> ready_terms(Terms terms = {}) {
  std::promise<Terms> promise;
  auto future = promise.get_future().share();
  promise.set_value(std::move(terms));
  return future;
}

void fulfill(const std::shared_ptr<std::promise<Terms>> &promise,
             Terms terms = {}) noexcept {
  if (!promise)
    return;
  try {
    promise->set_value(std::move(terms));
  } catch (...) {
  }
}
} // namespace

struct ScreenTermCaptureCoordinator::State {
  struct Request {
    std::string key;
    Capture capture;
    std::shared_ptr<std::promise<Terms>> promise;
    std::shared_future<Terms> future;
    Clock::time_point requestedAt;
    std::uint64_t generation{0};
  };

  struct Active {
    std::string key;
    std::shared_future<Terms> future;
    std::uint64_t generation{0};
  };

  struct CacheEntry {
    std::string key;
    Terms terms;
    Clock::time_point completedAt;
  };

  explicit State(Options value) : options(std::move(value)) {
    options.maximumCacheEntries =
        std::max<std::size_t>(1, options.maximumCacheEntries);
    options.maximumPendingCaptures =
        std::max<std::size_t>(1, options.maximumPendingCaptures);
    options.cacheLifetime =
        std::max(options.cacheLifetime, std::chrono::milliseconds(1));
    options.maximumCaptureAge =
        std::max(options.maximumCaptureAge, std::chrono::milliseconds(1));
  }

  Options options;
  std::mutex mutex;
  std::uint64_t generation{1};
  std::optional<Active> active;
  std::deque<Request> pending;
  std::deque<CacheEntry> cache;
};

namespace {
using State = ScreenTermCaptureCoordinator::State;
using Request = State::Request;

void launch(const std::shared_ptr<State> &state, Request request);

void prune_cache(State &state, const Clock::time_point now) {
  state.cache.erase(std::remove_if(state.cache.begin(), state.cache.end(),
                                   [&](const auto &entry) {
                                     return now - entry.completedAt >
                                            state.options.cacheLifetime;
                                   }),
                    state.cache.end());
}

void complete(const std::shared_ptr<State> &state, Request request,
              std::optional<Terms> captured) noexcept {
  const auto completedAt = Clock::now();
  Terms output;
  std::optional<Request> next;
  std::vector<std::shared_ptr<std::promise<Terms>>> discarded;

  {
    std::lock_guard lock(state->mutex);
    const bool currentGeneration = request.generation == state->generation;
    const bool completedInTime =
        completedAt - request.requestedAt <= state->options.maximumCaptureAge;
    if (currentGeneration && completedInTime && captured.has_value()) {
      output = std::move(*captured);
      prune_cache(*state, completedAt);
      const auto prior = std::find_if(
          state->cache.begin(), state->cache.end(),
          [&](const auto &entry) { return entry.key == request.key; });
      if (prior != state->cache.end())
        state->cache.erase(prior);
      state->cache.push_front({request.key, output, completedAt});
      while (state->cache.size() > state->options.maximumCacheEntries) {
        state->cache.pop_back();
      }
    }

    state->active.reset();
    while (!state->pending.empty()) {
      auto candidate = std::move(state->pending.front());
      state->pending.pop_front();
      const bool usable = candidate.generation == state->generation &&
                          completedAt - candidate.requestedAt <=
                              state->options.maximumCaptureAge;
      if (!usable) {
        discarded.push_back(std::move(candidate.promise));
        continue;
      }
      state->active =
          State::Active{candidate.key, candidate.future, candidate.generation};
      next = std::move(candidate);
      break;
    }
  }

  fulfill(request.promise, std::move(output));
  for (const auto &promise : discarded)
    fulfill(promise);
  if (next.has_value())
    launch(state, std::move(*next));
}

void launch(const std::shared_ptr<State> &state, Request request) {
  std::shared_ptr<Request> work;
  try {
    work = std::make_shared<Request>(std::move(request));
    std::thread([state, work]() mutable {
      auto owned = std::move(*work);
      work.reset();
      std::optional<Terms> captured;
      try {
        captured = owned.capture();
      } catch (...) {
      }
      complete(state, std::move(owned), std::move(captured));
    }).detach();
  } catch (...) {
    complete(state, work ? std::move(*work) : std::move(request), std::nullopt);
  }
}
} // namespace

ScreenTermCaptureCoordinator::ScreenTermCaptureCoordinator()
    : ScreenTermCaptureCoordinator(Options{}) {}

ScreenTermCaptureCoordinator::ScreenTermCaptureCoordinator(Options options)
    : state_(std::make_shared<State>(std::move(options))) {}

ScreenTermCaptureCoordinator::~ScreenTermCaptureCoordinator() { reset(); }

std::shared_future<ScreenTermCaptureCoordinator::Terms>
ScreenTermCaptureCoordinator::request(std::string targetKey, Capture capture) {
  if (targetKey.empty() || !capture)
    return ready_terms();

  const auto requestedAt = Clock::now();
  auto promise = std::make_shared<std::promise<Terms>>();
  auto future = promise->get_future().share();
  std::optional<Request> launchNow;
  std::shared_ptr<std::promise<Terms>> evicted;

  {
    std::lock_guard lock(state_->mutex);
    prune_cache(*state_, requestedAt);
    const auto cached =
        std::find_if(state_->cache.begin(), state_->cache.end(),
                     [&](const auto &entry) { return entry.key == targetKey; });
    if (cached != state_->cache.end()) {
      auto terms = cached->terms;
      auto entry = std::move(*cached);
      state_->cache.erase(cached);
      state_->cache.push_front(std::move(entry));
      return ready_terms(std::move(terms));
    }

    if (state_->active && state_->active->generation == state_->generation &&
        state_->active->key == targetKey) {
      return state_->active->future;
    }
    const auto queued =
        std::find_if(state_->pending.begin(), state_->pending.end(),
                     [&](const auto &request) {
                       return request.generation == state_->generation &&
                              request.key == targetKey;
                     });
    if (queued != state_->pending.end())
      return queued->future;

    Request request{std::move(targetKey), std::move(capture), promise, future,
                    requestedAt,          state_->generation};
    if (!state_->active.has_value()) {
      state_->active =
          State::Active{request.key, request.future, request.generation};
      launchNow = std::move(request);
    } else {
      if (state_->pending.size() >= state_->options.maximumPendingCaptures) {
        evicted = std::move(state_->pending.front().promise);
        state_->pending.pop_front();
      }
      state_->pending.push_back(std::move(request));
    }
  }

  if (evicted)
    fulfill(evicted);
  if (launchNow.has_value())
    launch(state_, std::move(*launchNow));
  return future;
}

void ScreenTermCaptureCoordinator::reset() noexcept {
  if (!state_)
    return;
  std::vector<std::shared_ptr<std::promise<Terms>>> discarded;
  {
    std::lock_guard lock(state_->mutex);
    ++state_->generation;
    state_->cache.clear();
    while (!state_->pending.empty()) {
      discarded.push_back(std::move(state_->pending.front().promise));
      state_->pending.pop_front();
    }
  }
  for (const auto &promise : discarded)
    fulfill(promise);
}
