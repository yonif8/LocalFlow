#include "localflow/windows/AccessibilityText.hpp"

#ifdef _WIN32

#include <Windows.h>
#include <ole2.h>
#include <UIAutomation.h>
#include <wrl/client.h>

#include <algorithm>
#include <climits>
#include <cstdint>
#include <functional>
#include <string>

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

class BstrOwner final {
public:
    ~BstrOwner() {
        if (value != nullptr) {
            SysFreeString(value);
        }
    }
    BstrOwner(const BstrOwner&) = delete;
    BstrOwner& operator=(const BstrOwner&) = delete;
    BstrOwner() = default;
    BSTR value{nullptr};
};

std::string utf8_name(BSTR value, const std::size_t maximumWideUnits) {
    if (value == nullptr || maximumWideUnits == 0) {
        return {};
    }
    std::size_t length = std::min<std::size_t>(
        static_cast<std::size_t>(SysStringLen(value)), maximumWideUnits);
    if (length != 0 && length < static_cast<std::size_t>(SysStringLen(value))) {
        const wchar_t final = value[length - 1];
        if (final >= 0xD800 && final <= 0xDBFF) {
            --length;
        }
    }
    if (length == 0 || length > static_cast<std::size_t>(INT_MAX)) {
        return {};
    }
    const int sourceLength = static_cast<int>(length);
    const int byteCount = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, value, sourceLength, nullptr, 0,
        nullptr, nullptr);
    if (byteCount <= 0) {
        return {};
    }
    std::string result(static_cast<std::size_t>(byteCount), '\0');
    if (WideCharToMultiByte(
            CP_UTF8, WC_ERR_INVALID_CHARS, value, sourceLength,
            result.data(), byteCount, nullptr, nullptr) != byteCount) {
        return {};
    }
    return result;
}

void append_visible_text_ranges(
    IUIAutomationElement* element,
    const localflow::platform::AccessibilityTextLimits& limits,
    localflow::platform::AccessibilityTextAccumulator& accumulator) {
    ComPtr<IUIAutomationTextPattern> textPattern;
    if (FAILED(element->GetCurrentPatternAs(
            UIA_TextPatternId, IID_PPV_ARGS(&textPattern))) ||
        textPattern == nullptr) {
        return;
    }
    ComPtr<IUIAutomationTextRangeArray> ranges;
    if (FAILED(textPattern->GetVisibleRanges(&ranges)) || ranges == nullptr) {
        return;
    }
    int rangeCount = 0;
    if (FAILED(ranges->get_Length(&rangeCount)) || rangeCount <= 0) {
        return;
    }
    constexpr int kMaximumVisibleRangesPerNode = 16;
    rangeCount = std::min(rangeCount, kMaximumVisibleRangesPerNode);
    const std::size_t maximumCharacters = std::max<std::size_t>(
        1, std::min<std::size_t>(
               limits.maximumFragmentUtf8Bytes,
               static_cast<std::size_t>(INT_MAX)));
    for (int index = 0;
         index < rangeCount && !accumulator.stopped(); ++index) {
        ComPtr<IUIAutomationTextRange> range;
        if (FAILED(ranges->GetElement(index, &range)) || range == nullptr) {
            continue;
        }
        BstrOwner text;
        if (SUCCEEDED(range->GetText(
                static_cast<int>(maximumCharacters), &text.value)) &&
            text.value != nullptr) {
            (void)accumulator.append(utf8_name(
                text.value, limits.maximumFragmentUtf8Bytes));
        }
    }
}

}  // namespace

localflow::platform::AccessibilityTextSnapshot
capture_visible_accessibility_text(
    const FocusedTextTargetIdentity& expected,
    localflow::platform::AccessibilityTextLimits limits) noexcept {
    try {
        localflow::platform::AccessibilityTextAccumulator accumulator(limits);
        if (expected.fingerprint.protected_content ||
            !validate_focused_text_target(expected).safe_for_insertion()) {
            return std::move(accumulator).finish();
        }

        ComApartment apartment;
        if (FAILED(apartment.result())) {
            return std::move(accumulator).finish();
        }
        ComPtr<IUIAutomation> automation;
        if (FAILED(CoCreateInstance(
                __uuidof(CUIAutomation), nullptr, CLSCTX_INPROC_SERVER,
                IID_PPV_ARGS(&automation))) || automation == nullptr) {
            return std::move(accumulator).finish();
        }

        ComPtr<IUIAutomationElement> root;
        if (FAILED(automation->ElementFromHandle(
                expected.window.handle, &root)) || root == nullptr) {
            return std::move(accumulator).finish();
        }
        int rootProcessId = 0;
        if (FAILED(root->get_CurrentProcessId(&rootProcessId)) ||
            rootProcessId <= 0 ||
            static_cast<std::uint32_t>(rootProcessId) !=
                expected.window.process_id) {
            return std::move(accumulator).finish();
        }

        ComPtr<IUIAutomationTreeWalker> walker;
        if (FAILED(automation->get_ControlViewWalker(&walker)) ||
            walker == nullptr) {
            return std::move(accumulator).finish();
        }

        bool stop = false;
        std::function<void(IUIAutomationElement*, std::size_t)> visit;
        visit = [&](IUIAutomationElement* element, const std::size_t depth) {
            if (element == nullptr || stop) {
                return;
            }

            BOOL offscreen = TRUE;
            BOOL password = TRUE;
            const bool propertiesAvailable =
                SUCCEEDED(element->get_CurrentIsOffscreen(&offscreen)) &&
                SUCCEEDED(element->get_CurrentIsPassword(&password));
            const auto decision = accumulator.beginNode(
                depth,
                propertiesAvailable && offscreen == FALSE,
                !propertiesAvailable || password != FALSE);
            if (decision ==
                localflow::platform::AccessibilityNodeDecision::stop) {
                stop = true;
                return;
            }
            if (decision ==
                localflow::platform::AccessibilityNodeDecision::skipSubtree) {
                return;
            }

            BstrOwner name;
            if (SUCCEEDED(element->get_CurrentName(&name.value)) &&
                name.value != nullptr) {
                (void)accumulator.append(utf8_name(
                    name.value, limits.maximumFragmentUtf8Bytes));
            }
            append_visible_text_ranges(element, limits, accumulator);
            if (accumulator.stopped()) {
                stop = true;
                return;
            }

            ComPtr<IUIAutomationElement> child;
            if (FAILED(walker->GetFirstChildElement(
                    element, child.ReleaseAndGetAddressOf()))) {
                return;
            }
            while (child != nullptr && !stop) {
                visit(child.Get(), depth + 1);
                if (stop) {
                    break;
                }
                ComPtr<IUIAutomationElement> sibling;
                if (FAILED(walker->GetNextSiblingElement(
                        child.Get(), sibling.ReleaseAndGetAddressOf()))) {
                    break;
                }
                child = std::move(sibling);
            }
        };
        visit(root.Get(), 0);
        return std::move(accumulator).finish();
    } catch (...) {
        return {};
    }
}

}  // namespace localflow::windows

#endif
