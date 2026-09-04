#include "InternalFactories.hpp"
#include "localflow/linux/AccessibilityText.hpp"

#include <algorithm>
#include <climits>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <utility>

#if defined(LOCALFLOW_LINUX_WITH_ATSPI)
#include <atspi/atspi.h>
#include <glib-object.h>
#endif

namespace localflow::platform::linux::detail {
namespace {

#if defined(LOCALFLOW_LINUX_WITH_ATSPI)

constexpr int kMaximumTreeDepth = 40;
constexpr int kMaximumVisitedNodes = 4096;
constexpr int kMaximumEditableAncestors = 8;
constexpr auto kTraversalBudget = std::chrono::milliseconds(500);
constexpr int kAtSpiCallTimeoutMilliseconds = 400;
constexpr int kAtSpiStartupTimeoutMilliseconds = 800;

template <typename T>
struct GObjectUnref {
    void operator()(T* object) const noexcept {
        if (object) g_object_unref(object);
    }
};

struct GCharFree {
    void operator()(gchar* value) const noexcept {
        if (value) g_free(value);
    }
};

struct HashTableUnref {
    void operator()(GHashTable* value) const noexcept {
        if (value) g_hash_table_unref(value);
    }
};

struct ArrayUnref {
    void operator()(GArray* value) const noexcept {
        if (value) g_array_unref(value);
    }
};

struct RectFree {
    void operator()(AtspiRect* value) const noexcept {
        if (value) g_boxed_free(ATSPI_TYPE_RECT, value);
    }
};

using AccessiblePtr =
    std::unique_ptr<AtspiAccessible, GObjectUnref<AtspiAccessible>>;
using StateSetPtr = std::unique_ptr<AtspiStateSet, GObjectUnref<AtspiStateSet>>;
using GCharPtr = std::unique_ptr<gchar, GCharFree>;
using HashTablePtr = std::unique_ptr<GHashTable, HashTableUnref>;
using ArrayPtr = std::unique_ptr<GArray, ArrayUnref>;
using RectPtr = std::unique_ptr<AtspiRect, RectFree>;

void clearError(GError*& error) {
    if (error) {
        g_error_free(error);
        error = nullptr;
    }
}

std::string errorMessage(GError* error, const char* fallback) {
    return error && error->message ? error->message : fallback;
}

bool hasState(AtspiAccessible* accessible, AtspiStateType state) {
    StateSetPtr states(atspi_accessible_get_state_set(accessible));
    return states && atspi_state_set_contains(states.get(), state);
}

struct SearchBudget {
    int remaining{kMaximumVisitedNodes};
    std::chrono::steady_clock::time_point deadline{
        std::chrono::steady_clock::now() + kTraversalBudget};
    bool exhausted{false};

    SearchBudget() = default;
    SearchBudget(const int maximumNodes, const std::chrono::milliseconds budget)
        : remaining(std::max(maximumNodes, 1)),
          deadline(std::chrono::steady_clock::now() +
                   std::max(budget, std::chrono::milliseconds(1))) {}

    bool expired() {
        if (std::chrono::steady_clock::now() < deadline) return false;
        exhausted = true;
        return true;
    }

