#include "localflow/windows/FocusedTextTarget.hpp"

#ifdef _WIN32

#include "localflow/windows/WinError.hpp"

#include <UIAutomation.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cwctype>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace localflow::windows {
namespace {

using Microsoft::WRL::ComPtr;

class ComApartment final {
public:
    ComApartment() noexcept {
        result_ = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        initialized_ = SUCCEEDED(result_);
        if (result_ == RPC_E_CHANGED_MODE) {
            result_ = S_OK;
        }
    }

    ~ComApartment() {
        if (initialized_) {
            CoUninitialize();
        }
    }

    [[nodiscard]] HRESULT result() const noexcept { return result_; }

private:
    HRESULT result_{E_FAIL};
    bool initialized_{false};
};

class SafeArrayOwner final {
public:
    ~SafeArrayOwner() {
        if (value != nullptr) {
            SafeArrayDestroy(value);
        }
    }

    SafeArrayOwner(const SafeArrayOwner&) = delete;
    SafeArrayOwner& operator=(const SafeArrayOwner&) = delete;
    SafeArrayOwner() = default;

    SAFEARRAY* value{nullptr};
};

struct TargetObservation {
    FocusedTextTargetStatus status{FocusedTextTargetStatus::system_error};
    FocusedTextTargetFingerprint fingerprint;
    std::error_code error;
};

std::uintptr_t handle_value(const void* handle) noexcept {
    return reinterpret_cast<std::uintptr_t>(handle);
}

bool read_required_bool(
    IUIAutomationElement* element,
    const PROPERTYID property,
    bool& value,
    HRESULT& result) noexcept {
    VARIANT raw;
    VariantInit(&raw);
    result = element->GetCurrentPropertyValueEx(property, TRUE, &raw);
    const bool valid = SUCCEEDED(result) && raw.vt == VT_BOOL;
    if (valid) {
        value = raw.boolVal == VARIANT_TRUE;
    }
    VariantClear(&raw);
    return valid;
}

std::optional<std::vector<std::int32_t>> runtime_id(
    IUIAutomationElement* element,
    HRESULT& result) noexcept {
    SafeArrayOwner runtime;
    result = element->GetRuntimeId(&runtime.value);
    if (FAILED(result) || runtime.value == nullptr
        || SafeArrayGetDim(runtime.value) != 1U) {
        return std::nullopt;
    }

    LONG lower = 0;
    LONG upper = -1;
    result = SafeArrayGetLBound(runtime.value, 1U, &lower);
    if (FAILED(result)) {
        return std::nullopt;
    }
    result = SafeArrayGetUBound(runtime.value, 1U, &upper);
    if (FAILED(result) || upper < lower) {
        return std::nullopt;
    }
    const auto count = static_cast<unsigned long long>(
                           static_cast<long long>(upper)
                           - static_cast<long long>(lower))
        + 1ULL;
    if (count == 0ULL || count > 128ULL) {
        result = E_INVALIDARG;
        return std::nullopt;
    }

    std::vector<std::int32_t> values;
    values.reserve(static_cast<std::size_t>(count));
    for (LONG index = lower; index <= upper; ++index) {
        LONG value = 0;
        result = SafeArrayGetElement(runtime.value, &index, &value);
        if (FAILED(result)) {
            return std::nullopt;
        }
        values.push_back(static_cast<std::int32_t>(value));
        if (index == std::numeric_limits<LONG>::max()) {
            break;
        }
    }
    return values;
}

bool has_writable_value_pattern(IUIAutomationElement* element) noexcept {
    ComPtr<IUIAutomationValuePattern> value_pattern;
    if (FAILED(element->GetCurrentPatternAs(
            UIA_ValuePatternId, IID_PPV_ARGS(&value_pattern)))) {
        return false;
    }
    BOOL read_only = TRUE;
    return SUCCEEDED(value_pattern->get_CurrentIsReadOnly(&read_only))
        && read_only == FALSE;
}

bool has_writable_active_caret(IUIAutomationElement* element) noexcept {
    // UIA_IsReadOnlyAttributeId is 40015 on every supported Windows SDK. Some
    // cross-compilation headers omit the symbolic name despite exposing
    // TextPattern2, so retain the ABI-stable identifier locally.
    constexpr TEXTATTRIBUTEID kIsReadOnlyAttributeId = 40015;
    ComPtr<IUIAutomationTextPattern2> text_pattern;
    if (FAILED(element->GetCurrentPatternAs(
            UIA_TextPattern2Id, IID_PPV_ARGS(&text_pattern)))) {
        return false;
    }
    BOOL active = FALSE;
    ComPtr<IUIAutomationTextRange> caret;
    if (FAILED(text_pattern->GetCaretRange(&active, &caret)) || active == FALSE
        || caret == nullptr) {
        return false;
    }
    VARIANT read_only;
    VariantInit(&read_only);
    const HRESULT result =
        caret->GetAttributeValue(kIsReadOnlyAttributeId, &read_only);
    const bool writable = SUCCEEDED(result) && read_only.vt == VT_BOOL
        && read_only.boolVal == VARIANT_FALSE;
    VariantClear(&read_only);
    return writable;
}

TargetObservation observe_with_uiautomation(
    const ForegroundWindowIdentity& window) noexcept {
    TargetObservation observation;
    ComApartment apartment;
    if (FAILED(apartment.result())) {
        observation.status = FocusedTextTargetStatus::automation_unavailable;
        observation.error = hresult_error(apartment.result());
        return observation;
    }

    ComPtr<IUIAutomation> automation;
    HRESULT result = CoCreateInstance(
        __uuidof(CUIAutomation), nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&automation));
    if (FAILED(result)) {
        observation.status = FocusedTextTargetStatus::automation_unavailable;
        observation.error = hresult_error(result);
        return observation;
    }

