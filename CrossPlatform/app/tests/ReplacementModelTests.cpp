#include "../src/ReplacementModel.hpp"

#include <QCoreApplication>
#include <QDebug>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QTemporaryDir>

#include <cstdlib>

namespace {

int failures = 0;

void expect(const bool condition, const char* message) {
    if (!condition) {
        qCritical("FAIL: %s", message);
        ++failures;
    }
}

QString valueAt(const ReplacementModel& model, int row, int role) {
    return model.data(model.index(row), role).toString();
}

}  // namespace

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("LocalFlowTests"));
    QCoreApplication::setApplicationName(QStringLiteral("ReplacementModelTests"));

    QTemporaryDir settingsDirectory;
    expect(settingsDirectory.isValid(), "creates isolated settings directory");
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(
        QSettings::IniFormat, QSettings::UserScope, settingsDirectory.path());
    QSettings().clear();

    {
        ReplacementModel model;
        expect(model.rowCount() == 0, "starts empty");
        expect(!model.addRule(QStringLiteral("local flow"), QString()),
               "rejects an empty written form");
        expect(!model.lastError().isEmpty(), "explains invalid replacement");
        expect(model.addRule(
                   QStringLiteral(" local flow "), QStringLiteral("  LocalFlow  ")),
               "adds a valid replacement");
        expect(model.lastError().isEmpty(), "clears an old error after success");
        expect(!model.addRule(
                   QStringLiteral("LOCAL FLOW"), QStringLiteral("Local Flow")),
               "rejects a case-insensitive duplicate");
        expect(model.rowCount() == 1, "duplicate does not mutate the model");
        expect(valueAt(model, 0, ReplacementModel::SpokenRole) ==
                   QStringLiteral("local flow"),
               "normalizes surrounding spoken whitespace");
        expect(valueAt(model, 0, ReplacementModel::WrittenRole) ==
                   QStringLiteral("LocalFlow"),
               "normalizes accidental surrounding written whitespace");
        expect(!model.addRule(QString(201, QLatin1Char('a')), QStringLiteral("value")),
               "rejects an overlong spoken form");
        expect(model.lastError().contains(QStringLiteral("200")),
               "explains the spoken form limit");
        expect(model.updateRule(
                   0, QStringLiteral("postgres sequel"),
                   QStringLiteral("  PostgreSQL  ")),
               "updates a valid replacement");
    }

    {
        ReplacementModel reloaded;
        expect(reloaded.rowCount() == 1, "persists the replacement");
        expect(valueAt(reloaded, 0, ReplacementModel::SpokenRole) ==
                   QStringLiteral("postgres sequel"),
               "persists the spoken form");
        expect(valueAt(reloaded, 0, ReplacementModel::WrittenRole) ==
                   QStringLiteral("PostgreSQL"),
               "persists the written form");
        reloaded.removeRule(0);
        expect(reloaded.rowCount() == 0, "removes a replacement");
    }

    ReplacementModel finalReload;
    expect(finalReload.rowCount() == 0, "persists removal");

    QSettings().setValue(
        QStringLiteral("dictionary/rulesJson"), QByteArrayLiteral("not-json"));
    ReplacementModel malformed;
    expect(malformed.rowCount() == 0, "rejects malformed persisted JSON");
    expect(!malformed.lastError().isEmpty(),
           "explains malformed persisted JSON");

    QJsonArray oversizedRules;
    for (int index = 0; index < 502; ++index) {
        oversizedRules.append(QJsonObject{
            {QStringLiteral("spoken"), QStringLiteral("spoken %1").arg(index)},
            {QStringLiteral("written"), QStringLiteral("Written %1").arg(index)},
        });
    }
    oversizedRules.append(QJsonObject{
        {QStringLiteral("spoken"), QStringLiteral("SPOKEN 0")},
        {QStringLiteral("written"), QStringLiteral("Duplicate")},
    });
    QSettings().setValue(
        QStringLiteral("dictionary/rulesJson"),
        QJsonDocument(oversizedRules).toJson(QJsonDocument::Compact));
    ReplacementModel capped;
    expect(capped.rowCount() == 500, "caps an oversized persisted dictionary");
    expect(!capped.lastError().isEmpty(),
           "reports ignored excess or duplicate persisted rules");

    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