    bool consume() {
        if (remaining <= 0 || expired()) {
            exhausted = true;
            return false;
        }
        --remaining;
        return true;
    }
};

AccessiblePtr findFocused(
    AtspiAccessible* root,
    int depth,
    SearchBudget& budget) {
    if (!root || depth > kMaximumTreeDepth || !budget.consume()) return {};
    if (hasState(root, ATSPI_STATE_DEFUNCT)) return {};
    if (hasState(root, ATSPI_STATE_FOCUSED)) {
        return AccessiblePtr(static_cast<AtspiAccessible*>(g_object_ref(root)));
    }

    GError* error = nullptr;
    const int children = atspi_accessible_get_child_count(root, &error);
    if (error) {
        clearError(error);
        return {};
    }
    for (int index = 0; index < children && !budget.exhausted; ++index) {
        AccessiblePtr child(
            atspi_accessible_get_child_at_index(root, index, &error));
        if (error) {
            clearError(error);
            continue;
        }
        if (auto focused = findFocused(child.get(), depth + 1, budget)) {
            return focused;
        }
    }
    return {};
}

AccessiblePtr editableObjectForFocus(
    AtspiAccessible* focused,
    SearchBudget& budget) {
    AccessiblePtr current(
        static_cast<AtspiAccessible*>(g_object_ref(focused)));
    for (int level = 0; current && level < kMaximumEditableAncestors; ++level) {
        if (budget.expired()) return {};
        if (atspi_accessible_get_editable_text_iface(current.get()) != nullptr) {
            return current;
        }
        GError* error = nullptr;
        AtspiAccessible* parent =
            atspi_accessible_get_parent(current.get(), &error);
        if (error) {
            clearError(error);
            return {};
        }
        current.reset(parent);
    }
    return {};
}

std::string lower(std::string value) {
    for (auto& character : value) {
        character = static_cast<char>(
            std::tolower(static_cast<unsigned char>(character)));
    }
    return value;
}

bool containsSecureWord(const std::string& value) {
    const auto normalized = lower(value);
    return normalized.find("password") != std::string::npos ||
           normalized.find("passwd") != std::string::npos ||
           normalized.find("protected") != std::string::npos ||
           normalized.find("secret") != std::string::npos;
}

FieldSecurity objectSecurity(
    AtspiAccessible* accessible,
    SearchBudget& budget) {
    if (budget.expired()) return FieldSecurity::unknown;
    GError* error = nullptr;
    const auto role = atspi_accessible_get_role(accessible, &error);
    if (error) {
        clearError(error);
        return FieldSecurity::unknown;
    }
    if (role == ATSPI_ROLE_PASSWORD_TEXT) return FieldSecurity::secure;
    if (budget.expired()) return FieldSecurity::unknown;

    HashTablePtr attributes(atspi_accessible_get_attributes(accessible, &error));
    if (error || !attributes) {
        clearError(error);
        return FieldSecurity::unknown;
    }

    GHashTableIter iterator;
    gpointer rawKey = nullptr;
    gpointer rawValue = nullptr;
    g_hash_table_iter_init(&iterator, attributes.get());
    while (g_hash_table_iter_next(&iterator, &rawKey, &rawValue)) {
        const std::string key = rawKey
            ? static_cast<const char*>(rawKey)
            : std::string{};
        const std::string value = rawValue
            ? static_cast<const char*>(rawValue)
            : std::string{};
        const auto normalizedValue = lower(value);
        const bool explicitlyFalse = normalizedValue == "false" ||
                                     normalizedValue == "no" ||
                                     normalizedValue == "0";
        if (containsSecureWord(value) ||
            (containsSecureWord(key) && !explicitlyFalse)) {
            return FieldSecurity::secure;
        }
    }
    return FieldSecurity::non_secure;
}

FieldSecurity targetSecurity(
    AtspiAccessible* focused,
    AtspiAccessible* editable,
    SearchBudget& budget) {
    const auto focusedSecurity = objectSecurity(focused, budget);
    if (focusedSecurity == FieldSecurity::secure) return focusedSecurity;
    if (editable == focused) return focusedSecurity;

    const auto editableSecurity = objectSecurity(editable, budget);
    if (editableSecurity == FieldSecurity::secure) return editableSecurity;
    if (focusedSecurity == FieldSecurity::unknown ||
        editableSecurity == FieldSecurity::unknown) {
        return FieldSecurity::unknown;
    }
    return FieldSecurity::non_secure;
}

std::string accessibleString(gchar* value, GError*& error) {
    GCharPtr owned(value);
    if (error) {
        clearError(error);
        return {};
    }
    return owned ? std::string(owned.get()) : std::string{};
}

bool isWindowRole(AtspiRole role) {
    return role == ATSPI_ROLE_DIALOG || role == ATSPI_ROLE_FRAME ||
           role == ATSPI_ROLE_INTERNAL_FRAME || role == ATSPI_ROLE_WINDOW ||
           role == ATSPI_ROLE_DOCUMENT_FRAME;
}

std::string nearestWindowTitle(
    AtspiAccessible* focused,
    SearchBudget& budget) {
    AccessiblePtr current(
        static_cast<AtspiAccessible*>(g_object_ref(focused)));
    for (int level = 0; current && level < 16; ++level) {
        if (budget.expired()) return {};
        GError* error = nullptr;
        const auto role = atspi_accessible_get_role(current.get(), &error);
        if (!error && isWindowRole(role)) {
            const auto name = accessibleString(
                atspi_accessible_get_name(current.get(), &error), error);
            if (!name.empty()) return name;
        }
        clearError(error);
        AtspiAccessible* parent =
            atspi_accessible_get_parent(current.get(), &error);
        if (error) {
            clearError(error);
            return {};
        }
        current.reset(parent);
    }
    return {};
}

struct CapturedTarget {
    ApplicationInfo information;
    AccessiblePtr focused;
    AccessiblePtr editable;
};

Result<CapturedTarget> captureFocusedTarget(
    AtspiAccessible* searchRoot,
    SearchBudget& budget) {
    auto focused = findFocused(searchRoot, 0, budget);
    if (!focused) {
        return Result<CapturedTarget>::failure(Status::failure(
            budget.exhausted ? ErrorCode::timed_out : ErrorCode::service_unavailable,
            budget.exhausted
                ? "The bounded AT-SPI focused-target search timed out."
                : "No focused AT-SPI accessible object could be identified.",
            "Keep the destination field focused and try dictating again."));
    }

    const auto timedOut = [&]() {
        return Result<CapturedTarget>::failure(Status::failure(
            ErrorCode::timed_out,
            "The bounded AT-SPI focused-target snapshot timed out.",
            "Keep the destination field focused and try dictating again."));
    };

    auto* object = ATSPI_OBJECT(focused.get());
    if (!object || !object->app || !object->app->bus_name ||
        !*object->app->bus_name || !object->path || !*object->path) {
        return Result<CapturedTarget>::failure(Status::failure(
            ErrorCode::protocol_error,
            "The focused AT-SPI object did not expose a stable bus name and object path."));
    }

    GError* error = nullptr;
    if (budget.expired()) return timedOut();
    const guint processId =
        atspi_accessible_get_process_id(focused.get(), &error);
    if (error || processId == 0) {
        const auto detail = errorMessage(
            error, "The focused AT-SPI application did not expose a process ID.");
        clearError(error);
        return Result<CapturedTarget>::failure(Status::failure(
            ErrorCode::protocol_error, detail));
    }

    if (budget.expired()) return timedOut();
    AccessiblePtr application(
        atspi_accessible_get_application(focused.get(), &error));
    if (error || !application) {
        const auto detail = errorMessage(
            error, "The focused AT-SPI object did not expose its application.");
        clearError(error);
        return Result<CapturedTarget>::failure(Status::failure(
            ErrorCode::protocol_error, detail));
    }

    ApplicationInfo information;
    information.processId = static_cast<std::int64_t>(processId);
    information.name = accessibleString(
        atspi_accessible_get_name(application.get(), &error), error);
    if (budget.expired()) return timedOut();
    information.applicationId = accessibleString(
        atspi_accessible_get_accessible_id(application.get(), &error), error);
    if (information.applicationId.empty()) {
        information.applicationId = object->app->bus_name;
    }
    if (information.name.empty()) information.name = information.applicationId;

    FocusedAccessibleTarget target;
    target.busName = object->app->bus_name;
    target.objectPath = object->path;
    if (budget.expired()) return timedOut();
    target.accessibleId = accessibleString(
        atspi_accessible_get_accessible_id(focused.get(), &error), error);
    if (budget.expired()) return timedOut();
    const auto role = atspi_accessible_get_role(focused.get(), &error);
    if (error) {
        const auto detail = errorMessage(
            error, "The focused AT-SPI object did not expose its role.");
        clearError(error);
        return Result<CapturedTarget>::failure(Status::failure(
            ErrorCode::protocol_error, detail));
    }
    target.role = accessibleString(
        atspi_accessible_get_role_name(focused.get(), &error), error);
    if (target.role.empty()) {
        GCharPtr roleName(atspi_role_get_name(role));
        if (roleName) target.role = roleName.get();
    }
    if (budget.expired()) return timedOut();
    atspi_accessible_clear_cache_single(focused.get());
    target.focused = hasState(focused.get(), ATSPI_STATE_FOCUSED) &&
                     !hasState(focused.get(), ATSPI_STATE_DEFUNCT);

    auto editable = editableObjectForFocus(focused.get(), budget);
    if (editable) {
        target.editable = hasState(editable.get(), ATSPI_STATE_EDITABLE) &&
                          !hasState(editable.get(), ATSPI_STATE_READ_ONLY) &&
                          !hasState(editable.get(), ATSPI_STATE_DEFUNCT);
        target.security = targetSecurity(
            focused.get(), editable.get(), budget);
    } else {
        target.editable = false;
        target.security = objectSecurity(focused.get(), budget);
    }
    if (budget.expired()) return timedOut();
    information.focusedTarget = std::move(target);
    information.windowTitle = nearestWindowTitle(focused.get(), budget);

    CapturedTarget captured;
    captured.information = std::move(information);
    captured.focused = std::move(focused);
    captured.editable = std::move(editable);
    return Result<CapturedTarget>::success(std::move(captured));
}

Result<CapturedTarget> captureFocusedTarget() {
    AccessiblePtr desktop(atspi_get_desktop(0));
    if (!desktop) {
        return Result<CapturedTarget>::failure(Status::failure(
            ErrorCode::service_unavailable,
            "AT-SPI2 did not return an accessible desktop."));
    }
    SearchBudget budget;
    return captureFocusedTarget(desktop.get(), budget);
}

class AtSpiRuntime {
public:
    AtSpiRuntime() {
        std::lock_guard<std::mutex> lock(lifecycleMutex());
        if (references()++ == 0) {
            atspi_set_timeout(
                kAtSpiCallTimeoutMilliseconds,
                kAtSpiStartupTimeoutMilliseconds);
            if (!atspi_is_initialized()) {
                ownsInitialization() = atspi_init() == 0;
            }
        }
    }

