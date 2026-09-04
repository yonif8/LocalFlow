#pragma once

#include <QObject>
#include <QString>
#include <QThread>

#include <functional>
#include <vector>

namespace localflow::app {

class SingleInstanceWorker;

enum class InstanceCommand {
    activate,
    background,
};

bool notifyExistingInstance(
    const QString& serverName,
    InstanceCommand command,
    int timeoutMs = 750);

bool notifyStartingInstance(
    const QString& serverName,
    InstanceCommand command,
    int totalTimeoutMs = 1500,
    int attemptTimeoutMs = 250);

class SingleInstanceServer final : public QObject {
public:
    using CommandHandler = std::function<void(InstanceCommand)>;

    explicit SingleInstanceServer(QObject* parent = nullptr);
    ~SingleInstanceServer() override;

    bool listen(const QString& serverName, QString* error = nullptr);
    void setCommandHandler(CommandHandler handler);

private:
    void dispatch(InstanceCommand command);

    friend class SingleInstanceWorker;

    QThread workerThread_;
    SingleInstanceWorker* worker_{nullptr};
    CommandHandler handler_;
    std::vector<InstanceCommand> pendingCommands_;
};

}  // namespace localflow::app
