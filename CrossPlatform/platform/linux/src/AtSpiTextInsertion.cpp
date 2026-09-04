#include "InternalFactories.hpp"

#include <climits>
#include <memory>
#include <mutex>
#include <string>

#if defined(LOCALFLOW_LINUX_WITH_ATSPI)
#include <atspi/atspi.h>
#include <glib-object.h>
#endif

namespace localflow::platform::linux::detail {
namespace {

#if defined(LOCALFLOW_LINUX_WITH_ATSPI)

template <typename T>
struct GObjectUnref {
    void operator()(T* object) const noexcept {
        if (object) {
            g_object_unref(object);
        }
    }
};

using AccessiblePtr = std::unique_ptr<AtspiAccessible, GObjectUnref<AtspiAccessible>>;
using StateSetPtr = std::unique_ptr<AtspiStateSet, GObjectUnref<AtspiStateSet>>;

void clearError(GError*& error) {
    if (error) {
        g_error_free(error);
        error = nullptr;
    }
}

bool hasState(AtspiAccessible* accessible, AtspiStateType state) {
    StateSetPtr states(atspi_accessible_get_state_set(accessible));
    return states && atspi_state_set_contains(states.get(), state);
}

AccessiblePtr findFocused(AtspiAccessible* root, int depth, int& remaining) {
    if (!root || depth > 40 || remaining-- <= 0) {
        return {};
    }
    if (hasState(root, ATSPI_STATE_DEFUNCT)) {
        return {};
    }
    if (hasState(root, ATSPI_STATE_FOCUSED)) {
        return AccessiblePtr(static_cast<AtspiAccessible*>(g_object_ref(root)));
    }

    GError* error = nullptr;
    const int children = atspi_accessible_get_child_count(root, &error);
    if (error) {
        clearError(error);
        return {};
    }
    for (int index = 0; index < children && remaining > 0; ++index) {
        AccessiblePtr child(atspi_accessible_get_child_at_index(root, index, &error));
        if (error) {
            clearError(error);
            continue;
        }
        if (!child) {
            continue;
        }
        auto focused = findFocused(child.get(), depth + 1, remaining);
        if (focused) {
            return focused;
        }
    }
    return {};
}

AccessiblePtr editableObjectForFocus(AtspiAccessible* focused) {
    AccessiblePtr current(static_cast<AtspiAccessible*>(g_object_ref(focused)));
    for (int level = 0; current && level < 8; ++level) {
        if (atspi_accessible_get_editable_text_iface(current.get()) != nullptr) {
            return current;
        }
        GError* error = nullptr;
        AtspiAccessible* parent = atspi_accessible_get_parent(current.get(), &error);
        if (error) {
            clearError(error);
            return {};
        }
        current.reset(parent);
    }
    return {};
}

class AtSpiRuntime {
public:
    AtSpiRuntime() {
        std::lock_guard<std::mutex> lock(globalMutex());
        if (references()++ == 0 && !atspi_is_initialized()) {
            ownsInitialization() = atspi_init() == 0;
        }
    }

    ~AtSpiRuntime() {
        std::lock_guard<std::mutex> lock(globalMutex());
        if (--references() == 0 && ownsInitialization()) {
            atspi_exit();
            ownsInitialization() = false;
        }
    }

    bool ready() const { return atspi_is_initialized(); }

private:
    static std::mutex& globalMutex() {
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

class AtSpiInserter final : public AccessibilityTextInserter {
public:
    Status insertAtCaret(const std::string& utf8Text) override {
        if (!runtime_.ready()) {
            return Status::failure(
                ErrorCode::service_unavailable,
                "LocalFlow could not connect to the AT-SPI2 accessibility bus.",
                "Enable desktop accessibility and restart LocalFlow in the graphical session.");
        }
        if (utf8Text.size() > static_cast<std::size_t>(INT_MAX)) {
            return Status::failure(
                ErrorCode::invalid_argument,
                "The transcript is too large for AT-SPI2 insertion.");
        }

        AccessiblePtr desktop(atspi_get_desktop(0));
        if (!desktop) {
            return Status::failure(
                ErrorCode::service_unavailable,
                "AT-SPI2 did not return an accessible desktop.");
        }

        int remaining = 4096;
        auto focused = findFocused(desktop.get(), 0, remaining);
        if (!focused) {
            return Status::failure(
                ErrorCode::not_editable,
                "No focused AT-SPI2 text control was found.");
        }
        auto editable = editableObjectForFocus(focused.get());
        if (!editable) {
            return Status::failure(
                ErrorCode::not_editable,
                "The focused control does not expose AT-SPI2 EditableText.");
        }

        auto* text = atspi_accessible_get_text_iface(editable.get());
        auto* editableText = atspi_accessible_get_editable_text_iface(editable.get());
        if (!text || !editableText) {
            return Status::failure(
                ErrorCode::not_editable,
                "The focused control exposes incomplete AT-SPI2 text interfaces.");
        }

        GError* error = nullptr;
        const int caret = atspi_text_get_caret_offset(text, &error);
        if (error || caret < 0) {
            const std::string detail = error && error->message
                ? error->message
                : "The focused control did not provide a valid caret position.";
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
            const std::string detail = error && error->message
                ? error->message
                : "The focused application rejected AT-SPI2 text insertion.";
            clearError(error);
            return Status::failure(
                ErrorCode::permission_denied,
                detail,
                "LocalFlow will use clipboard paste fallback when it is safely available.");
        }
        return Status::success();
    }

private:
    AtSpiRuntime runtime_;
};

#else

class AtSpiInserter final : public AccessibilityTextInserter {
public:
    Status insertAtCaret(const std::string&) override {
        return Status::failure(
            ErrorCode::missing_dependency,
            "This LocalFlow build does not contain the AT-SPI2 adapter.",
            "Rebuild with the at-spi2-core development package installed.");
    }
};

#endif

}  // namespace

std::unique_ptr<AccessibilityTextInserter> makeAtSpiInserter() {
    return std::make_unique<AtSpiInserter>();
}

}  // namespace localflow::platform::linux::detail
