#include "LearnedTermModel.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QSaveFile>
#include <QStandardPaths>
#include <QStringList>
#include <QVariant>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <utility>

namespace {
using localflow::core::LearnedTerm;
using localflow::core::LearnedTerminologyBank;

std::uint32_t unsignedCount(const QJsonValue& value) {
    if (!value.isDouble()) return 1;
    const double decoded = value.toDouble();
    if (!std::isfinite(decoded)) return 1;
    return static_cast<std::uint32_t>(std::clamp(
        decoded,
        0.0,
        static_cast<double>(std::numeric_limits<std::uint32_t>::max())));
}

std::int64_t milliseconds(const QJsonValue& value) {
    if (!value.isDouble()) return 0;
    const double decoded = value.toDouble();
    if (!std::isfinite(decoded)) return 0;
    const double minimum = static_cast<double>(
        std::numeric_limits<std::int64_t>::min());
    const double maximum = static_cast<double>(
        std::numeric_limits<std::int64_t>::max());
    if (decoded <= minimum) return std::numeric_limits<std::int64_t>::min();
    if (decoded >= maximum) return std::numeric_limits<std::int64_t>::max();
    return static_cast<std::int64_t>(decoded);
}

QStringList aliases(const LearnedTerm& term) {
    QStringList result;
    result.reserve(static_cast<qsizetype>(term.aliases.size()));
    for (const auto& alias : term.aliases) {
        result.append(QString::fromStdString(alias));
    }
    return result;
}

QJsonObject encode(const LearnedTerm& term) {
    QJsonArray encodedAliases;
    for (const auto& alias : term.aliases) {
        encodedAliases.append(QString::fromStdString(alias));
    }
    QJsonObject object{
        {QStringLiteral("id"), QString::fromStdString(term.id)},
        {QStringLiteral("canonical"), QString::fromStdString(term.canonical)},
        {QStringLiteral("aliases"), encodedAliases},
        {QStringLiteral("useCount"), static_cast<double>(term.use_count)},
        {QStringLiteral("createdAtMs"), static_cast<double>(term.created_at_ms)},
        {QStringLiteral("lastUsedAtMs"), static_cast<double>(term.last_used_at_ms)},
    };
    if (term.source_app_id.has_value() && !term.source_app_id->empty()) {
        object.insert(
            QStringLiteral("sourceAppId"),
            QString::fromStdString(*term.source_app_id));
    }
    return object;
}

LearnedTerm decode(const QJsonObject& object) {
    LearnedTerm term;
    term.id = object.value(QStringLiteral("id")).toString().toStdString();
    term.canonical = object.value(QStringLiteral("canonical")).toString().toStdString();
    for (const auto& value : object.value(QStringLiteral("aliases")).toArray()) {
        if (value.isString()) term.aliases.push_back(value.toString().toStdString());
    }
    term.use_count = unsignedCount(object.value(QStringLiteral("useCount")));
    term.created_at_ms = milliseconds(object.value(QStringLiteral("createdAtMs")));
    term.last_used_at_ms = milliseconds(object.value(QStringLiteral("lastUsedAtMs")));
    const auto source = object.value(QStringLiteral("sourceAppId"));
    if (source.isString() && !source.toString().isEmpty()) {
        term.source_app_id = source.toString().toStdString();
    }
    return term;
}
}  // namespace

LearnedTermModel::LearnedTermModel(QObject* parent)
    : LearnedTermModel(defaultStoragePath(), parent) {}

LearnedTermModel::LearnedTermModel(QString storagePath, QObject* parent)
    : QAbstractListModel(parent), storagePath_(std::move(storagePath)) {
    (void)reload();
}

int LearnedTermModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : static_cast<int>(terms_.size());
}

