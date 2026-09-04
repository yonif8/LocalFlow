#include "ReplacementModel.hpp"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QSet>
#include <QSettings>

#include <utility>

ReplacementModel::ReplacementModel(QObject* parent) : QAbstractListModel(parent) { load(); }

int ReplacementModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : int(rules_.size());
}

QVariant ReplacementModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= int(rules_.size())) return {};
    const auto& rule = rules_.at(std::size_t(index.row()));
    if (role == SpokenRole) return rule.spoken;
    if (role == WrittenRole) return rule.written;
    return {};
}

QHash<int, QByteArray> ReplacementModel::roleNames() const {
    return {{SpokenRole, "spoken"}, {WrittenRole, "written"}};
}

bool ReplacementModel::valid(const QString& spoken, const QString& written) {
    return !spoken.trimmed().isEmpty() && spoken.trimmed().size() <= 200
        && !written.trimmed().isEmpty() && written.size() <= 500;
}

bool ReplacementModel::addRule(const QString& spoken, const QString& written) {
    const QString key = spoken.trimmed();
    const QString value = written.trimmed();
    if (!valid(key, value)) {
        setError(key.isEmpty() || value.isEmpty()
            ? QStringLiteral("Enter both a spoken form and a written form.")
            : QStringLiteral("Spoken forms can be up to 200 characters and written forms up to 500 characters."));
        return false;
    }
    if (rules_.size() >= 500) {
        setError(QStringLiteral("The personal dictionary is limited to 500 replacements."));
        return false;
    }
    for (const auto& rule : rules_) {
        if (rule.spoken.compare(key, Qt::CaseInsensitive) == 0) {
            setError(QStringLiteral("That spoken form already has a replacement."));
            return false;
        }
    }
    auto next = rules_;
    next.push_back({key, value});
    if (!save(next)) return false;
    beginInsertRows({}, int(rules_.size()), int(rules_.size()));
    rules_.push_back({key, value});
    endInsertRows();
    emit rulesChanged();
    return true;
}

bool ReplacementModel::updateRule(int row, const QString& spoken, const QString& written) {
    const QString key = spoken.trimmed();
    const QString value = written.trimmed();
    if (row < 0 || row >= int(rules_.size())) return false;
    if (!valid(key, value)) {
        setError(key.isEmpty() || value.isEmpty()
            ? QStringLiteral("Enter both a spoken form and a written form.")
            : QStringLiteral("Spoken forms can be up to 200 characters and written forms up to 500 characters."));
        emit dataChanged(index(row), index(row), {SpokenRole, WrittenRole});
        return false;
    }
    for (int index = 0; index < int(rules_.size()); ++index) {
        if (index != row && rules_[std::size_t(index)].spoken.compare(key, Qt::CaseInsensitive) == 0) {
            setError(QStringLiteral("That spoken form already has a replacement."));
            emit dataChanged(this->index(row), this->index(row), {SpokenRole, WrittenRole});
            return false;
        }
    }
    auto next = rules_;
    next[std::size_t(row)] = {key, value};
    if (!save(next)) {
        emit dataChanged(index(row), index(row), {SpokenRole, WrittenRole});
        return false;
    }
    rules_[std::size_t(row)] = {key, value};
    emit dataChanged(index(row), index(row), {SpokenRole, WrittenRole});
    emit rulesChanged();
    return true;
}

void ReplacementModel::removeRule(int row) {
    if (row < 0 || row >= int(rules_.size())) return;
    auto next = rules_;
    next.erase(next.begin() + row);
    if (!save(next)) return;
    beginRemoveRows({}, row, row);
    rules_.erase(rules_.begin() + row);
    endRemoveRows();
    emit rulesChanged();
}

void ReplacementModel::load() {
    const QByteArray encoded = QSettings().value(
        QStringLiteral("dictionary/rulesJson"), QByteArrayLiteral("[]")).toByteArray();
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(encoded, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isArray()) {
        setError(QStringLiteral("The saved personal dictionary could not be read. No invalid rules were loaded."));
        return;
    }
    QSet<QString> spokenForms;
    bool ignoredInvalidRule = false;
    for (const auto& value : document.array()) {
        const auto object = value.toObject();
        const QString spoken = object.value(QStringLiteral("spoken")).toString().trimmed();
        const QString written = object.value(QStringLiteral("written")).toString().trimmed();
        const QString folded = spoken.toCaseFolded();
        if (valid(spoken, written) && !spokenForms.contains(folded)
            && rules_.size() < 500) {
            rules_.push_back({spoken, written});
            spokenForms.insert(folded);
        } else {
            ignoredInvalidRule = true;
        }
    }
    if (ignoredInvalidRule) {
        setError(QStringLiteral("Some invalid or duplicate saved replacements were ignored."));
    }
}

bool ReplacementModel::save(const std::vector<Rule>& rules) {
    QJsonArray values;
    for (const auto& rule : rules) {
        values.append(QJsonObject{
            {QStringLiteral("spoken"), rule.spoken},
            {QStringLiteral("written"), rule.written},
        });
    }
    const QString storageKey = QStringLiteral("dictionary/rulesJson");
    QSettings settings;
    const bool hadPrevious = settings.contains(storageKey);
    const QVariant previous = settings.value(storageKey);
    settings.setValue(storageKey, QJsonDocument(values).toJson(QJsonDocument::Compact));
    settings.sync();
    if (settings.status() != QSettings::NoError) {
        QSettings rollback;
        if (hadPrevious) rollback.setValue(storageKey, previous);
        else rollback.remove(storageKey);
        rollback.sync();
        setError(QStringLiteral("LocalFlow could not save the personal dictionary. Check that your account can write app settings, then try again."));
        return false;
    }
    setError({});
    return true;
}

void ReplacementModel::setError(QString error) {
    if (lastError_ == error) return;
    lastError_ = std::move(error);
    emit lastErrorChanged();
}
