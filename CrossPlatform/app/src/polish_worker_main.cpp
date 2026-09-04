#include "localflow/inference/S1MiniPolisher.hpp"

#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>

#include <iostream>

namespace {
void send(const QJsonObject& value) {
    std::cout << QJsonDocument(value).toJson(QJsonDocument::Compact).constData() << '\n' << std::flush;
}
}

int main(int argc, char* argv[]) {
    QCoreApplication application(argc, argv);
    const QStringList arguments = application.arguments();
    if (arguments.contains(QStringLiteral("--probe-runtime"))) {
        const auto probe = localflow::inference::S1MiniPolisher::probeRuntime();
        send({
            {QStringLiteral("ready"), bool(probe)},
            {QStringLiteral("error"), probe ? QString() : QString::fromStdString(probe.error())},
        });
        return probe ? 0 : 4;
    }
    const int modelIndex = arguments.indexOf(QStringLiteral("--model"));
    if (modelIndex < 0 || modelIndex + 1 >= arguments.size()) {
        send({{QStringLiteral("ready"), false}, {QStringLiteral("error"), QStringLiteral("Missing --model")}});
        return 2;
    }

    localflow::inference::S1MiniPolisher model({
        arguments.at(modelIndex + 1).toUtf8().toStdString(),
        4096,
        0,
    });
    const auto prepared = model.prepare();
    if (!prepared) {
        send({{QStringLiteral("ready"), false}, {QStringLiteral("error"), QString::fromStdString(prepared.error())}});
        return 3;
    }
    send({{QStringLiteral("ready"), true}});

    std::string line;
    while (std::getline(std::cin, line)) {
        const QJsonDocument document = QJsonDocument::fromJson(QByteArray::fromStdString(line));
        if (!document.isObject()) continue;
        const QJsonObject object = document.object();
        if (object.value(QStringLiteral("command")).toString() == QStringLiteral("quit")) break;
        const QString id = object.value(QStringLiteral("id")).toString();
        const QString text = object.value(QStringLiteral("text")).toString();
        if (text.isEmpty() || text.size() > 8000) {
            send({{QStringLiteral("id"), id}, {QStringLiteral("ok"), false},
                  {QStringLiteral("error"), QStringLiteral("Invalid polish input")}});
            continue;
        }
        localflow::inference::PolishRequest request;
        request.transcript = text.toStdString();
        request.tone = object.value(QStringLiteral("tone")).toString() == QStringLiteral("casual")
            ? localflow::inference::Tone::Casual : localflow::inference::Tone::Neutral;
        request.timeout = std::chrono::milliseconds(qBound(250, object.value(QStringLiteral("timeoutMs")).toInt(3000), 10000));
        request.maxOutputTokens = std::size_t(qBound(16, object.value(QStringLiteral("maxOutputTokens")).toInt(1024), 2048));
        const auto result = model.polish(request);
        if (!result) {
            send({{QStringLiteral("id"), id}, {QStringLiteral("ok"), false},
                  {QStringLiteral("error"), QString::fromStdString(result.error())}});
            continue;
        }
        send({
            {QStringLiteral("id"), id},
            {QStringLiteral("ok"), true},
            {QStringLiteral("text"), QString::fromStdString(result.value().text)},
            {QStringLiteral("elapsedMs"), double(result.value().elapsed.count())},
        });
    }
    return 0;
}