    ~AtSpiRuntime() {
        std::lock_guard<std::mutex> lock(lifecycleMutex());
        if (--references() == 0 && ownsInitialization()) {
            atspi_exit();
            ownsInitialization() = false;
        }
    }

    bool ready() const { return atspi_is_initialized(); }

    static std::mutex& operationMutex() {
        static std::mutex value;
        return value;
    }

private:
    static std::mutex& lifecycleMutex() {
        static std::mutex value;
        return value;
    }
    static int& references() {
        static int value = 0;
        return value;
    }
    static bool& ownsInitialization() {
        static bool value = false;
        return value;
    }
};

AccessiblePtr expectedApplication(
    const ApplicationInfo& expected,
    SearchBudget& budget) {
    if (!expected.focusedTarget || expected.processId <= 0 ||
        expected.focusedTarget->busName.empty()) {
        return {};
    }
    AccessiblePtr desktop(atspi_get_desktop(0));
    if (!desktop) {
        return {};
    }

    GError* error = nullptr;
    const int childCount =
        atspi_accessible_get_child_count(desktop.get(), &error);
    if (error || childCount <= 0) {
        clearError(error);
        return {};
    }
    for (int index = 0; index < childCount && budget.consume(); ++index) {
        AccessiblePtr child(
            atspi_accessible_get_child_at_index(desktop.get(), index, &error));
        if (error || !child) {
            clearError(error);
            continue;
        }
        auto* object = ATSPI_OBJECT(child.get());
        const bool sameBus = object && object->app && object->app->bus_name &&
            expected.focusedTarget->busName == object->app->bus_name;
        if (!sameBus) {
            continue;
        }
        const guint processId =
            atspi_accessible_get_process_id(child.get(), &error);
        if (error) {
            clearError(error);
            continue;
        }
        if (static_cast<std::int64_t>(processId) == expected.processId) {
            return child;
        }
    }
    return {};
}

AccessiblePtr visibleWindowForFocus(
    AtspiAccessible* focused,
    SearchBudget& budget) {
    AccessiblePtr current(
        static_cast<AtspiAccessible*>(g_object_ref(focused)));
    for (int level = 0; current && level < 16 && budget.consume(); ++level) {
        GError* error = nullptr;
        const auto role = atspi_accessible_get_role(current.get(), &error);
        if (!error && isWindowRole(role)) {
            return current;
        }
        clearError(error);
        AtspiAccessible* parent =
            atspi_accessible_get_parent(current.get(), &error);
        if (error) {
            clearError(error);
            return {};
        }
        current.reset(parent);
    }
    return {};
}

void appendVisibleTextRanges(
    AtspiAccessible* accessible,
    SearchBudget& budget,
    localflow::platform::AccessibilityTextAccumulator& accumulator) {
    if (budget.expired() || accumulator.stopped()) {
        return;
    }
    auto* text = atspi_accessible_get_text_iface(accessible);
    auto* component = atspi_accessible_get_component_iface(accessible);
    if (!text || !component) {
        return;
    }

    GError* error = nullptr;
    RectPtr bounds(atspi_component_get_extents(
        component, ATSPI_COORD_TYPE_SCREEN, &error));
    if (error || !bounds || bounds->width <= 0 || bounds->height <= 0) {
        clearError(error);
        return;
    }
    ArrayPtr ranges(atspi_text_get_bounded_ranges(
        text,
        bounds->x,
        bounds->y,
        bounds->width,
        bounds->height,
        ATSPI_COORD_TYPE_SCREEN,
        ATSPI_TEXT_CLIP_BOTH,
        ATSPI_TEXT_CLIP_BOTH,
        &error));
    if (error || !ranges) {
        clearError(error);
        return;
    }

    constexpr guint kMaximumVisibleRangesPerNode = 16;
    const guint count = std::min(ranges->len, kMaximumVisibleRangesPerNode);
    for (guint index = 0;
         index < count && !budget.expired() && !accumulator.stopped();
         ++index) {
        const auto* range =
            g_array_index(ranges.get(), AtspiTextRange*, index);
        if (range && range->content && *range->content) {
            (void)accumulator.append(range->content);
        }
    }
}

void collectVisibleNames(
    AtspiAccessible* accessible,
    const std::size_t depth,
    SearchBudget& budget,
    localflow::platform::AccessibilityTextAccumulator& accumulator) {
    if (!accessible || budget.expired() || accumulator.stopped()) {
        return;
    }

    StateSetPtr states(atspi_accessible_get_state_set(accessible));
    const bool stateKnown = states != nullptr;
    const bool defunct = stateKnown &&
        atspi_state_set_contains(states.get(), ATSPI_STATE_DEFUNCT);
    const bool showing = stateKnown &&
        atspi_state_set_contains(states.get(), ATSPI_STATE_SHOWING);
    const auto security = stateKnown && !defunct
        ? objectSecurity(accessible, budget)
        : FieldSecurity::unknown;
    const auto decision = accumulator.beginNode(
        depth, showing && !defunct, security != FieldSecurity::non_secure);
    if (decision !=
        localflow::platform::AccessibilityNodeDecision::inspect) {
        return;
    }

    GError* error = nullptr;
    auto name = accessibleString(
        atspi_accessible_get_name(accessible, &error), error);
    if (!name.empty()) {
        (void)accumulator.append(std::move(name));
    }
    appendVisibleTextRanges(accessible, budget, accumulator);
    if (budget.expired() || accumulator.stopped()) {
        return;
    }

    const int childCount =
        atspi_accessible_get_child_count(accessible, &error);
    if (error || childCount <= 0) {
        clearError(error);
        return;
    }
    for (int index = 0; index < childCount && !budget.expired() &&
         !accumulator.stopped(); ++index) {
        AccessiblePtr child(
            atspi_accessible_get_child_at_index(accessible, index, &error));
        if (error || !child) {
            clearError(error);
            continue;
        }
        collectVisibleNames(
            child.get(), depth + 1, budget, accumulator);
    }
}

localflow::platform::AccessibilityTextSnapshot
captureVisibleAccessibilityTextNative(
    const ApplicationInfo& expected,
    localflow::platform::AccessibilityTextLimits limits) {
    localflow::platform::AccessibilityTextAccumulator accumulator(limits);
    if (!expected.focusedTarget || !expected.focusedTarget->focused ||
        !expected.focusedTarget->editable ||
        expected.focusedTarget->security != FieldSecurity::non_secure) {
        return std::move(accumulator).finish();
    }

    AtSpiRuntime runtime;
    if (!runtime.ready() || accumulator.stopped()) {
        return std::move(accumulator).finish();
    }
    std::unique_lock<std::mutex> operationLock(
        AtSpiRuntime::operationMutex(), std::try_to_lock);
    if (!operationLock.owns_lock()) {
        return std::move(accumulator).finish();
    }

    const auto nodeCount = std::min<std::size_t>(
        limits.maximumNodes, static_cast<std::size_t>(INT_MAX));
    SearchBudget budget(static_cast<int>(nodeCount), limits.timeBudget);
    auto application = expectedApplication(expected, budget);
    if (!application || budget.exhausted) {
        return std::move(accumulator).finish();
    }
    auto captured = captureFocusedTarget(application.get(), budget);
    if (!captured ||
        !validateFocusedTarget(expected, captured.value().information).ok() ||
        budget.exhausted) {
        return std::move(accumulator).finish();
    }
    auto root = visibleWindowForFocus(captured.value().focused.get(), budget);
    if (!root || budget.exhausted || budget.remaining <= 0 ||
        accumulator.stopped()) {
        return std::move(accumulator).finish();
    }

    // The accumulator was started before any native calls, so its deadline
    // covers target revalidation as well as the visible-tree walk. SearchBudget
    // carries the same deadline for AT-SPI-specific operations.
    collectVisibleNames(root.get(), 0, budget, accumulator);
    return std::move(accumulator).finish();
}

Status runtimeUnavailable() {
    return Status::failure(
        ErrorCode::service_unavailable,
        "LocalFlow could not connect to the AT-SPI2 accessibility bus.",
        "Enable desktop accessibility and restart LocalFlow in the graphical session.");
}

Status insertCaptured(CapturedTarget& captured, const std::string& utf8Text) {
    if (!captured.editable) {
        return Status::failure(
            ErrorCode::not_editable,
            "The focused control does not expose AT-SPI2 EditableText.");
    }

    // Re-read focus immediately before touching the caret. We then insert
    // through this same retained object instead of performing a blind global
    // paste or resolving a second object after validation.
    atspi_accessible_clear_cache_single(captured.focused.get());
    if (!hasState(captured.focused.get(), ATSPI_STATE_FOCUSED) ||
        hasState(captured.focused.get(), ATSPI_STATE_DEFUNCT)) {
        return Status::failure(
            ErrorCode::focus_changed,
            "The focused field changed immediately before AT-SPI insertion.",
            "Nothing was inserted. Paste the transcript manually if appropriate.");
    }

    auto* text = atspi_accessible_get_text_iface(captured.editable.get());
    auto* editableText =
        atspi_accessible_get_editable_text_iface(captured.editable.get());
    if (!text || !editableText) {
        return Status::failure(
            ErrorCode::not_editable,
            "The focused control exposes incomplete AT-SPI2 text interfaces.");
    }

    GError* error = nullptr;
    const int caret = atspi_text_get_caret_offset(text, &error);
    if (error || caret < 0) {
        const auto detail = errorMessage(
            error, "The focused control did not provide a valid caret position.");
        clearError(error);
        return Status::failure(ErrorCode::protocol_error, detail);
    }

    const gboolean inserted = atspi_editable_text_insert_text(
        editableText,
        caret,
        utf8Text.c_str(),
        static_cast<int>(utf8Text.size()),
        &error);
    if (!inserted || error) {
        const auto detail = errorMessage(
            error, "The focused application rejected AT-SPI2 text insertion.");
        clearError(error);
        return Status::failure(
            ErrorCode::permission_denied,
            detail,
            "LocalFlow can use clipboard paste only after revalidating the same target.");
    }
    return Status::success();
}

class AtSpiFocusedTargetProvider final : public FocusedTargetProvider {
public:
    Result<ApplicationInfo> snapshotFocusedTarget() override {
        std::lock_guard<std::mutex> lock(AtSpiRuntime::operationMutex());
        if (!runtime_.ready()) {
            return Result<ApplicationInfo>::failure(runtimeUnavailable());
        }
        auto captured = captureFocusedTarget();
        if (!captured) {
            return Result<ApplicationInfo>::failure(captured.status());
        }
        return Result<ApplicationInfo>::success(
            std::move(captured).value().information);
    }

private:
    AtSpiRuntime runtime_;
};

class AtSpiInserter final : public AccessibilityTextInserter {
public:
    Status insertAtCaret(const std::string& utf8Text) override {
        return insert(utf8Text, nullptr);
    }

