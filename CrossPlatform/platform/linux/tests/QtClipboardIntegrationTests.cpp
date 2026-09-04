#include "localflow/linux/LinuxPlatform.hpp"

#include <QBuffer>
#include <QByteArray>
#include <QClipboard>
#include <QCoreApplication>
#include <QEventLoop>
#include <QGuiApplication>
#include <QImage>
#include <QMimeData>
#include <QPixmap>
#include <QUrl>
#include <QVariant>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <future>
#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using namespace localflow::platform::linux;

namespace {

int failures = 0;

#define EXPECT_TRUE(value) do { \
    if (!(value)) { \
        std::cerr << __FILE__ << ':' << __LINE__ << " expected true: " #value "\n"; \
        ++failures; \
    } \
} while (false)

#define EXPECT_EQ(left, right) do { \
    const auto actualLeft = (left); \
    const auto actualRight = (right); \
    if (!(actualLeft == actualRight)) { \
        std::cerr << __FILE__ << ':' << __LINE__ << " expected equality: " #left " == " #right "\n"; \
        ++failures; \
    } \
} while (false)

constexpr auto kTransactionMime =
    "application/x-localflow-clipboard-transaction";

CapabilityReport clipboardReport() {
    CapabilityReport report;
    report.session.type = SessionType::x11;
    report.capabilities.push_back({
        Feature::clipboard_paste,
        Availability::available,
        "Qt QClipboard",
        {},
        {},
    });
    return report;
}

template <typename Work>
auto onWorker(Work work) {
    auto future = std::async(std::launch::async, std::move(work));
    while (future.wait_for(std::chrono::milliseconds(1)) !=
           std::future_status::ready) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    }
    return future.get();
}

QByteArray png(const QImage& image) {
    QByteArray encoded;
    QBuffer destination(&encoded);
    if (!destination.open(QIODevice::WriteOnly) ||
        !image.save(&destination, "PNG")) {
        return {};
    }
    return encoded;
}

QImage imageFromMime(const QMimeData& mime) {
    const auto value = mime.imageData();
    if (value.canConvert<QImage>()) return qvariant_cast<QImage>(value);
    if (value.canConvert<QPixmap>()) return qvariant_cast<QPixmap>(value).toImage();
    return {};
}

void seedRichClipboard(QClipboard& clipboard, const QImage& image) {
    auto data = std::make_unique<QMimeData>();
    data->setText(QStringLiteral("original text"));
    data->setHtml(QStringLiteral("<p><b>original</b> text</p>"));
    data->setUrls({
        QUrl(QStringLiteral("https://localflow.example/docs")),
        QUrl::fromLocalFile(QStringLiteral("/tmp/LocalFlow terminology.txt")),
    });
    data->setData(QStringLiteral("image/png"), png(image));
    data->setImageData(image);
    data->setData(
        QStringLiteral("application/x-localflow-test-binary"),
        QByteArray::fromHex("00ff1041424300"));
    data->setData(QStringLiteral("application/x-localflow-test-empty"), {});
    clipboard.setMimeData(data.release(), QClipboard::Clipboard);
}

void testRichRoundTrip(Clipboard& backend, QClipboard& clipboard) {
    QImage image(3, 2, QImage::Format_ARGB32);
    image.fill(qRgba(17, 91, 203, 255));
    seedRichClipboard(clipboard, image);

    const auto snapshot = onWorker([&] { return backend.snapshot(); });
    EXPECT_TRUE(snapshot.ok());
    if (!snapshot) return;
    const auto& saved = snapshot.value();
    EXPECT_TRUE(saved.payloads.count("text/plain") != 0);
    EXPECT_TRUE(saved.payloads.count("text/html") != 0);
    EXPECT_TRUE(saved.payloads.count("text/uri-list") != 0);
    EXPECT_TRUE(saved.payloads.count("image/png") != 0);
    EXPECT_TRUE(saved.payloads.count("application/x-localflow-test-binary") != 0);
    EXPECT_TRUE(saved.payloads.count("application/x-localflow-test-empty") != 0);
    EXPECT_TRUE(saved.payloads.at("application/x-localflow-test-empty").empty());
    EXPECT_TRUE(!saved.semanticImagePng.empty());

    EXPECT_TRUE(onWorker([&] { return backend.setText("temporary transcript"); }).ok());
    const auto* transient = clipboard.mimeData(QClipboard::Clipboard);
    EXPECT_TRUE(transient != nullptr);
    if (transient != nullptr) {
        EXPECT_EQ(transient->text(), QStringLiteral("temporary transcript"));
        EXPECT_TRUE(!transient->data(QString::fromLatin1(kTransactionMime)).isEmpty());
    }

    EXPECT_TRUE(onWorker([&] { return backend.restore(saved); }).ok());
    const auto* restored = clipboard.mimeData(QClipboard::Clipboard);
    EXPECT_TRUE(restored != nullptr);
    if (restored == nullptr) return;
    for (const auto& [format, payload] : saved.payloads) {
        const auto actual = restored->data(QString::fromStdString(format));
        const auto expected = QByteArray(
            reinterpret_cast<const char*>(payload.data()),
            static_cast<qsizetype>(payload.size()));
        EXPECT_EQ(actual, expected);
    }
    EXPECT_TRUE(restored->data(QString::fromLatin1(kTransactionMime)).isEmpty());
    EXPECT_TRUE(restored->hasImage());
    const auto restoredImage = imageFromMime(*restored);
    EXPECT_EQ(restoredImage.size(), image.size());
    EXPECT_EQ(restoredImage.pixelColor(1, 1), image.pixelColor(1, 1));
}

