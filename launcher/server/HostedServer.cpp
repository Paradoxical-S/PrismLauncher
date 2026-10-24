// SPDX-License-Identifier: GPL-3.0-only
#include "HostedServer.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <QUuid>
#include <cmath>

#include "FileSystem.h"
#include "minecraft/MinecraftInstance.h"
#include "server/ServerProperties.h"
#include "settings/INISettingsObject.h"

namespace {
QHash<QString, HostedServer*>& registry()
{
    static QHash<QString, HostedServer*> servers;
    return servers;
}

constexpr int kConsoleLimit = 5000;
constexpr int kStopTimeoutMs = 90 * 1000;

const QString kDefaultJvmArgs =
    "-XX:+UseG1GC -XX:+ParallelRefProcEnabled -XX:MaxGCPauseMillis=200 -XX:+UnlockExperimentalVMOptions -XX:+DisableExplicitGC "
    "-XX:G1NewSizePercent=30 -XX:G1MaxNewSizePercent=40 -XX:G1HeapRegionSize=8M -XX:G1ReservePercent=20 "
    "-XX:InitiatingHeapOccupancyPercent=15";

QString defaultWrapper()
{
    // On machines that share resources through the `res` broker, lease the server's cores and RAM by default.
    if (!QStandardPaths::findExecutable("res").isEmpty()) {
        return R"(res run --label "Minecraft server: $INST_NAME" --cpus 4 --mem $SERVER_LEASE_GB --)";
    }
    return {};
}
}  // namespace

HostedServer* HostedServer::get(MinecraftInstance* instance)
{
    auto& servers = registry();
    auto it = servers.find(instance->id());
    if (it != servers.end()) {
        (*it)->m_instanceName = instance->name();
        (*it)->m_instanceJava = instance->settings()->get("JavaPath").toString();
        return *it;
    }
    if (servers.isEmpty()) {
        QObject::connect(qApp, &QCoreApplication::aboutToQuit, [] { HostedServer::stopAll(); });
    }
    auto server = new HostedServer(instance);
    servers.insert(instance->id(), server);
    return server;
}

void HostedServer::stopAll()
{
    for (auto server : registry()) {
        if (!server->m_process) {
            continue;
        }
        server->stop();
        if (!server->m_process->waitForFinished(kStopTimeoutMs)) {
            server->kill();
        }
    }
}

HostedServer::HostedServer(MinecraftInstance* instance) : QObject(qApp)
{
    m_instanceId = instance->id();
    m_instanceName = instance->name();
    m_instanceJava = instance->settings()->get("JavaPath").toString();
    m_serverDir = FS::PathCombine(instance->instanceRoot(), "server");

    m_settings = std::make_unique<INISettingsObject>(FS::PathCombine(instance->instanceRoot(), "hostserver.cfg"));
    m_settings->registerSetting("MinMemAlloc", 4096);
    m_settings->registerSetting("MaxMemAlloc", 8192);
    m_settings->registerSetting("JavaPath", "");
    m_settings->registerSetting("JvmArgs", kDefaultJvmArgs);
    m_settings->registerSetting("WrapperCommand", defaultWrapper());
    m_settings->registerSetting("EulaAccepted", false);
    // What the server was last built from, to tell when the instance moved to a newer pack version.
    m_settings->registerSetting("BuiltFromVersionId", "");
    m_settings->registerSetting("BuiltFromVersionName", "");
    m_settings->registerSetting("BuiltFromSource", "");
    m_settings->registerSetting("ExcludedMods", QStringList());

    m_stopTimer.setSingleShot(true);
    connect(&m_stopTimer, &QTimer::timeout, this, [this] {
        appendConsole(tr("[Prism] The server did not stop in time, killing it."));
        kill();
    });

    m_state = isBuilt() ? State::Stopped : State::Missing;
}

QString HostedServer::propertiesPath() const
{
    return FS::PathCombine(m_serverDir, "server.properties");
}

bool HostedServer::isBuilt() const
{
    QString error;
    return !launchCommand(&error).isEmpty();
}

int HostedServer::port() const
{
    ServerProperties props;
    props.load(propertiesPath());
    return props.value("server-port", "25565").toInt();
}

QString HostedServer::levelName() const
{
    ServerProperties props;
    props.load(propertiesPath());
    return props.value("level-name", "world");
}

QString HostedServer::expandVariables(QString text) const
{
    auto maxMem = m_settings->get("MaxMemAlloc").toInt();
    // Heap plus room for the JVM itself, metaspace and native buffers.
    auto leaseGb = static_cast<int>(std::ceil(maxMem / 1024.0)) + 2;
    text.replace("$INST_NAME", m_instanceName);
    text.replace("$INST_ID", m_instanceId);
    text.replace("$SERVER_DIR", m_serverDir);
    text.replace("$SERVER_LEASE_GB", QString("%1G").arg(leaseGb));
    text.replace("$SERVER_MAX_MEM_MB", QString::number(maxMem));
    return text;
}