    ComPtr<IUIAutomationElement> element;
    result = automation->GetFocusedElement(&element);
    if (FAILED(result) || element == nullptr) {
        observation.status = FocusedTextTargetStatus::no_focused_control;
        if (FAILED(result)) {
            observation.error = hresult_error(result);
        }
        return observation;
    }

    int process_id = 0;
    result = element->get_CurrentProcessId(&process_id);
    if (FAILED(result)) {
        observation.status = FocusedTextTargetStatus::automation_unavailable;
        observation.error = hresult_error(result);
        return observation;
    }
    if (process_id <= 0 || static_cast<std::uint32_t>(process_id) != window.process_id
        || !is_still_foreground(window)) {
        observation.status = FocusedTextTargetStatus::target_changed;
        return observation;
    }

    bool password = false;
    if (!read_required_bool(
            element.Get(), UIA_IsPasswordPropertyId, password, result)) {
        observation.status = FocusedTextTargetStatus::automation_unavailable;
        observation.error = hresult_error(FAILED(result) ? result : E_NOINTERFACE);
        return observation;
    }
    if (password) {
        observation.status = FocusedTextTargetStatus::protected_content;
        observation.fingerprint.protected_content = true;
        return observation;
    }

    bool enabled = false;
    bool has_focus = false;
    if (!read_required_bool(element.Get(), UIA_IsEnabledPropertyId, enabled, result)
        || !read_required_bool(
            element.Get(), UIA_HasKeyboardFocusPropertyId, has_focus, result)) {
        observation.status = FocusedTextTargetStatus::automation_unavailable;
        observation.error = hresult_error(FAILED(result) ? result : E_NOINTERFACE);
        return observation;
    }
    if (!enabled || !has_focus) {
        observation.status = FocusedTextTargetStatus::not_editable;
        return observation;
    }

    const bool editable = has_writable_value_pattern(element.Get())
        || has_writable_active_caret(element.Get());
    if (!editable) {
        observation.status = FocusedTextTargetStatus::not_editable;
        return observation;
    }

