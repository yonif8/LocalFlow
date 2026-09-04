#pragma once

#include <optional>
#include <string>
#include <utility>

namespace localflow::platform::linux {

enum class ErrorCode {
    none,
    invalid_argument,
    unsupported_session,
    missing_dependency,
    service_unavailable,
    permission_required,
    permission_denied,
    not_configured,
    not_editable,
    secure_field,
    focus_changed,
    busy,
    cancelled,
    timed_out,
    io_error,
    protocol_error,
    internal_error,
};

struct Status {
    ErrorCode code{ErrorCode::none};
    std::string message;
    std::string remediation;

    [[nodiscard]] bool ok() const noexcept { return code == ErrorCode::none; }
    explicit operator bool() const noexcept { return ok(); }

    static Status success() { return {}; }

    static Status failure(
        ErrorCode error,
        std::string description,
        std::string recovery = {}) {
        return {error, std::move(description), std::move(recovery)};
    }
};

template <typename T>
class Result {
public:
    static Result success(T value) {
        return Result(Status::success(), std::move(value));
    }

    static Result failure(Status status) {
        return Result(std::move(status), std::nullopt);
    }

    [[nodiscard]] bool ok() const noexcept { return status_.ok(); }
    explicit operator bool() const noexcept { return ok(); }

    [[nodiscard]] const Status& status() const noexcept { return status_; }
    [[nodiscard]] const T& value() const& { return *value_; }
    [[nodiscard]] T& value() & { return *value_; }
    [[nodiscard]] T&& value() && { return std::move(*value_); }

private:
    Result(Status status, std::optional<T> value)
        : status_(std::move(status)), value_(std::move(value)) {}

    Status status_;
    std::optional<T> value_;
};

}  // namespace localflow::platform::linux