QVariant LearnedTermModel::data(const QModelIndex& index, const int role) const {
    if (!index.isValid() || index.parent().isValid() || index.column() != 0
        || index.row() < 0 || index.row() >= rowCount()) {
        return {};
    }
    const auto& term = terms_[static_cast<std::size_t>(index.row())];
    switch (role) {
        case Qt::DisplayRole:
        case CanonicalRole:
            return QString::fromStdString(term.canonical);
        case IdRole:
            return QString::fromStdString(term.id);
        case AliasesRole:
            return aliases(term);
        case UseCountRole:
            return QVariant::fromValue(static_cast<qulonglong>(term.use_count));
        case CreatedAtMsRole:
            return QVariant::fromValue(static_cast<qlonglong>(term.created_at_ms));
        case LastUsedAtMsRole:
            return QVariant::fromValue(static_cast<qlonglong>(term.last_used_at_ms));
        case SourceAppIdRole:
            return term.source_app_id.has_value()
                ? QString::fromStdString(*term.source_app_id)
                : QString{};
        default:
            return {};
    }
}

QHash<int, QByteArray> LearnedTermModel::roleNames() const {
    return {
        {IdRole, "termId"},
        {CanonicalRole, "canonical"},
        {AliasesRole, "aliases"},
        {UseCountRole, "useCount"},
        {CreatedAtMsRole, "createdAtMs"},
        {LastUsedAtMsRole, "lastUsedAtMs"},
        {SourceAppIdRole, "sourceAppId"},
    };
}

int LearnedTermModel::count() const noexcept {
    return static_cast<int>(terms_.size());
}

bool LearnedTermModel::canUndo() const noexcept {
    return undo_.has_value();
}

QString LearnedTermModel::lastError() const {
    return lastError_;
}

QString LearnedTermModel::storagePath() const {
    return storagePath_;
}

const std::vector<LearnedTerm>& LearnedTermModel::terms() const noexcept {
    return terms_;
}

QString LearnedTermModel::defaultStoragePath() {
    return QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)
        + QStringLiteral("/learned-terminology.json");
}

bool LearnedTermModel::readTerms(
    std::vector<LearnedTerm>* terms,
    QString* error) const {
    if (terms == nullptr) {
        if (error != nullptr) *error = QStringLiteral("No learned-term destination was provided.");
        return false;
    }
    terms->clear();
    QFile file(storagePath_);
    if (!file.exists()) return true;
    if (!file.open(QIODevice::ReadOnly)) {
        if (error != nullptr) {
            *error = QStringLiteral("Couldn’t read learned terminology: %1")
                .arg(file.errorString());
        }
        return false;
    }

    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isArray()) {
        if (error != nullptr) {
            *error = parseError.error == QJsonParseError::NoError
                ? QStringLiteral("Learned terminology has an invalid file format.")
                : QStringLiteral("Couldn’t parse learned terminology: %1")
                    .arg(parseError.errorString());
        }
        return false;
    }

    std::vector<LearnedTerm> decoded;
    decoded.reserve(std::min<std::size_t>(
        static_cast<std::size_t>(document.array().size()),
        LearnedTerminologyBank::max_terms));
    for (const auto& value : document.array()) {
        if (value.isObject()) decoded.push_back(decode(value.toObject()));
    }
    *terms = LearnedTerminologyBank::sanitized(decoded);
    return true;
}

bool LearnedTermModel::persist(
    const std::vector<LearnedTerm>& terms,
    QString* error) const {
    const QFileInfo destination(storagePath_);
    if (!QDir().mkpath(destination.absolutePath())) {
        if (error != nullptr) {
            *error = QStringLiteral("Couldn’t create the learned terminology folder.");
        }
        return false;
    }

    QJsonArray array;
    for (const auto& term : terms) array.append(encode(term));
    const QByteArray payload = QJsonDocument(array).toJson(QJsonDocument::Compact);
    QSaveFile output(storagePath_);
    if (!output.open(QIODevice::WriteOnly)) {
        if (error != nullptr) {
            *error = QStringLiteral("Couldn’t save learned terminology: %1")
                .arg(output.errorString());
        }
        return false;
    }
    if (output.write(payload) != payload.size()) {
        if (error != nullptr) {
            *error = QStringLiteral("Couldn’t save learned terminology: %1")
                .arg(output.errorString());
        }
        output.cancelWriting();
        return false;
    }
    if (!output.commit()) {
        if (error != nullptr) {
            *error = QStringLiteral("Couldn’t finish saving learned terminology: %1")
                .arg(output.errorString());
        }
        return false;
    }
    return true;
}