void testUserCopyWinsRace(Clipboard& backend, QClipboard& clipboard) {
    QImage original(1, 1, QImage::Format_ARGB32);
    original.fill(Qt::red);
    seedRichClipboard(clipboard, original);
    const auto snapshot = onWorker([&] { return backend.snapshot(); });
    EXPECT_TRUE(snapshot.ok());
    if (!snapshot) return;
    EXPECT_TRUE(onWorker([&] { return backend.setText("temporary transcript"); }).ok());

    auto userCopy = std::make_unique<QMimeData>();
    // Deliberately use the same visible text as LocalFlow's transient value.
    // Text comparison cannot detect this race; the private marker must.
    userCopy->setText(QStringLiteral("temporary transcript"));
    userCopy->setHtml(QStringLiteral("<i>the user's newer rich copy</i>"));
    userCopy->setData(
        QStringLiteral("application/x-user-copy"), QByteArray("keep-me"));
    clipboard.setMimeData(userCopy.release(), QClipboard::Clipboard);

    EXPECT_TRUE(onWorker([&] { return backend.restore(snapshot.value()); }).ok());
    const auto* current = clipboard.mimeData(QClipboard::Clipboard);
    EXPECT_TRUE(current != nullptr);
    if (current != nullptr) {
        EXPECT_EQ(current->text(), QStringLiteral("temporary transcript"));
        EXPECT_EQ(
            current->html(),
            QStringLiteral("<i>the user's newer rich copy</i>"));
        EXPECT_EQ(
            current->data(QStringLiteral("application/x-user-copy")),
            QByteArray("keep-me"));
        EXPECT_TRUE(current->data(QString::fromLatin1(kTransactionMime)).isEmpty());
    }
}

void testEmptyClipboardRoundTrip(Clipboard& backend, QClipboard& clipboard) {
    clipboard.clear(QClipboard::Clipboard);
    const auto snapshot = onWorker([&] { return backend.snapshot(); });
    EXPECT_TRUE(snapshot.ok());
    if (!snapshot) return;
    EXPECT_TRUE(snapshot.value().payloads.empty());
    EXPECT_TRUE(snapshot.value().semanticImagePng.empty());

    EXPECT_TRUE(onWorker([&] { return backend.setText("temporary transcript"); }).ok());
    EXPECT_TRUE(onWorker([&] { return backend.restore(snapshot.value()); }).ok());
    const auto* current = clipboard.mimeData(QClipboard::Clipboard);
    EXPECT_TRUE(current == nullptr || current->formats().empty());
}

void testBlockedGuiDispatchIsAbandoned(QClipboard& clipboard) {
    clipboard.setText(QStringLiteral("clipboard sentinel"));
    auto backend = makeSystemClipboard(clipboardReport());
    std::promise<void> workerEntered;
    auto entered = workerEntered.get_future();
    auto result = std::async(std::launch::async, [
        backend = backend.get(),
        workerEntered = std::move(workerEntered)
    ]() mutable {
        workerEntered.set_value();
        return backend->setText("must never appear");
    });
    entered.wait();

    // Deliberately do not process GUI events. This reproduces application
    // teardown, where a worker used to wait forever on a blocking metacall
    // while the GUI thread waited for that worker.
    const auto readiness = result.wait_for(std::chrono::seconds(3));
    EXPECT_EQ(readiness, std::future_status::ready);
    if (readiness != std::future_status::ready) {
        // Release an unfixed implementation so this regression test reports a
        // failure instead of hanging in std::future's destructor.
        while (result.wait_for(std::chrono::milliseconds(1)) !=
               std::future_status::ready) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        }
    }
    const auto status = result.get();
    EXPECT_TRUE(!status.ok());
    EXPECT_EQ(status.code, ErrorCode::service_unavailable);

    // The abandoned metacall may still be in Qt's queue. Destroying the
    // backend first catches dangling captures; processing it must be a no-op.
    backend.reset();
    QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
    EXPECT_EQ(clipboard.text(), QStringLiteral("clipboard sentinel"));
}

}  // namespace

int main(int argc, char** argv) {
    QGuiApplication application(argc, argv);
    auto* clipboard = QGuiApplication::clipboard();
    if (clipboard == nullptr) {
        std::cerr << "Qt did not provide a clipboard.\n";
        return EXIT_FAILURE;
    }
    auto backend = makeSystemClipboard(clipboardReport());
    testRichRoundTrip(*backend, *clipboard);
    testUserCopyWinsRace(*backend, *clipboard);
    testEmptyClipboardRoundTrip(*backend, *clipboard);
    testBlockedGuiDispatchIsAbandoned(*clipboard);

    if (failures == 0) {
        std::cout << "All Qt clipboard integration tests passed.\n";
        return EXIT_SUCCESS;
    }
    std::cerr << failures << " Qt clipboard assertion(s) failed.\n";
    return EXIT_FAILURE;
}
