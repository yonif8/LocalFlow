#pragma once

#include "localflow/linux/GlobalShortcut.hpp"
#include "localflow/linux/Status.hpp"

#include <cstdint>
#include <string>

namespace localflow::platform::linux::detail {

// Portal requests finish asynchronously with a small numeric response code.
// Keeping this translation independent from QDBus makes denial/cancellation
// behavior deterministic and directly unit-testable.
[[nodiscard]] Status portalResponseStatus(
    std::uint32_t response,
    const std::string& operation,
    const std::string& permissionRemediation = {});

// Converts the platform-neutral shortcut into the freedesktop shortcuts
// specification (for example CTRL+SHIFT+F8).
[[nodiscard]] std::string portalShortcutTrigger(const ShortcutSpec& shortcut);

}  // namespace localflow::platform::linux::detail
