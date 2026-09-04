#include "Process.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <optional>
#include <poll.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace localflow::platform::linux::detail {
namespace {

constexpr std::size_t kMaximumCapturedBytes = 4 * 1024 * 1024;

void closeIfOpen(int& descriptor) {
    if (descriptor >= 0) {
        ::close(descriptor);
        descriptor = -1;
    }
}

void makeNonBlocking(int descriptor) {
    const int flags = ::fcntl(descriptor, F_GETFL, 0);
    if (flags >= 0) {
        (void)::fcntl(descriptor, F_SETFL, flags | O_NONBLOCK);
    }
}

void appendAvailable(int& descriptor, std::string& destination) {
    std::array<char, 8192> buffer{};
    while (descriptor >= 0) {
        const auto count = ::read(descriptor, buffer.data(), buffer.size());
        if (count > 0) {
            const auto allowed = std::min<std::size_t>(
                static_cast<std::size_t>(count),
                kMaximumCapturedBytes > destination.size()
                    ? kMaximumCapturedBytes - destination.size()
                    : 0);
            destination.append(buffer.data(), allowed);
            continue;
        }
        if (count == 0) {
            closeIfOpen(descriptor);
        } else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
            closeIfOpen(descriptor);
        }
        break;
    }
}

}  // namespace

bool executableOnPath(const std::string& name) {
    if (name.empty() || name.find('/') != std::string::npos) {
        return !name.empty() && ::access(name.c_str(), X_OK) == 0;
    }

    const char* rawPath = std::getenv("PATH");
    if (rawPath == nullptr) {
        return false;
    }

    const std::string path(rawPath);
    std::size_t start = 0;
    while (start <= path.size()) {
        const auto end = path.find(':', start);
        const std::string directory = path.substr(
            start,
            end == std::string::npos ? std::string::npos : end - start);
        const std::string candidate = (directory.empty() ? "." : directory) + "/" + name;
        if (::access(candidate.c_str(), X_OK) == 0) {
            return true;
        }
        if (end == std::string::npos) {
            break;
        }
        start = end + 1;
    }
    return false;
}

CommandResult runCommand(
    const std::vector<std::string>& arguments,
    const std::string& standardInput,
    std::chrono::milliseconds timeout) {
    CommandResult result;
    if (arguments.empty() || !executableOnPath(arguments.front())) {
        return result;
    }

    int inputPipe[2]{-1, -1};
    int outputPipe[2]{-1, -1};
    int errorPipe[2]{-1, -1};
    if (::pipe(inputPipe) != 0 || ::pipe(outputPipe) != 0 || ::pipe(errorPipe) != 0) {
        closeIfOpen(inputPipe[0]);
        closeIfOpen(inputPipe[1]);
        closeIfOpen(outputPipe[0]);
        closeIfOpen(outputPipe[1]);
        closeIfOpen(errorPipe[0]);
        closeIfOpen(errorPipe[1]);
        return result;
    }

    const pid_t child = ::fork();
    if (child == 0) {
        (void)::dup2(inputPipe[0], STDIN_FILENO);
        (void)::dup2(outputPipe[1], STDOUT_FILENO);
        (void)::dup2(errorPipe[1], STDERR_FILENO);

        ::close(inputPipe[0]);
        ::close(inputPipe[1]);
        ::close(outputPipe[0]);
        ::close(outputPipe[1]);
        ::close(errorPipe[0]);
        ::close(errorPipe[1]);

        std::vector<char*> argv;
        argv.reserve(arguments.size() + 1);
        for (const auto& argument : arguments) {
            argv.push_back(const_cast<char*>(argument.c_str()));
        }
        argv.push_back(nullptr);
        ::execvp(argv[0], argv.data());
        _exit(127);
    }

    if (child < 0) {
        closeIfOpen(inputPipe[0]);
        closeIfOpen(inputPipe[1]);
        closeIfOpen(outputPipe[0]);
        closeIfOpen(outputPipe[1]);
        closeIfOpen(errorPipe[0]);
        closeIfOpen(errorPipe[1]);
        return result;
    }

    result.launched = true;
    closeIfOpen(inputPipe[0]);
    closeIfOpen(outputPipe[1]);
    closeIfOpen(errorPipe[1]);
    makeNonBlocking(inputPipe[1]);
    makeNonBlocking(outputPipe[0]);
    makeNonBlocking(errorPipe[0]);

    std::size_t inputOffset = 0;
    if (standardInput.empty()) {
        closeIfOpen(inputPipe[1]);
    }

    const auto deadline = std::chrono::steady_clock::now() + timeout;
    bool childExited = false;
    int waitStatus = 0;
    std::optional<std::chrono::steady_clock::time_point> drainDeadline;

    while (!childExited || outputPipe[0] >= 0 || errorPipe[0] >= 0) {
        if (std::chrono::steady_clock::now() >= deadline && !childExited) {
            result.timedOut = true;
            (void)::kill(child, SIGTERM);
            for (int attempt = 0; attempt < 10; ++attempt) {
                if (::waitpid(child, &waitStatus, WNOHANG) == child) {
                    childExited = true;
                    break;
                }
                ::usleep(10'000);
            }
            if (!childExited) {
                (void)::kill(child, SIGKILL);
                (void)::waitpid(child, &waitStatus, 0);
                childExited = true;
            }
        }

        std::array<pollfd, 3> descriptors{{
            {outputPipe[0], static_cast<short>(outputPipe[0] >= 0 ? POLLIN : 0), 0},
            {errorPipe[0], static_cast<short>(errorPipe[0] >= 0 ? POLLIN : 0), 0},
            {inputPipe[1], static_cast<short>(inputPipe[1] >= 0 ? POLLOUT : 0), 0},
        }};
        (void)::poll(descriptors.data(), descriptors.size(), 20);

        appendAvailable(outputPipe[0], result.standardOutput);
        appendAvailable(errorPipe[0], result.standardError);

        if (inputPipe[1] >= 0 && inputOffset < standardInput.size()) {
            const auto count = ::write(
                inputPipe[1],
                standardInput.data() + inputOffset,
                standardInput.size() - inputOffset);
            if (count > 0) {
                inputOffset += static_cast<std::size_t>(count);
            } else if (count < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                closeIfOpen(inputPipe[1]);
            }
            if (inputOffset == standardInput.size()) {
                closeIfOpen(inputPipe[1]);
            }
        }

        if (!childExited && ::waitpid(child, &waitStatus, WNOHANG) == child) {
            childExited = true;
            closeIfOpen(inputPipe[1]);
            drainDeadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
        }

        if (childExited) {
            appendAvailable(outputPipe[0], result.standardOutput);
            appendAvailable(errorPipe[0], result.standardError);
            // Clipboard owners such as xclip intentionally daemonize and may
            // inherit stdout/stderr. Do not wait forever for that grandchild.
            if (drainDeadline && std::chrono::steady_clock::now() >= *drainDeadline) {
                closeIfOpen(outputPipe[0]);
                closeIfOpen(errorPipe[0]);
            }
        }
    }

    closeIfOpen(inputPipe[1]);
    closeIfOpen(outputPipe[0]);
    closeIfOpen(errorPipe[0]);

    if (!result.timedOut) {
        if (WIFEXITED(waitStatus)) {
            result.exitCode = WEXITSTATUS(waitStatus);
        } else if (WIFSIGNALED(waitStatus)) {
            result.exitCode = 128 + WTERMSIG(waitStatus);
        }
    }
    return result;
}

}  // namespace localflow::platform::linux::detail