QStringList HostedServer::launchCommand(QString* error) const
{
    QDir dir(m_serverDir);
    if (!dir.exists()) {
        *error = tr("The server has not been created yet.");
        return {};
    }

    // Work out how this loader wants to be started.
    QStringList target;
#ifdef Q_OS_WIN
    const QString argsFile = "win_args.txt";
#else
    const QString argsFile = "unix_args.txt";
#endif
    QDirIterator libs(dir.filePath("libraries"), { argsFile }, QDir::Files, QDirIterator::Subdirectories);
    if (libs.hasNext()) {
        // Modern Forge and NeoForge put their whole classpath in an args file.
        target << "@" + dir.relativeFilePath(libs.next());
    } else {
        for (const auto& jar : { "fabric-server-launch.jar", "quilt-server-launch.jar" }) {
            if (dir.exists(jar)) {
                target << "-jar" << jar;
                break;
            }
        }
        if (target.isEmpty()) {
            // Older Forge ships a single jar named after the version.
            auto forgeJars = dir.entryList({ "forge-*.jar", "minecraftforge-*.jar" }, QDir::Files);
            forgeJars.removeIf([](const QString& name) { return name.contains("installer"); });
            if (!forgeJars.isEmpty()) {
                target << "-jar" << forgeJars.first();
            } else if (dir.exists("server.jar")) {
                target << "-jar" << "server.jar";
            }
        }
    }
    if (target.isEmpty()) {
        *error = tr("No server jar was found in %1.").arg(m_serverDir);
        return {};
    }

    auto java = m_settings->get("JavaPath").toString();
    if (java.isEmpty()) {
        java = m_instanceJava;
    }
    if (java.isEmpty()) {
        java = "java";
    }

    QStringList cmd;
    auto wrapper = expandVariables(m_settings->get("WrapperCommand").toString()).trimmed();
    if (!wrapper.isEmpty()) {
        cmd << QProcess::splitCommand(wrapper);
    }
    cmd << java;
    cmd << QString("-Xms%1M").arg(m_settings->get("MinMemAlloc").toInt());
    cmd << QString("-Xmx%1M").arg(m_settings->get("MaxMemAlloc").toInt());
    cmd << QProcess::splitCommand(expandVariables(m_settings->get("JvmArgs").toString()));
    cmd << target << "nogui";
    return cmd;
}

bool HostedServer::start(QString* error)
{
    if (m_process) {
        *error = tr("The server is already running.");
        return false;
    }
    if (!m_settings->get("EulaAccepted").toBool()) {
        *error = tr("You need to accept the Minecraft EULA in the Launch tab before the server can start.");
        return false;
    }
    auto cmd = launchCommand(error);
    if (cmd.isEmpty()) {
        return false;
    }
    try {
        FS::write(FS::PathCombine(m_serverDir, "eula.txt"), "eula=true\n");
    } catch (const FS::FileSystemException& e) {
        *error = tr("Could not write eula.txt: %1").arg(e.cause());
        return false;
    }

    m_process = new QProcess(this);
    m_process->setWorkingDirectory(m_serverDir);
    m_process->setProcessChannelMode(QProcess::MergedChannels);
    m_process->setProgram(cmd.takeFirst());
    m_process->setArguments(cmd);
    connect(m_process, &QProcess::readyReadStandardOutput, this, &HostedServer::onOutput);
    connect(m_process, &QProcess::finished, this, &HostedServer::onFinished);
    connect(m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError err) {
        if (err == QProcess::FailedToStart) {
            appendConsole(tr("[Prism] Failed to start %1: %2").arg(m_process->program(), m_process->errorString()));
            onFinished(-1, QProcess::CrashExit);
        }
    });

    appendConsole(tr("[Prism] Starting: %1 %2").arg(m_process->program(), m_process->arguments().join(' ')));
    setState(State::Starting);
    m_process->start();
    return true;
}

void HostedServer::stop()
{
    if (!m_process) {
        return;
    }
    if (m_state == State::Starting) {
        // The console is not reading input yet while mods load, so ask the JVM to shut down instead.
        appendConsole(tr("[Prism] Stopping a server that is still loading."));
        m_process->terminate();
    } else {
        sendCommand("stop");
    }
    setState(State::Stopping);
    m_stopTimer.start(kStopTimeoutMs);
}

void HostedServer::kill()
{
    if (m_process) {
        m_process->kill();
    }
}

