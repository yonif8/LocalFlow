#ifdef linux
#undef linux
#endif

#include "InternalFactories.hpp"

#ifndef LOCALFLOW_LINUX_WITH_QT_CLIPBOARD
#define LOCALFLOW_LINUX_WITH_QT_CLIPBOARD 0
#endif

#if LOCALFLOW_LINUX_WITH_QT_CLIPBOARD

#include <QAbstractEventDispatcher>
#include <QBuffer>
#include <QByteArray>
#include <QClipboard>
#include <QCoreApplication>
#include <QGuiApplication>
#include <QImage>
#include <QMetaObject>
#include <QMimeData>
#include <QPixmap>
#include <QThread>
#include <QUuid>
#include <QVariant>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace localflow::platform::linux::detail {
namespace {

constexpr auto kTransactionMime =
    "application/x-localflow-clipboard-transaction";
constexpr auto kUtf8TextMime = "text/plain;charset=utf-8";
constexpr auto kGuiDispatchStartTimeout = std::chrono::seconds(1);

Status guiUnavailableStatus() {
    return Status::failure(
        ErrorCode::service_unavailable,
        "The Qt GUI clipboard is unavailable.",
        "Start LocalFlow inside a graphical desktop session and try again.");
}

template <typename ResultType, typename Work>
ResultType onGuiThread(ResultType dispatchFailure, Work work) {
    auto* application = qobject_cast<QGuiApplication*>(
        QCoreApplication::instance());
    if (application == nullptr || QCoreApplication::closingDown() ||
        application->thread() == nullptr ||
        QAbstractEventDispatcher::instance(application->thread()) == nullptr) {
        return dispatchFailure;
    }
    if (QThread::currentThread() == application->thread()) {
        return work();
    }

    enum class DispatchPhase { queued, running, finished, abandoned };
    struct DispatchState {
        explicit DispatchState(ResultType fallback)
            : result(std::move(fallback)) {}

        std::mutex mutex;
        std::condition_variable changed;
        DispatchPhase phase{DispatchPhase::queued};
        ResultType result;
    };
    auto dispatch = std::make_shared<DispatchState>(std::move(dispatchFailure));
    const bool invoked = QMetaObject::invokeMethod(
        application,
        [dispatch, work = std::move(work)]() mutable {
            {
                std::lock_guard lock(dispatch->mutex);
                if (dispatch->phase != DispatchPhase::queued) return;
                dispatch->phase = DispatchPhase::running;
            }

            try {
                auto result = work();
                std::lock_guard lock(dispatch->mutex);
                dispatch->result = std::move(result);
            } catch (...) {
                // Qt event callbacks must not leak exceptions through the
                // event loop. The caller receives its dispatch failure.
            }

            {
                std::lock_guard lock(dispatch->mutex);
                dispatch->phase = DispatchPhase::finished;
            }
            dispatch->changed.notify_one();
        },
        Qt::QueuedConnection);
    if (!invoked) return std::move(dispatch->result);

    std::unique_lock lock(dispatch->mutex);
    const auto startedBy = std::chrono::steady_clock::now() +
                           kGuiDispatchStartTimeout;
    // The GUI thread may already be tearing down and waiting for this worker.
    // Bound only the time a callback may remain queued: after work starts, the
    // caller must keep its captured references alive until that work finishes.
    if (!dispatch->changed.wait_until(lock, startedBy, [&dispatch] {
            return dispatch->phase != DispatchPhase::queued;
        })) {
        dispatch->phase = DispatchPhase::abandoned;
        return std::move(dispatch->result);
    }
    if (dispatch->phase == DispatchPhase::running) {
        dispatch->changed.wait(lock, [&dispatch] {
            return dispatch->phase == DispatchPhase::finished;
        });
    }
    return std::move(dispatch->result);
}

std::vector<std::uint8_t> bytes(const QByteArray& value) {
    if (value.isEmpty()) return {};
    const auto* first = reinterpret_cast<const std::uint8_t*>(value.constData());
    return {first, first + value.size()};
}

QByteArray byteArray(const std::vector<std::uint8_t>& value) {
    if (value.empty()) return {};
    return {
        reinterpret_cast<const char*>(value.data()),
        static_cast<qsizetype>(value.size()),
    };
}

Result<QImage> semanticImage(const QMimeData& mimeData) {
    if (!mimeData.hasImage()) return Result<QImage>::success({});
    const auto imageData = mimeData.imageData();
    QImage image;
    if (imageData.canConvert<QImage>()) {
        image = qvariant_cast<QImage>(imageData);
    } else if (imageData.canConvert<QPixmap>()) {
        image = qvariant_cast<QPixmap>(imageData).toImage();
    }
    if (image.isNull()) {
        return Result<QImage>::failure(Status::failure(
            ErrorCode::protocol_error,
            "The clipboard offered image data that Qt could not decode."));
    }
    return Result<QImage>::success(std::move(image));
}

class QtClipboard final : public Clipboard {
public:
    Result<ClipboardSnapshot> snapshot() override {
        return onGuiThread(
            Result<ClipboardSnapshot>::failure(guiUnavailableStatus()),
            [this] { return snapshotOnGuiThread(); });
    }