void LearnedTermModel::replaceInMemory(
    std::vector<LearnedTerm> terms,
    const bool invalidateUndo) {
    const int previousCount = count();
    beginResetModel();
    terms_ = std::move(terms);
    endResetModel();
    if (invalidateUndo) setUndo(std::nullopt);
    if (previousCount != count()) emit countChanged();
    emit termsChanged();
}

bool LearnedTermModel::reload() {
    std::vector<LearnedTerm> loaded;
    QString error;
    if (!readTerms(&loaded, &error)) {
        setLastError(std::move(error));
        return false;
    }
    replaceInMemory(std::move(loaded), true);
    setLastError({});
    return true;
}

bool LearnedTermModel::replaceTermsAndSave(std::vector<LearnedTerm> terms) {
    terms = LearnedTerminologyBank::sanitized(terms);
    QString error;
    if (!persist(terms, &error)) {
        setLastError(std::move(error));
        return false;
    }
    replaceInMemory(std::move(terms), true);
    setLastError({});
    return true;
}

bool LearnedTermModel::removeTerm(const int row) {
    if (row < 0 || row >= count()) return false;
    auto next = terms_;
    const auto removed = next[static_cast<std::size_t>(row)];
    next.erase(next.begin() + row);
    QString error;
    if (!persist(next, &error)) {
        setLastError(std::move(error));
        return false;
    }

    beginRemoveRows({}, row, row);
    terms_.erase(terms_.begin() + row);
    endRemoveRows();
    setUndo(UndoRemoval{{removed}, row, false});
    setLastError({});
    emit countChanged();
    emit termsChanged();
    return true;
}

bool LearnedTermModel::clearTerms() {
    if (terms_.empty()) return true;
    QString error;
    if (!persist({}, &error)) {
        setLastError(std::move(error));
        return false;
    }

    UndoRemoval undo{terms_, 0, true};
    beginRemoveRows({}, 0, count() - 1);
    terms_.clear();
    endRemoveRows();
    setUndo(std::move(undo));
    setLastError({});
    emit countChanged();
    emit termsChanged();
    return true;
}

bool LearnedTermModel::undoLastRemoval() {
    if (!undo_.has_value()) return false;
    const auto undo = *undo_;
    auto restored = terms_;
    const int insertionRow = std::clamp(undo.row, 0, static_cast<int>(restored.size()));
    restored.insert(
        restored.begin() + insertionRow,
        undo.terms.begin(),
        undo.terms.end());
    restored = LearnedTerminologyBank::sanitized(restored);
    QString error;
    if (!persist(restored, &error)) {
        setLastError(std::move(error));
        return false;
    }

    if (undo.clearedAll) {
        replaceInMemory(std::move(restored), false);
    } else {
        beginInsertRows({}, insertionRow, insertionRow);
        terms_.insert(terms_.begin() + insertionRow, undo.terms.front());
        endInsertRows();
        emit countChanged();
        emit termsChanged();
    }
    setUndo(std::nullopt);
    setLastError({});
    return true;
}

void LearnedTermModel::setUndo(std::optional<UndoRemoval> undo) {
    const bool wasAvailable = canUndo();
    undo_ = std::move(undo);
    if (wasAvailable != canUndo()) emit canUndoChanged();
}

void LearnedTermModel::setLastError(QString error) {
    if (lastError_ == error) return;
    lastError_ = std::move(error);
    emit lastErrorChanged();
}
