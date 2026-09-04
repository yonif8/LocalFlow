#include "localflow/platform/AccessibilityText.hpp"

#include <chrono>
#include <iostream>
#include <string>
#include <thread>

namespace {

int failures = 0;

#define EXPECT_TRUE(value) do { \
    if (!(value)) { \
        std::cerr << __FILE__ << ':' << __LINE__ \
                  << " expected true: " #value "\n"; \
        ++failures; \
    } \
} while (false)

#define EXPECT_EQ(left, right) do { \
    const auto actualLeft = (left); \
    const auto actualRight = (right); \
    if (!(actualLeft == actualRight)) { \
        std::cerr << __FILE__ << ':' << __LINE__ \
                  << " expected equality: " #left " == " #right "\n"; \
        ++failures; \
    } \
} while (false)

using localflow::platform::AccessibilityNodeDecision;
using localflow::platform::AccessibilityTextAccumulator;
using localflow::platform::AccessibilityTextLimits;

void secure_and_hidden_subtrees_never_capture_text() {
    AccessibilityTextAccumulator values;
    EXPECT_EQ(
        values.beginNode(0, true, true),
        AccessibilityNodeDecision::skipSubtree);
    EXPECT_TRUE(!values.append("hunter2", true));
    EXPECT_EQ(
        values.beginNode(0, false, false),
        AccessibilityNodeDecision::skipSubtree);
    auto snapshot = std::move(values).finish();
    EXPECT_TRUE(snapshot.visibleStrings.empty());
}

void node_depth_fragment_and_byte_limits_are_enforced() {
    AccessibilityTextLimits limits;
    limits.maximumNodes = 2;
    limits.maximumDepth = 1;
    limits.maximumFragments = 2;
    limits.maximumUtf8Bytes = 9;
    limits.maximumFragmentUtf8Bytes = 6;
    AccessibilityTextAccumulator values(limits);

    EXPECT_EQ(
        values.beginNode(0, true, false),
        AccessibilityNodeDecision::inspect);
    EXPECT_TRUE(values.append("PostgreSQL"));
    EXPECT_EQ(
        values.beginNode(2, true, false),
        AccessibilityNodeDecision::skipSubtree);
    EXPECT_EQ(
        values.beginNode(0, true, false),
        AccessibilityNodeDecision::stop);

    auto snapshot = std::move(values).finish();
    EXPECT_EQ(snapshot.visitedNodes, std::size_t{2});
    EXPECT_EQ(snapshot.visibleStrings.size(), std::size_t{1});
    EXPECT_EQ(snapshot.visibleStrings.front(), std::string("Postgr"));
    EXPECT_TRUE(snapshot.truncated);
}

void byte_truncation_preserves_complete_utf8_scalars() {
    AccessibilityTextLimits limits;
    limits.maximumUtf8Bytes = 5;
    limits.maximumFragmentUtf8Bytes = 5;
    AccessibilityTextAccumulator values(limits);
    EXPECT_EQ(
        values.beginNode(0, true, false),
        AccessibilityNodeDecision::inspect);
    EXPECT_TRUE(values.append("ab\xF0\x9F\x98\x80z"));
    auto snapshot = std::move(values).finish();
    EXPECT_EQ(snapshot.visibleStrings.front(), std::string("ab"));
    EXPECT_TRUE(snapshot.truncated);
}

void duplicate_fragments_do_not_consume_the_budget() {
    AccessibilityTextLimits limits;
    limits.maximumFragments = 2;
    AccessibilityTextAccumulator values(limits);
    EXPECT_TRUE(values.append("LocalFlow"));
    EXPECT_TRUE(values.append("LocalFlow"));
    EXPECT_TRUE(values.append("PostgreSQL"));
    auto snapshot = std::move(values).finish();
    EXPECT_EQ(snapshot.visibleStrings.size(), std::size_t{2});
}

void elapsed_time_stops_the_walk() {
    AccessibilityTextLimits limits;
    limits.timeBudget = std::chrono::milliseconds(1);
    AccessibilityTextAccumulator values(limits);
    std::this_thread::sleep_for(std::chrono::milliseconds(3));
    EXPECT_EQ(
        values.beginNode(0, true, false),
        AccessibilityNodeDecision::stop);
    auto snapshot = std::move(values).finish();
    EXPECT_TRUE(snapshot.truncated);
}

}  // namespace

int main() {
    secure_and_hidden_subtrees_never_capture_text();
    node_depth_fragment_and_byte_limits_are_enforced();
    byte_truncation_preserves_complete_utf8_scalars();
    duplicate_fragments_do_not_consume_the_budget();
    elapsed_time_stops_the_walk();
    return failures == 0 ? 0 : 1;
}