    Status setText(const std::string& utf8Text) override {
        return onGuiThread(guiUnavailableStatus(), [this, &utf8Text] {
            return setTextOnGuiThread(utf8Text);
        });
    }

    Status restore(const ClipboardSnapshot& snapshot) override {
        return onGuiThread(guiUnavailableStatus(), [this, &snapshot] {
            return restoreOnGuiThread(snapshot);
        });
    }

private:
    Result<ClipboardSnapshot> snapshotOnGuiThread() {
        auto* clipboard = QGuiApplication::clipboard();
        if (clipboard == nullptr) {
            return Result<ClipboardSnapshot>::failure(guiUnavailableStatus());
        }

        ClipboardSnapshot snapshot;
        const auto* current = clipboard->mimeData(QClipboard::Clipboard);
        if (current == nullptr) {
            return Result<ClipboardSnapshot>::success(std::move(snapshot));
        }
        for (const auto& format : current->formats()) {
            if (format == QString::fromLatin1(kTransactionMime)) continue;
            snapshot.payloads[format.toUtf8().toStdString()] =
                bytes(current->data(format));
        }

        const auto image = semanticImage(*current);
        if (!image) {
            return Result<ClipboardSnapshot>::failure(image.status());
        }
        if (!image.value().isNull()) {
            QByteArray encoded;
            QBuffer destination(&encoded);
            if (!destination.open(QIODevice::WriteOnly) ||
                !image.value().save(&destination, "PNG")) {
                return Result<ClipboardSnapshot>::failure(Status::failure(
                    ErrorCode::io_error,
                    "LocalFlow could not preserve the clipboard image."));
            }
            snapshot.semanticImagePng = bytes(encoded);
        }
        return Result<ClipboardSnapshot>::success(std::move(snapshot));
    }

    Status setTextOnGuiThread(const std::string& utf8Text) {
        auto* clipboard = QGuiApplication::clipboard();
        if (clipboard == nullptr) return guiUnavailableStatus();

        transientToken_ = QUuid::createUuid().toRfc4122();
        auto replacement = std::make_unique<QMimeData>();
        replacement->setText(QString::fromUtf8(
            utf8Text.data(), static_cast<qsizetype>(utf8Text.size())));
        replacement->setData(
            QString::fromLatin1(kUtf8TextMime),
            QByteArray(utf8Text.data(), static_cast<qsizetype>(utf8Text.size())));
        replacement->setData(
            QString::fromLatin1(kTransactionMime), transientToken_);
        clipboard->setMimeData(replacement.release(), QClipboard::Clipboard);
        return Status::success();
    }

    Status restoreOnGuiThread(const ClipboardSnapshot& snapshot) {
        auto* clipboard = QGuiApplication::clipboard();
        if (clipboard == nullptr) return guiUnavailableStatus();
        const auto* current = clipboard->mimeData(QClipboard::Clipboard);

        // Losing the private marker means somebody copied something after
        // LocalFlow wrote its transient transcript. Their newer clipboard is
        // authoritative, even if the visible text happens to be identical.
        if (transientToken_.isEmpty() || current == nullptr ||
            current->data(QString::fromLatin1(kTransactionMime)) !=
                transientToken_) {
            transientToken_.clear();
            return Status::success();
        }

        if (snapshot.payloads.empty() && snapshot.semanticImagePng.empty()) {
            transientToken_.clear();
            clipboard->clear(QClipboard::Clipboard);
            return Status::success();
        }

        auto restored = std::make_unique<QMimeData>();
        for (const auto& [format, payload] : snapshot.payloads) {
            if (format == kTransactionMime) continue;
            restored->setData(
                QString::fromUtf8(format.data(), static_cast<qsizetype>(format.size())),
                byteArray(payload));
        }
        if (!snapshot.semanticImagePng.empty()) {
            const auto image = QImage::fromData(
                byteArray(snapshot.semanticImagePng), "PNG");
            if (image.isNull()) {
                return Status::failure(
                    ErrorCode::protocol_error,
                    "The saved clipboard image could not be decoded for restoration.");
            }
            restored->setImageData(image);
        }
        transientToken_.clear();
        clipboard->setMimeData(restored.release(), QClipboard::Clipboard);
        return Status::success();
    }

    QByteArray transientToken_;
};

}  // namespace

std::unique_ptr<Clipboard> makeQtClipboard() {
    return std::make_unique<QtClipboard>();
}

}  // namespace localflow::platform::linux::detail

#else

namespace localflow::platform::linux::detail {

std::unique_ptr<Clipboard> makeQtClipboard() {
    return {};
}

}  // namespace localflow::platform::linux::detail

#endif