bool HostedServer::sendCommand(const QString& command)
{
    if (!m_process || m_process->state() != QProcess::Running) {
        return false;
    }
    appendConsole("> " + command);
    m_process->write(command.toUtf8() + '\n');
    return true;
}

void HostedServer::onOutput()
{
    m_partialLine += m_process->readAllStandardOutput();
    int newline;
    while ((newline = m_partialLine.indexOf('\n')) >= 0) {
        auto line = QString::fromUtf8(m_partialLine.left(newline)).trimmed();
        m_partialLine.remove(0, newline + 1);
        appendConsole(line);
        static const QRegularExpression doneLine(R"(Done \([0-9.,]+s\)!)");
        if (m_state == State::Starting && doneLine.match(line).hasMatch()) {
            setState(State::Running);
            emit ready();
        }
    }
}

void HostedServer::onFinished(int exitCode, QProcess::ExitStatus status)
{
    if (!m_process) {
        return;
    }
    if (!m_partialLine.isEmpty()) {
        appendConsole(QString::fromUtf8(m_partialLine));
        m_partialLine.clear();
    }
    m_stopTimer.stop();
    if (status == QProcess::CrashExit) {
        appendConsole(tr("[Prism] The server process crashed."));
    } else {
        appendConsole(tr("[Prism] The server exited with code %1.").arg(exitCode));
    }
    m_process->deleteLater();
    m_process = nullptr;
    setState(isBuilt() ? State::Stopped : State::Missing);
}

void HostedServer::setState(State state)
{
    if (state == State::Stopped && !isBuilt()) {
        state = State::Missing;
    }
    if (m_state == state) {
        return;
    }
    m_state = state;
    emit stateChanged(state);
}

void HostedServer::refresh()
{
    if (!isActive()) {
        setState(isBuilt() ? State::Stopped : State::Missing);
    }
}

void HostedServer::appendConsole(const QString& line)
{
    m_console.append(line);
    if (m_console.size() > kConsoleLimit) {
        m_console.erase(m_console.begin(), m_console.begin() + (m_console.size() - kConsoleLimit));
    }
    emit consoleLine(line);
}

QStringList HostedServer::playerList(const QString& file) const
{
    QStringList names;
    QFile f(FS::PathCombine(m_serverDir, file));
    if (!f.open(QIODevice::ReadOnly)) {
        return names;
    }
    for (auto entry : QJsonDocument::fromJson(f.readAll()).array()) {
        names.append(entry.toObject().value("name").toString());
    }
    return names;
}

static bool writePlayerFile(const QString& path, const QJsonArray& entries)
{
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly)) {
        return false;
    }
    f.write(QJsonDocument(entries).toJson());
    return f.commit();
}

void HostedServer::removePlayer(const QString& file, const QString& name)
{
    if (m_state == State::Running) {
        sendCommand(file == "ops.json" ? "deop " + name : "whitelist remove " + name);
        return;
    }
    auto path = FS::PathCombine(m_serverDir, file);
    QFile f(path);
    QJsonArray kept;
    if (f.open(QIODevice::ReadOnly)) {
        for (auto entry : QJsonDocument::fromJson(f.readAll()).array()) {
            if (entry.toObject().value("name").toString().compare(name, Qt::CaseInsensitive) != 0) {
                kept.append(entry);
            }
        }
        f.close();
    }
    writePlayerFile(path, kept);
}

void HostedServer::addPlayer(const QString& file, const QString& name, const QString& uuid)
{
    if (m_state == State::Running) {
        sendCommand(file == "ops.json" ? "op " + name : "whitelist add " + name);
        return;
    }
    auto path = FS::PathCombine(m_serverDir, file);
    QFile f(path);
    QJsonArray entries;
    if (f.open(QIODevice::ReadOnly)) {
        entries = QJsonDocument::fromJson(f.readAll()).array();
        f.close();
    }
    for (auto entry : entries) {
        if (entry.toObject().value("name").toString().compare(name, Qt::CaseInsensitive) == 0) {
            return;
        }
    }
    QJsonObject entry{ { "uuid", uuid }, { "name", name } };
    if (file == "ops.json") {
        entry["level"] = 4;
        entry["bypassesPlayerLimit"] = false;
    }
    entries.append(entry);
    writePlayerFile(path, entries);
}

// Same as Java's UUID.nameUUIDFromBytes("OfflinePlayer:" + name), which offline-mode servers use.
QString HostedServer::offlineUuid(const QString& name)
{
    auto hash = QCryptographicHash::hash(("OfflinePlayer:" + name).toUtf8(), QCryptographicHash::Md5);
    hash[6] = static_cast<char>((hash[6] & 0x0f) | 0x30);
    hash[8] = static_cast<char>((hash[8] & 0x3f) | 0x80);
    return QUuid::fromRfc4122(hash).toString(QUuid::WithoutBraces);
}
