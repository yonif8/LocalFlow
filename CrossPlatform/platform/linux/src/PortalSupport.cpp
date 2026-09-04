#include "PortalSupport.hpp"

namespace localflow::platform::linux::detail {

Status portalResponseStatus(
    std::uint32_t response,
    const std::string& operation,
    const std::string& permissionRemediation) {
    switch (response) {
        case 0:
            return Status::success();
        case 1:
            return Status::failure(
                ErrorCode::cancelled,
                "The " + operation + " request was cancelled.",
                permissionRemediation);
        case 2:
            return Status::failure(
                ErrorCode::permission_denied,
                "The desktop did not grant the " + operation + " request.",
                permissionRemediation);
        default:
            return Status::failure(
                ErrorCode::protocol_error,
                "The desktop returned an unknown response (" +
                    std::to_string(response) + ") for the " + operation + " request.",
                "Update xdg-desktop-portal and the portal backend for your desktop.");
    }
}

std::string portalShortcutTrigger(const ShortcutSpec& shortcut) {
    std::string result;
    const auto append = [&result](const char* value) {
        if (!result.empty()) result += '+';
        result += value;
    };

    for (const auto modifier : shortcut.modifiers) {
        switch (modifier) {
            case Modifier::shift:
                append("SHIFT");
                break;
            case Modifier::control:
                append("CTRL");
                break;
            case Modifier::alt:
                append("ALT");
                break;
            case Modifier::super:
                append("LOGO");
                break;
        }
    }
    if (!shortcut.trigger.empty()) {
        if (!result.empty()) result += '+';
        result += shortcut.trigger;
    }
    return result;
}

}  // namespace localflow::platform::linux::detail
