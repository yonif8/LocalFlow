#include "InternalFactories.hpp"

#include "Process.hpp"

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace localflow::platform::linux::detail {
namespace {

constexpr const char* kPlainTextMime = "text/plain;charset=utf-8";

class CommandClipboard final : public Clipboard {
public:
    CommandClipboard(SessionType session, std::string backend)
        : session_(session), backend_(std::move(backend)) {}

    Result<ClipboardSnapshot> snapshot() override {
        const auto response = readText();
        if (!response.launched || response.timedOut || response.exitCode != 0) {
            return Result<ClipboardSnapshot>::failure(commandFailure(
                "read the current clipboard", response));
        }
        ClipboardSnapshot snapshot;
        snapshot.payloads[kPlainTextMime] = std::vector<std::uint8_t>(
            response.standardOutput.begin(), response.standardOutput.end());
        return Result<ClipboardSnapshot>::success(std::move(snapshot));
    }

    Status setText(const std::string& utf8Text) override {
        const auto status = writeText(utf8Text);
        if (status.ok()) transientText_ = utf8Text;
        return status;
    }

    Status restore(const ClipboardSnapshot& snapshot) override {
        if (!transientText_.has_value()) {
            return Status::failure(
                ErrorCode::not_configured,
                "No command-clipboard transaction is active.");
        }

        // Command tools can only compare the visible plain text. If reading
        // fails or the text changed, prefer preserving a possible user copy
        // over restoring stale content.
        const auto current = readText();
        const bool stillTransient = current.launched && !current.timedOut &&
                                    current.exitCode == 0 &&
                                    current.standardOutput == *transientText_;
        transientText_.reset();
        if (!stillTransient) return Status::success();

        const auto found = snapshot.payloads.find(kPlainTextMime);
        if (found == snapshot.payloads.end()) {
            return Status::failure(
                ErrorCode::protocol_error,
                "The clipboard snapshot did not contain UTF-8 plain text.");
        }
        return writeText(std::string(found->second.begin(), found->second.end()));
    }

private:
    CommandResult readText() const {
        if (session_ == SessionType::wayland && executableOnPath("wl-paste")) {
            return runCommand({"wl-paste", "--no-newline", "--type", kPlainTextMime});
        }
        if (session_ == SessionType::x11 && executableOnPath("xclip")) {
            return runCommand({"xclip", "-selection", "clipboard", "-out"});
        }
        if (session_ == SessionType::x11 && executableOnPath("xsel")) {
            return runCommand({"xsel", "--clipboard", "--output"});
        }
        return {};
    }

    Status writeText(const std::string& text) const {
        CommandResult response;
        if (session_ == SessionType::wayland && executableOnPath("wl-copy")) {
            response = runCommand(
                {"wl-copy", "--type", kPlainTextMime}, text, std::chrono::milliseconds(2500));
        } else if (session_ == SessionType::x11 && executableOnPath("xclip")) {
            response = runCommand(
                {"xclip", "-selection", "clipboard", "-in"}, text,
                std::chrono::milliseconds(2500));
        } else if (session_ == SessionType::x11 && executableOnPath("xsel")) {
            response = runCommand(
                {"xsel", "--clipboard", "--input"}, text,
                std::chrono::milliseconds(2500));
        }
        if (!response.launched || response.timedOut || response.exitCode != 0) {
            return commandFailure("write the clipboard", response);
        }
        return Status::success();
    }

    Status commandFailure(const std::string& operation, const CommandResult& response) const {
        if (!response.launched) {
            return Status::failure(
                ErrorCode::missing_dependency,
                "No supported clipboard command is installed for " + std::string(toString(session_)) + ".",
                session_ == SessionType::wayland
                    ? "Install wl-clipboard."
                    : "Install xclip or xsel.");
        }
        if (response.timedOut) {
            return Status::failure(
                ErrorCode::timed_out,
                "The " + backend_ + " backend timed out while trying to " + operation + ".");
        }
        return Status::failure(
            ErrorCode::io_error,
            "The " + backend_ + " backend could not " + operation +
                (response.standardError.empty() ? "." : ": " + response.standardError));
    }

    SessionType session_;
    std::string backend_;
    std::optional<std::string> transientText_;
};

}  // namespace

std::unique_ptr<Clipboard> makeCommandClipboard(
    SessionType session,
    std::string backend) {
    return std::make_unique<CommandClipboard>(session, std::move(backend));
}

}  // namespace localflow::platform::linux::detail