    auto id = runtime_id(element.Get(), result);
    if (!id.has_value()) {
        observation.status = FocusedTextTargetStatus::automation_unavailable;
        observation.error = hresult_error(FAILED(result) ? result : E_NOINTERFACE);
        return observation;
    }

    UIA_HWND native_handle = nullptr;
    if (FAILED(element->get_CurrentNativeWindowHandle(&native_handle))) {
        native_handle = nullptr;
    }
    observation.fingerprint.backend =
        FocusedTextTargetBackend::ui_automation_runtime_id;
    observation.fingerprint.foreground_window = handle_value(window.handle);
    observation.fingerprint.process_id = window.process_id;
    observation.fingerprint.native_focus_window = handle_value(native_handle);
    observation.fingerprint.automation_runtime_id = std::move(*id);
    observation.fingerprint.editable = true;
    observation.status = FocusedTextTargetStatus::ready;
    return observation;
}

bool is_native_edit_class(std::wstring class_name) {
    std::transform(
        class_name.begin(), class_name.end(), class_name.begin(),
        [](const wchar_t value) { return static_cast<wchar_t>(std::towlower(value)); });
    return class_name == L"edit" || class_name.rfind(L"richedit", 0) == 0;
}

TargetObservation observe_native_edit(
    const ForegroundWindowIdentity& window) noexcept {
    TargetObservation observation;
    GUITHREADINFO information{};
    information.cbSize = static_cast<DWORD>(sizeof(information));
    if (GetGUIThreadInfo(static_cast<DWORD>(window.thread_id), &information) == FALSE) {
        observation.status = FocusedTextTargetStatus::no_focused_control;
        observation.error = last_win32_error();
        return observation;
    }
    const HWND focus = information.hwndFocus;
    if (focus == nullptr || IsWindow(focus) == FALSE) {
        observation.status = FocusedTextTargetStatus::no_focused_control;
        return observation;
    }
    const HWND root = GetAncestor(focus, GA_ROOT);
    if (root != window.handle || !is_still_foreground(window)) {
        observation.status = FocusedTextTargetStatus::target_changed;
        return observation;
    }
    DWORD process_id = 0;
    if (GetWindowThreadProcessId(focus, &process_id) == 0
        || process_id != window.process_id) {
        observation.status = FocusedTextTargetStatus::target_changed;
        return observation;
    }

    std::array<wchar_t, 256> class_name{};
    const int class_length = GetClassNameW(
        focus, class_name.data(), static_cast<int>(class_name.size()));
    if (class_length <= 0
        || !is_native_edit_class(std::wstring(
            class_name.data(), static_cast<std::size_t>(class_length)))) {
        observation.status = FocusedTextTargetStatus::not_editable;
        return observation;
    }

    SetLastError(ERROR_SUCCESS);
    const LONG_PTR style = GetWindowLongPtrW(focus, GWL_STYLE);
    if (style == 0 && GetLastError() != ERROR_SUCCESS) {
        observation.status = FocusedTextTargetStatus::system_error;
        observation.error = last_win32_error();
        return observation;
    }
    if (IsWindowEnabled(focus) == FALSE
        || (style & static_cast<LONG_PTR>(ES_READONLY)) != 0) {
        observation.status = FocusedTextTargetStatus::not_editable;
        return observation;
    }
    if ((style & static_cast<LONG_PTR>(ES_PASSWORD)) != 0) {
        observation.status = FocusedTextTargetStatus::protected_content;
        observation.fingerprint.protected_content = true;
        return observation;
    }

    DWORD_PTR password_character = 0;
    SetLastError(ERROR_SUCCESS);
    if (SendMessageTimeoutW(
            focus, EM_GETPASSWORDCHAR, 0, 0, SMTO_ABORTIFHUNG | SMTO_BLOCK, 50,
            &password_character) == 0) {
        observation.status = FocusedTextTargetStatus::automation_unavailable;
        const DWORD code = GetLastError();
        if (code != ERROR_SUCCESS) {
            observation.error = win32_error(code);
        }
        return observation;
    }
    if (password_character != 0) {
        observation.status = FocusedTextTargetStatus::protected_content;
        observation.fingerprint.protected_content = true;
        return observation;
    }

    observation.fingerprint.backend = FocusedTextTargetBackend::native_edit_control;
    observation.fingerprint.foreground_window = handle_value(window.handle);
    observation.fingerprint.process_id = window.process_id;
    observation.fingerprint.native_focus_window = handle_value(focus);
    observation.fingerprint.editable = true;
    observation.status = FocusedTextTargetStatus::ready;
    return observation;
}

