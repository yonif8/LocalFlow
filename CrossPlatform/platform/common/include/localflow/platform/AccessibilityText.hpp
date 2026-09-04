#pragma once

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace localflow::platform {

// Accessibility providers are optional screen-context sources, never a
// prerequisite for dictation. These deliberately conservative defaults keep
// an accessibility walk small even when an application exposes a very large
// virtual document tree.
struct AccessibilityTextLimits {
    std::size_t maximumNodes{320};
    std::size_t maximumDepth{24};
    std::size_t maximumFragments{160};
    std::size_t maximumUtf8Bytes{32 * 1024};
    std::size_t maximumFragmentUtf8Bytes{2048};
    std::chrono::milliseconds timeBudget{80};
};

struct AccessibilityTextSnapshot {
    std::vector<std::string> visibleStrings;
    std::size_t visitedNodes{0};
    std::size_t capturedUtf8Bytes{0};
    bool truncated{false};
};

enum class AccessibilityNodeDecision {
    inspect,
    skipSubtree,
    stop,
};

// Shared, dependency-free enforcement for native UI Automation and AT-SPI
// collectors. Callers must classify a node before asking it for text: secure
// and non-visible nodes are rejected at beginNode(), preventing their content
// from entering this object at all.
class AccessibilityTextAccumulator final {
public:
    explicit AccessibilityTextAccumulator(
        AccessibilityTextLimits limits = {}) noexcept
        : limits_(sanitized(std::move(limits))),
          deadline_(Clock::now() + limits_.timeBudget) {
        snapshot_.visibleStrings.reserve(limits_.maximumFragments);
    }

    [[nodiscard]] AccessibilityNodeDecision beginNode(
        const std::size_t depth,
        const bool visible,
        const bool protectedContent) noexcept {
        if (stopped() || snapshot_.visitedNodes >= limits_.maximumNodes) {
            snapshot_.truncated = true;
            return AccessibilityNodeDecision::stop;
        }
        ++snapshot_.visitedNodes;
        if (depth > limits_.maximumDepth) {
            snapshot_.truncated = true;
            return AccessibilityNodeDecision::skipSubtree;
        }
        if (!visible || protectedContent) {
            return AccessibilityNodeDecision::skipSubtree;
        }
        return AccessibilityNodeDecision::inspect;
    }

    [[nodiscard]] bool append(
        std::string_view value,
        const bool protectedContent = false) {
        if (protectedContent || stopped()) {
            return false;
        }

        trim(value);
        if (value.empty()) {
            return false;
        }
        if (snapshot_.visibleStrings.size() >= limits_.maximumFragments ||
            snapshot_.capturedUtf8Bytes >= limits_.maximumUtf8Bytes) {
            snapshot_.truncated = true;
            return false;
        }

        const std::size_t remaining =
            limits_.maximumUtf8Bytes - snapshot_.capturedUtf8Bytes;
        const std::size_t cap = std::min(
            limits_.maximumFragmentUtf8Bytes, remaining);
        if (value.size() > cap) {
            value = value.substr(0, validUtf8Prefix(value, cap));
            trim(value);
            snapshot_.truncated = true;
        }
        if (value.empty()) {
            return false;
        }

        std::string bounded(value);
        if (seen_.find(bounded) != seen_.end()) {
            return true;
        }

        snapshot_.capturedUtf8Bytes += bounded.size();
        seen_.insert(bounded);
        snapshot_.visibleStrings.push_back(std::move(bounded));
        return true;
    }

    [[nodiscard]] bool stopped() const noexcept {
        return snapshot_.visibleStrings.size() >= limits_.maximumFragments ||
               snapshot_.capturedUtf8Bytes >= limits_.maximumUtf8Bytes ||
               Clock::now() >= deadline_;
    }

    [[nodiscard]] AccessibilityTextSnapshot finish() && {
        if (Clock::now() >= deadline_ ||
            snapshot_.visibleStrings.size() >= limits_.maximumFragments ||
            snapshot_.capturedUtf8Bytes >= limits_.maximumUtf8Bytes) {
            snapshot_.truncated = true;
        }
        return std::move(snapshot_);
    }

private:
    using Clock = std::chrono::steady_clock;

    static AccessibilityTextLimits sanitized(
        AccessibilityTextLimits value) noexcept {
        value.maximumNodes = std::max<std::size_t>(value.maximumNodes, 1);
        value.maximumDepth = std::max<std::size_t>(value.maximumDepth, 1);
        value.maximumFragments =
            std::max<std::size_t>(value.maximumFragments, 1);
        value.maximumUtf8Bytes =
            std::max<std::size_t>(value.maximumUtf8Bytes, 1);
        value.maximumFragmentUtf8Bytes = std::max<std::size_t>(
            std::min(value.maximumFragmentUtf8Bytes,
                     value.maximumUtf8Bytes),
            1);
        value.timeBudget =
            std::max(value.timeBudget, std::chrono::milliseconds(1));
        return value;
    }

    static bool whitespace(const unsigned char value) noexcept {
        return value == ' ' || value == '\t' || value == '\r' ||
               value == '\n' || value == '\f' || value == '\v';
    }

    static void trim(std::string_view& value) noexcept {
        while (!value.empty() &&
               (whitespace(static_cast<unsigned char>(value.back())) ||
               value.back() == '\0')) {
            value.remove_suffix(1);
        }
        while (!value.empty() &&
               (whitespace(static_cast<unsigned char>(value.front())) ||
                value.front() == '\0')) {
            value.remove_prefix(1);
        }
    }

    // Native APIs return UTF-8, but a byte cap can land inside a multi-byte
    // scalar. Preserve the largest complete prefix without attempting to
    // repair or reinterpret provider content.
    static std::size_t validUtf8Prefix(
        std::string_view value,
        const std::size_t maximum) noexcept {
        std::size_t cursor = 0;
        std::size_t complete = 0;
        const std::size_t end = std::min(maximum, value.size());
        while (cursor < end) {
            const auto lead = static_cast<unsigned char>(value[cursor]);
            std::size_t width = 1;
            if ((lead & 0x80U) == 0U) {
                width = 1;
            } else if ((lead & 0xE0U) == 0xC0U) {
                width = 2;
            } else if ((lead & 0xF0U) == 0xE0U) {
                width = 3;
            } else if ((lead & 0xF8U) == 0xF0U) {
                width = 4;
            } else {
                break;
            }
            if (cursor + width > end) {
                break;
            }
            bool continuation = true;
            for (std::size_t index = 1; index < width; ++index) {
                const auto byte =
                    static_cast<unsigned char>(value[cursor + index]);
                continuation = continuation && (byte & 0xC0U) == 0x80U;
            }
            if (!continuation) {
                break;
            }
            cursor += width;
            complete = cursor;
        }
        return complete;
    }

    AccessibilityTextLimits limits_;
    Clock::time_point deadline_;
    AccessibilityTextSnapshot snapshot_;
    std::unordered_set<std::string> seen_;
};

}  // namespace localflow::platform
