#pragma once

#include <QAbstractListModel>
#include <QString>
#include <vector>

class ReplacementModel final : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(QString lastError READ lastError NOTIFY lastErrorChanged)

public:
    enum Role { SpokenRole = Qt::UserRole + 1, WrittenRole };

    explicit ReplacementModel(QObject* parent = nullptr);
    int rowCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;
    QString lastError() const { return lastError_; }

    Q_INVOKABLE bool addRule(const QString& spoken, const QString& written);
    Q_INVOKABLE bool updateRule(int row, const QString& spoken, const QString& written);
    Q_INVOKABLE void removeRule(int row);

signals:
    void rulesChanged();
    void lastErrorChanged();

private:
    struct Rule { QString spoken; QString written; };
    static bool valid(const QString& spoken, const QString& written);
    void load();
    bool save(const std::vector<Rule>& rules);
    void setError(QString error);

    std::vector<Rule> rules_;
    QString lastError_;
};