    Status insertAtCaret(
        const std::string& utf8Text,
        const ApplicationInfo& expectedTarget) override {
        return insert(utf8Text, &expectedTarget);
    }

private:
    Status insert(
        const std::string& utf8Text,
        const ApplicationInfo* expectedTarget) {
        if (!runtime_.ready()) return runtimeUnavailable();
        if (utf8Text.size() > static_cast<std::size_t>(INT_MAX)) {
            return Status::failure(
                ErrorCode::invalid_argument,
                "The transcript is too large for AT-SPI2 insertion.");
        }

        std::lock_guard<std::mutex> lock(AtSpiRuntime::operationMutex());
        auto captured = captureFocusedTarget();
        if (!captured) return captured.status();
        auto& value = captured.value();
        const auto validation = expectedTarget
            ? validateFocusedTarget(*expectedTarget, value.information)
            : validateFocusedTarget(value.information, value.information);
        if (!validation.ok()) return validation;
        return insertCaptured(value, utf8Text);
    }

    AtSpiRuntime runtime_;
};

#else

localflow::platform::AccessibilityTextSnapshot
captureVisibleAccessibilityTextNative(
    const ApplicationInfo&,
    localflow::platform::AccessibilityTextLimits) {
    return {};
}

Status notBuiltStatus() {
    return Status::failure(
        ErrorCode::missing_dependency,
        "This LocalFlow build does not contain the AT-SPI2 adapter.",
        "Rebuild with the at-spi2-core development package installed.");
}

class AtSpiFocusedTargetProvider final : public FocusedTargetProvider {
public:
    Result<ApplicationInfo> snapshotFocusedTarget() override {
        return Result<ApplicationInfo>::failure(notBuiltStatus());
    }
};

class AtSpiInserter final : public AccessibilityTextInserter {
public:
    Status insertAtCaret(const std::string&) override {
        return notBuiltStatus();
    }

    Status insertAtCaret(
        const std::string&,
        const ApplicationInfo&) override {
        return notBuiltStatus();
    }
};

#endif

}  // namespace

std::unique_ptr<AccessibilityTextInserter> makeAtSpiInserter() {
    return std::make_unique<AtSpiInserter>();
}

std::shared_ptr<FocusedTargetProvider> makeAtSpiFocusedTargetProvider() {
    return std::make_shared<AtSpiFocusedTargetProvider>();
}

}  // namespace localflow::platform::linux::detail

namespace localflow::platform::linux {

localflow::platform::AccessibilityTextSnapshot
captureVisibleAccessibilityText(
    const ApplicationInfo& expected,
    localflow::platform::AccessibilityTextLimits limits) noexcept {
    try {
        return detail::captureVisibleAccessibilityTextNative(
            expected, std::move(limits));
    } catch (...) {
        return {};
    }
}

}  // namespace localflow::platform::linux
