#pragma once

#include "localflow/core/learned_terminology.hpp"

#include <QAbstractListModel>
#include <QByteArray>
#include <QHash>
#include <QString>
#include <QVariant>

#include <optional>
#include <vector>

/// QML-facing, locally persisted view of LocalFlow's learned terminology.
///
/// The on-disk representation intentionally matches the existing
/// learned-terminology.json array written by AppController. Mutations are
/// persisted atomically before the model changes, so a failed write cannot make
/// the UI and the pipeline disagree about what will survive a restart.
class LearnedTermModel final : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY countChanged)
    Q_PROPERTY(bool canUndo READ canUndo NOTIFY canUndoChanged)
    Q_PROPERTY(QString lastError READ lastError NOTIFY lastErrorChanged)

public:
    enum Role {
        IdRole = Qt::UserRole + 1,
        CanonicalRole,
        AliasesRole,
        UseCountRole,
        CreatedAtMsRole,
        LastUsedAtMsRole,
        SourceAppIdRole,
    };
    Q_ENUM(Role)

    explicit LearnedTermModel(QObject* parent = nullptr);
    explicit LearnedTermModel(QString storagePath, QObject* parent = nullptr);

    [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override;
    [[nodiscard]] QHash<int, QByteArray> roleNames() const override;

    [[nodiscard]] int count() const noexcept;
    [[nodiscard]] bool canUndo() const noexcept;
    [[nodiscard]] QString lastError() const;
    [[nodiscard]] QString storagePath() const;
    [[nodiscard]] const std::vector<localflow::core::LearnedTerm>& terms() const noexcept;

    /// Re-read the shared persistence file. Existing in-memory terms are kept
    /// when the file is unreadable or malformed.
    Q_INVOKABLE bool reload();
    Q_INVOKABLE bool removeTerm(int row);
    Q_INVOKABLE bool clearTerms();
    Q_INVOKABLE bool undoLastRemoval();

    /// Synchronize terms learned by the pipeline and persist them using the same
    /// schema. Call this on the model's owning thread after a pipeline finishes.
    bool replaceTermsAndSave(
        std::vector<localflow::core::LearnedTerm> terms);

    [[nodiscard]] static QString defaultStoragePath();

signals:
    void countChanged();
    void canUndoChanged();
    void lastErrorChanged();
    void termsChanged();

private:
    struct UndoRemoval {
        std::vector<localflow::core::LearnedTerm> terms;
        int row{0};
        bool clearedAll{false};
    };

    [[nodiscard]] bool readTerms(
        std::vector<localflow::core::LearnedTerm>* terms,
        QString* error) const;
    [[nodiscard]] bool persist(
        const std::vector<localflow::core::LearnedTerm>& terms,
        QString* error) const;
    void replaceInMemory(
        std::vector<localflow::core::LearnedTerm> terms,
        bool invalidateUndo);
    void setUndo(std::optional<UndoRemoval> undo);
    void setLastError(QString error);

    QString storagePath_;
    std::vector<localflow::core::LearnedTerm> terms_;
    std::optional<UndoRemoval> undo_;
    QString lastError_;
};
