// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <QProcess>
#include <optional>

#include "tasks/Task.h"

class MinecraftInstance;
class HostedServer;

// Builds (or rebuilds) the dedicated server of an instance so it matches the instance's current pack version.
//
// Content comes from the best source available:
//  1. the pack's official server pack (CurseForge),
//  2. the pack's .mrpack, keeping only files marked for the server (Modrinth),
//  3. the instance's own files, dropping mods that declare themselves client-only.
// Then the instance's mod loader is installed for the server. A rebuild keeps worlds, server.properties and player
// lists, and moves everything it replaces into server/backups/rebuild-<time>.
class ServerBuildTask : public Task {
    Q_OBJECT
   public:
    ServerBuildTask(MinecraftInstance* instance, HostedServer* server);

    bool canAbort() const override { return true; }
    bool abort() override;

   protected:
    void executeTask() override;

   private:
    enum class Result { Ok, NotAvailable, Failed };

    Result gatherFromCurseForge();
    Result gatherFromModrinth();
    bool gatherFromInstance();
    bool installLoader();
    bool commit();

    bool runTask(Task::Ptr task);
    std::optional<QByteArray> fetch(const QUrl& url);
    bool download(const QUrl& url, const QString& path, const QString& sha1 = {}, const QString& sha512 = {});
    bool runJava(const QStringList& args);
    bool insideStaging(const QString& path) const;
    bool fail(const QString& reason);

    MinecraftInstance* m_instance;
    HostedServer* m_server;
    QString m_staging;
    QString m_scratch;
    QString m_java;
    QString m_source;
    QStringList m_excluded;
    QString m_error;

    Task::Ptr m_current;
    QProcess* m_process = nullptr;
    bool m_abort = false;
};
