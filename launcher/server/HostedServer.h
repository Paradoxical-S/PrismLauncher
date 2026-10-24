// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <QObject>
#include <QProcess>
#include <QStringList>
#include <QTimer>
#include <memory>

#include "settings/SettingsObject.h"

class MinecraftInstance;

// A dedicated server that belongs to one instance. It lives in <instance>/server, with its launcher settings in
// <instance>/hostserver.cfg so a rebuild never touches them. One object per instance, kept for the whole session,
// so the server keeps running while the instance window is closed.
class HostedServer : public QObject {
    Q_OBJECT
   public:
    enum class State { Missing, Stopped, Starting, Running, Stopping };

    static HostedServer* get(MinecraftInstance* instance);
    // Stops every running server and waits for it to save, for use when the launcher quits.
    static void stopAll();

    QString serverDir() const { return m_serverDir; }
    QString propertiesPath() const;
    SettingsObject* settings() const { return m_settings.get(); }

    bool isBuilt() const;
    State state() const { return m_state; }
    bool isActive() const { return m_state == State::Starting || m_state == State::Running || m_state == State::Stopping; }
    QStringList consoleHistory() const { return m_console; }

    int port() const;
    QString levelName() const;

    // The full command line used to start the server, after the wrapper command. Empty on error.
    QStringList launchCommand(QString* error) const;

    bool start(QString* error);
    void stop();
    void kill();
    bool sendCommand(const QString& command);

    // Player lists. When the server runs these go through console commands, otherwise straight to the json files.
    QStringList playerList(const QString& file) const;
    void removePlayer(const QString& file, const QString& name);
    // UUID is looked up by the caller (online mode) or derived from the name (offline mode).
    void addPlayer(const QString& file, const QString& name, const QString& uuid);
    static QString offlineUuid(const QString& name);

    void appendConsole(const QString& line);
    // Re-checks whether the server is built, after a build changed the folder.
    void refresh();

   signals:
    void stateChanged(HostedServer::State state);
    void consoleLine(const QString& line);
    void ready();  // the server finished loading ("Done (…)!")

   private:
    HostedServer(MinecraftInstance* instance);
    void setState(State state);
    QString expandVariables(QString text) const;
    void onOutput();
    void onFinished(int exitCode, QProcess::ExitStatus status);

    QString m_instanceId;
    QString m_instanceName;
    QString m_instanceJava;
    QString m_serverDir;
    std::unique_ptr<SettingsObject> m_settings;

    QProcess* m_process = nullptr;
    QByteArray m_partialLine;
    QTimer m_stopTimer;
    State m_state = State::Missing;
    QStringList m_console;
};