TargetObservation observe_target(
    const ForegroundWindowIdentity& window,
    const std::optional<FocusedTextTargetBackend> required_backend) noexcept {
    if (!required_backend.has_value()
        || *required_backend == FocusedTextTargetBackend::ui_automation_runtime_id) {
        TargetObservation uia = observe_with_uiautomation(window);
        if (uia.status == FocusedTextTargetStatus::ready
            || uia.status == FocusedTextTargetStatus::protected_content
            || required_backend.has_value()) {
            return uia;
        }
    }
    return observe_native_edit(window);
}

FocusedTextTargetStatus status_for_match(const FocusedTextTargetMatch match) noexcept {
    switch (match) {
        case FocusedTextTargetMatch::matched:
            return FocusedTextTargetStatus::ready;
        case FocusedTextTargetMatch::invalid_expected_identity:
            return FocusedTextTargetStatus::invalid_target;
        case FocusedTextTargetMatch::became_protected:
            return FocusedTextTargetStatus::protected_content;
        case FocusedTextTargetMatch::became_non_editable:
            return FocusedTextTargetStatus::not_editable;
        case FocusedTextTargetMatch::foreground_changed:
        case FocusedTextTargetMatch::focused_field_changed:
            return FocusedTextTargetStatus::target_changed;
    }
    return FocusedTextTargetStatus::system_error;
}

}  // namespace

FocusedTextTargetCapture capture_focused_text_target() noexcept {
    FocusedTextTargetCapture capture;
    std::error_code foreground_error;
    auto window = query_foreground_window(foreground_error);
    if (!window.has_value()) {
        capture.status = FocusedTextTargetStatus::no_foreground_window;
        capture.error = foreground_error;
        return capture;
    }

    TargetObservation observation = observe_target(*window, std::nullopt);
    capture.status = observation.status;
    capture.error = observation.error;
    if (observation.status == FocusedTextTargetStatus::ready
        && is_valid_focused_text_target(observation.fingerprint)) {
        capture.target = FocusedTextTargetIdentity{
            std::move(*window), std::move(observation.fingerprint)};
    } else if (observation.status == FocusedTextTargetStatus::ready) {
        capture.status = FocusedTextTargetStatus::invalid_target;
    }
    return capture;
}

FocusedTextTargetValidation validate_focused_text_target(
    const FocusedTextTargetIdentity& expected) noexcept {
    FocusedTextTargetValidation validation;
    if (!is_valid_focused_text_target(expected.fingerprint)) {
        validation.status = FocusedTextTargetStatus::invalid_target;
        return validation;
    }

    std::error_code foreground_error;
    auto current_window = query_foreground_window(foreground_error);
    if (!current_window.has_value()) {
        validation.status = FocusedTextTargetStatus::no_foreground_window;
        validation.error = foreground_error;
        return validation;
    }
    if (current_window->handle != expected.window.handle
        || current_window->process_id != expected.window.process_id) {
        validation.status = FocusedTextTargetStatus::target_changed;
        return validation;
    }

    TargetObservation current = observe_target(
        *current_window, expected.fingerprint.backend);
    validation.error = current.error;
    if (current.status != FocusedTextTargetStatus::ready) {
        validation.status = current.status;
        return validation;
    }
    validation.status = status_for_match(compare_focused_text_targets(
        expected.fingerprint, current.fingerprint));
    return validation;
}

}  // namespace localflow::windows

#endif
