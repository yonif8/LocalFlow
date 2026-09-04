#pragma once

#include <chrono>
#include <string>
#include <vector>

namespace localflow::platform::linux::detail {

struct CommandResult {
    bool launched{false};
    bool timedOut{false};
    int exitCode{-1};
    std::string standardOutput;
    std::string standardError;
};

[[nodiscard]] bool executableOnPath(const std::string& name);

[[nodiscard]] CommandResult runCommand(
    const std::vector<std::string>& arguments,
    const std::string& standardInput = {},
    std::chrono::milliseconds timeout = std::chrono::milliseconds(1500));

}  // namespace localflow::platform::linux::detail
