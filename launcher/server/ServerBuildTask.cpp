// SPDX-License-Identifier: GPL-3.0-only
#include "ServerBuildTask.h"

#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>

#include "Application.h"
#include "BuildConfig.h"
#include "FileSystem.h"
#include "MMCZip.h"
#include "archive/ArchiveReader.h"
#include "minecraft/MinecraftInstance.h"
#include "minecraft/PackProfile.h"
#include "net/ApiDownload.h"
#include "net/ChecksumValidator.h"
#include "net/NetJob.h"
#include "server/HostedServer.h"
#include "server/ServerProperties.h"

namespace {
// Folders of a client instance that a server needs too.
const QStringList kServerFolders = { "mods",          "config",       "defaultconfigs", "kubejs",
                                     "scripts",       "global_packs", "datapacks",      "openloader",
                                     "patchouli_books", "moonlight-global-datapacks" };

// What a rebuild keeps from the existing server instead of replacing it.
const QStringList kPreserved = { "server.properties", "whitelist.json", "ops.json",     "banned-ips.json", "banned-players.json",
                                 "usercache.json",    "eula.txt",       "logs",         "crash-reports",   "backups" };

constexpr int kKeptRebuildBackups = 2;

// Mods that say in their own metadata that they only run on the client.
bool declaresClientOnly(const QString& jarPath)
{
    MMCZip::ArchiveReader jar(jarPath);
    bool clientOnly = false;
    jar.parse([&clientOnly](MMCZip::ArchiveReader::File* file, bool& stop) {
        auto name = file->filename();
        if (name == "fabric.mod.json") {
            auto obj = QJsonDocument::fromJson(file->readAll()).object();
            clientOnly = obj.value("environment").toString() == "client";
            stop = true;
        } else if (name == "quilt.mod.json") {
            auto obj = QJsonDocument::fromJson(file->readAll()).object();
            clientOnly = obj.value("minecraft").toObject().value("environment").toString() == "client";
            stop = true;
        } else if (name == "META-INF/mods.toml" || name == "META-INF/neoforge.mods.toml") {
            static const QRegularExpression flag(R"(^\s*clientSideOnly\s*=\s*true)", QRegularExpression::MultilineOption);
            clientOnly = flag.match(QString::fromUtf8(file->readAll())).hasMatch();
            stop = true;
        } else {
            file->skip();
        }
        return true;
    });
    return clientOnly;
}

// Mods whose Prism metadata (from Modrinth or CurseForge) marks them client-side.
QSet<QString> clientSideFromMetadata(const QString& modsDir)
{
    QSet<QString> files;
    static const QRegularExpression filenameLine(R"(^filename\s*=\s*['"](.+)['"])", QRegularExpression::MultilineOption);
    static const QRegularExpression sideLine(R"(^side\s*=\s*['"]client['"])", QRegularExpression::MultilineOption);
    QDirIterator it(FS::PathCombine(modsDir, ".index"), { "*.pw.toml" }, QDir::Files);
    while (it.hasNext()) {
        QFile f(it.next());
        if (!f.open(QIODevice::ReadOnly)) {
            continue;
        }
        auto text = QString::fromUtf8(f.readAll());
        auto name = filenameLine.match(text);
        if (name.hasMatch() && sideLine.match(text).hasMatch()) {
            files.insert(name.captured(1));
        }
    }
    return files;
}
}  // namespace

ServerBuildTask::ServerBuildTask(MinecraftInstance* instance, HostedServer* server) : m_instance(instance), m_server(server)
{
    m_staging = FS::PathCombine(instance->instanceRoot(), "server.staging");
    m_scratch = FS::PathCombine(instance->instanceRoot(), "server.download");
}

bool ServerBuildTask::abort()
{
    m_abort = true;
    if (m_current) {
        m_current->abort();
    }
    if (m_process) {
        m_process->kill();
    }
    return true;
}

bool ServerBuildTask::fail(const QString& reason)
{
    if (m_error.isEmpty()) {
        m_error = reason;
    }
    return false;
}

void ServerBuildTask::executeTask()
{
    if (m_server->isActive()) {
        emitFailed(tr("Stop the server before rebuilding it."));
        return;
    }
    FS::deletePath(m_staging);
    FS::deletePath(m_scratch);
    FS::ensureFolderPathExists(m_staging);
    FS::ensureFolderPathExists(m_scratch);

    m_java = m_server->settings()->get("JavaPath").toString();
    if (m_java.isEmpty()) {
        m_java = m_instance->settings()->get("JavaPath").toString();
    }

    auto ok = [this]() -> bool {
        auto type = m_instance->getManagedPackType();
        Result result = Result::NotAvailable;
        if (type == "flame") {
            result = gatherFromCurseForge();
        } else if (type == "modrinth") {
            result = gatherFromModrinth();
        }
        if (result == Result::Failed || m_abort) {
            return false;
        }
        if (result == Result::NotAvailable && !gatherFromInstance()) {
            return false;
        }
        return installLoader() && commit();
    }();

    FS::deletePath(m_scratch);
    if (!ok || m_abort) {
        FS::deletePath(m_staging);
        if (m_abort) {
            emitAborted();
        } else {
            emitFailed(m_error.isEmpty() ? tr("Building the server failed.") : m_error);
        }
        return;
    }
    emitSucceeded();
}

bool ServerBuildTask::runTask(Task::Ptr task)
{
    QEventLoop loop;
    bool succeeded = false;
    QString reason;
    connect(task.get(), &Task::succeeded, this, [&succeeded] { succeeded = true; });
    connect(task.get(), &Task::failed, this, [&reason](QString r) { reason = r; });
    connect(task.get(), &Task::finished, &loop, &QEventLoop::quit);
    connect(task.get(), &Task::progress, this, [this](qint64 current, qint64 total) { setProgress(current, total); });
    connect(task.get(), &Task::stepProgress, this, &ServerBuildTask::propagateStepProgress);
    m_current = task;
    QMetaObject::invokeMethod(task.get(), &Task::start, Qt::QueuedConnection);
    loop.exec();
    m_current.reset();
    if (!succeeded) {
        return fail(reason);
    }
    return !m_abort;
}

std::optional<QByteArray> ServerBuildTask::fetch(const QUrl& url)
{
    auto job = makeShared<NetJob>(tr("Server metadata"), APPLICATION->network());
    auto [action, response] = Net::ApiDownload::makeByteArray(url);
    job->addNetAction(action);
    if (!runTask(job)) {
        fail(tr("Could not fetch %1").arg(url.toString()));
        return {};
    }
    return *response;
}

bool ServerBuildTask::download(const QUrl& url, const QString& path, const QString& sha1, const QString& sha512)
{
    auto job = makeShared<NetJob>(tr("Server download"), APPLICATION->network());
    auto dl = Net::ApiDownload::makeFile(url, path);
    if (!sha512.isEmpty()) {
        dl->addValidator(new Net::ChecksumValidator(QCryptographicHash::Sha512, sha512));
    } else if (!sha1.isEmpty()) {
        dl->addValidator(new Net::ChecksumValidator(QCryptographicHash::Sha1, sha1));
    }
    job->addNetAction(dl);
    if (!runTask(job)) {
        return fail(tr("Could not download %1: %2").arg(url.toString(), m_error));
    }
    return true;
}

bool ServerBuildTask::insideStaging(const QString& path) const
{
    auto root = QDir(m_staging).absolutePath() + '/';
    return QDir::cleanPath(QDir(m_staging).absoluteFilePath(path)).startsWith(root);
}

ServerBuildTask::Result ServerBuildTask::gatherFromCurseForge()
{
    setStatus(tr("Looking for the official server pack…"));
    auto packId = m_instance->getManagedPackID();
    auto fileUrl = [packId](const QString& fileId) {
        return QUrl(QString("%1/mods/%2/files/%3").arg(BuildConfig.FLAME_BASE_URL, packId, fileId));
    };
    auto clientFile = fetch(fileUrl(m_instance->getManagedPackVersionID()));
    if (!clientFile) {
        return Result::Failed;
    }
    auto serverPackId = QJsonDocument::fromJson(*clientFile).object()["data"].toObject()["serverPackFileId"].toInt();
    if (serverPackId <= 0) {
        // Plenty of packs never publish one; the instance's own files are the next best thing.
        return Result::NotAvailable;
    }
    auto serverFile = fetch(fileUrl(QString::number(serverPackId)));
    if (!serverFile) {
        return Result::Failed;
    }
    auto data = QJsonDocument::fromJson(*serverFile).object()["data"].toObject();
    auto fileName = data["fileName"].toString();
    QUrl url(data["downloadUrl"].toString());
    if (!url.isValid() || url.isEmpty()) {
        // The API leaves the URL out for some projects; the CDN path is derived from the file id.
        url = QUrl(QString("https://mediafilez.forgecdn.net/files/%1/%2/%3").arg(serverPackId / 1000).arg(serverPackId % 1000).arg(fileName));
    }

    setStatus(tr("Downloading the server pack %1…").arg(fileName));
    auto zip = FS::PathCombine(m_scratch, "serverpack.zip");
    if (!download(url, zip)) {
        return Result::Failed;
    }
    setStatus(tr("Extracting the server pack…"));
    auto extracted = FS::PathCombine(m_scratch, "serverpack");
    if (!MMCZip::extractDir(zip, extracted)) {
        fail(tr("Could not extract the server pack."));
        return Result::Failed;
    }

    // Server packs are often wrapped in a top-level folder; the root is where the mods folder is.
    QString root = extracted;
    if (!QDir(root).exists("mods")) {
        QDirIterator it(extracted, { "mods" }, QDir::Dirs | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            auto candidate = QFileInfo(it.next()).absolutePath();
            if (QDir(extracted).relativeFilePath(candidate).count('/') < 2) {
                root = candidate;
                break;
            }
        }
    }
    if (!QDir(root).exists("mods")) {
        fail(tr("The server pack %1 has no mods folder.").arg(fileName));
        return Result::Failed;
    }
    if (!FS::copy(root, m_staging)()) {
        fail(tr("Could not copy the server pack."));
        return Result::Failed;
    }
    // The pack's own loader install may not match the instance's loader version; Prism installs that itself.
    QDir staging(m_staging);
    FS::deletePath(staging.filePath("libraries"));
    for (const auto& entry : staging.entryList({ "*.jar", "run.sh", "run.bat", "user_jvm_args.txt" }, QDir::Files)) {
        staging.remove(entry);
    }
    m_source = tr("CurseForge server pack (%1)").arg(fileName);
    return Result::Ok;
}

ServerBuildTask::Result ServerBuildTask::gatherFromModrinth()
{
    setStatus(tr("Fetching the Modrinth pack…"));
    auto version = fetch(QUrl(QString("%1/version/%2").arg(BuildConfig.MODRINTH_PROD_URL, m_instance->getManagedPackVersionID())));
    if (!version) {
        return Result::Failed;
    }
    QJsonObject packFile;
    for (auto file : QJsonDocument::fromJson(*version).object()["files"].toArray()) {
        auto obj = file.toObject();
        if (obj["primary"].toBool() || (packFile.isEmpty() && obj["filename"].toString().endsWith(".mrpack"))) {
            packFile = obj;
        }
    }
    if (packFile.isEmpty()) {
        return Result::NotAvailable;
    }
    auto mrpack = FS::PathCombine(m_scratch, "pack.mrpack");
    if (!download(QUrl(packFile["url"].toString()), mrpack, {}, packFile["hashes"].toObject()["sha512"].toString())) {
        return Result::Failed;
    }

    MMCZip::ArchiveReader zip(mrpack);
    QByteArray indexData;
    zip.parse([&indexData](MMCZip::ArchiveReader::File* file, bool& stop) {
        if (file->filename() == "modrinth.index.json") {
            indexData = file->readAll();
            stop = true;
        } else {
            file->skip();
        }
        return true;
    });
    auto index = QJsonDocument::fromJson(indexData).object();
    if (index.isEmpty()) {
        fail(tr("The pack has no modrinth.index.json."));
        return Result::Failed;
    }

    setStatus(tr("Downloading server mods…"));
    auto job = makeShared<NetJob>(tr("Server mods"), APPLICATION->network());
    for (auto value : index["files"].toArray()) {
        auto file = value.toObject();
        auto path = file["path"].toString().replace('\\', '/');
        auto side = file["env"].toObject()["server"].toString("required");
        if (side == "unsupported") {
            m_excluded.append(path);
            continue;
        }
        if (!insideStaging(path)) {
            fail(tr("The pack tries to write outside the server folder: %1").arg(path));
            return Result::Failed;
        }
        auto downloads = file["downloads"].toArray();
        if (downloads.isEmpty()) {
            continue;
        }
        auto dl = Net::ApiDownload::makeFile(QUrl(downloads.first().toString()), FS::PathCombine(m_staging, path));
        dl->addValidator(new Net::ChecksumValidator(QCryptographicHash::Sha512, file["hashes"].toObject()["sha512"].toString()));
        job->addNetAction(dl);
    }
    if (!runTask(job)) {
        return Result::Failed;
    }

    setStatus(tr("Extracting overrides…"));
    for (const auto& folder : { "overrides/", "server-overrides/" }) {
        MMCZip::ArchiveReader overrides(mrpack);
        if (!MMCZip::extractSubDir(&overrides, folder, m_staging)) {
            fail(tr("Could not extract %1 from the pack.").arg(folder));
            return Result::Failed;
        }
    }
    m_source = tr("Modrinth pack, server files only");
    return Result::Ok;
}

bool ServerBuildTask::gatherFromInstance()
{
    setStatus(tr("Copying the instance's files…"));
    QDir game(m_instance->gameRoot());
    for (const auto& folder : kServerFolders) {
        if (folder == "mods" || !game.exists(folder)) {
            continue;
        }
        if (!FS::copy(game.filePath(folder), FS::PathCombine(m_staging, folder))()) {
            return fail(tr("Could not copy %1.").arg(folder));
        }
    }

    auto modsDir = game.filePath("mods");
    auto clientSide = clientSideFromMetadata(modsDir);
    auto targetMods = FS::PathCombine(m_staging, "mods");
    FS::ensureFolderPathExists(targetMods);
    auto jars = QDir(modsDir).entryInfoList({ "*.jar", "*.zip" }, QDir::Files);
    int done = 0;
    for (const auto& jar : jars) {
        setProgress(done++, jars.size());
        if (m_abort) {
            return false;
        }
        if (clientSide.contains(jar.fileName()) || declaresClientOnly(jar.absoluteFilePath())) {
            m_excluded.append("mods/" + jar.fileName());
            continue;
        }
        if (!QFile::copy(jar.absoluteFilePath(), FS::PathCombine(targetMods, jar.fileName()))) {
            return fail(tr("Could not copy %1.").arg(jar.fileName()));
        }
    }
    m_source = m_instance->isManagedPack() ? tr("Instance files (the pack has no server pack)") : tr("Instance files");
    return true;
}

bool ServerBuildTask::runJava(const QStringList& args)
{
    QEventLoop loop;
    m_process = new QProcess(this);
    m_process->setWorkingDirectory(m_staging);
    m_process->setProcessChannelMode(QProcess::MergedChannels);
    connect(m_process, &QProcess::readyReadStandardOutput, this, [this] {
        auto lines = QString::fromUtf8(m_process->readAllStandardOutput()).split('\n', Qt::SkipEmptyParts);
        if (!lines.isEmpty()) {
            setDetails(lines.last().trimmed());
        }
    });
    connect(m_process, &QProcess::finished, &loop, &QEventLoop::quit);
    connect(m_process, &QProcess::errorOccurred, &loop, [&loop](QProcess::ProcessError err) {
        if (err == QProcess::FailedToStart) {
            loop.quit();
        }
    });
    m_process->start(m_java.isEmpty() ? "java" : m_java, args);
    loop.exec();
    auto ok = m_process->exitStatus() == QProcess::NormalExit && m_process->exitCode() == 0 && m_process->error() != QProcess::FailedToStart;
    if (!ok) {
        fail(tr("The loader installer failed (%1).").arg(m_process->error() == QProcess::FailedToStart ? m_process->errorString()
                                                                                                         : tr("exit code %1").arg(m_process->exitCode())));
    }
    m_process->deleteLater();
    m_process = nullptr;
    return ok && !m_abort;
}

bool ServerBuildTask::installLoader()
{
    auto profile = m_instance->getPackProfile();
    auto mc = profile->getComponentVersion("net.minecraft");
    if (mc.isEmpty()) {
        return fail(tr("The instance has no Minecraft version."));
    }
    auto fabric = profile->getComponentVersion("net.fabricmc.fabric-loader");
    auto quilt = profile->getComponentVersion("org.quiltmc.quilt-loader");
    auto forge = profile->getComponentVersion("net.minecraftforge");
    auto neoforge = profile->getComponentVersion("net.neoforged");

    if (!fabric.isEmpty()) {
        setStatus(tr("Installing Fabric %1 for the server…").arg(fabric));
        auto installers = fetch(QUrl("https://meta.fabricmc.net/v2/versions/installer"));
        if (!installers) {
            return false;
        }
        QString installer;
        for (auto v : QJsonDocument::fromJson(*installers).array()) {
            if (v.toObject()["stable"].toBool()) {
                installer = v.toObject()["version"].toString();
                break;
            }
        }
        if (installer.isEmpty()) {
            return fail(tr("Could not find a Fabric installer version."));
        }
        auto url = QString("https://meta.fabricmc.net/v2/versions/loader/%1/%2/%3/server/jar").arg(mc, fabric, installer);
        if (!download(QUrl(url), FS::PathCombine(m_staging, "fabric-server-launch.jar"))) {
            return false;
        }
        // No return: the Fabric launcher wraps the vanilla server.jar, which the vanilla step below fetches. Otherwise it
        // would download it on first start, and fail there instead of here.
    }

    if (!quilt.isEmpty()) {
        setStatus(tr("Installing Quilt %1 for the server…").arg(quilt));
        auto installers = fetch(QUrl("https://meta.quiltmc.org/v3/versions/installer"));
        if (!installers) {
            return false;
        }
        auto list = QJsonDocument::fromJson(*installers).array();
        if (list.isEmpty()) {
            return fail(tr("Could not find a Quilt installer version."));
        }
        auto v = list.first().toObject()["version"].toString();
        auto jar = FS::PathCombine(m_scratch, "quilt-installer.jar");
        if (!download(QUrl(QString("https://maven.quiltmc.org/repository/release/org/quiltmc/quilt-installer/%1/quilt-installer-%1.jar").arg(v)), jar)) {
            return false;
        }
        return runJava({ "-jar", jar, "install", "server", mc, quilt, "--download-server", "--install-dir=" + m_staging });
    }

    if (!forge.isEmpty() || !neoforge.isEmpty()) {
        QString url;
        if (!neoforge.isEmpty()) {
            setStatus(tr("Installing NeoForge %1 for the server…").arg(neoforge));
            // NeoForge for 1.20.1 still used Forge's coordinates.
            url = mc == "1.20.1"
                      ? QString("https://maven.neoforged.net/releases/net/neoforged/forge/1.20.1-%1/forge-1.20.1-%1-installer.jar").arg(neoforge)
                      : QString("https://maven.neoforged.net/releases/net/neoforged/neoforge/%1/neoforge-%1-installer.jar").arg(neoforge);
        } else {
            setStatus(tr("Installing Forge %1 for the server…").arg(forge));
            auto full = forge.startsWith(mc + "-") ? forge : mc + "-" + forge;
            url = QString("https://maven.minecraftforge.net/net/minecraftforge/forge/%1/forge-%1-installer.jar").arg(full);
        }
        auto jar = FS::PathCombine(m_scratch, "installer.jar");
        if (!download(QUrl(url), jar)) {
            return false;
        }
        if (!runJava({ "-jar", jar, "--installServer", m_staging })) {
            return false;
        }
        QDir staging(m_staging);
        for (const auto& leftover : staging.entryList({ "*installer.jar.log", "installer.log", "run.bat" }, QDir::Files)) {
            staging.remove(leftover);
        }
        return true;
    }

    // Vanilla, and the jar the Fabric launcher wraps.
    setStatus(tr("Downloading the Minecraft %1 server…").arg(mc));
    auto manifest = fetch(QUrl("https://piston-meta.mojang.com/mc/game/version_manifest_v2.json"));
    if (!manifest) {
        return false;
    }
    QString versionUrl;
    for (auto v : QJsonDocument::fromJson(*manifest).object()["versions"].toArray()) {
        if (v.toObject()["id"].toString() == mc) {
            versionUrl = v.toObject()["url"].toString();
            break;
        }
    }
    if (versionUrl.isEmpty()) {
        return fail(tr("Minecraft %1 is not in Mojang's version list.").arg(mc));
    }
    auto versionJson = fetch(QUrl(versionUrl));
    if (!versionJson) {
        return false;
    }
    auto server = QJsonDocument::fromJson(*versionJson).object()["downloads"].toObject()["server"].toObject();
    if (server.isEmpty()) {
        return fail(tr("Minecraft %1 has no dedicated server.").arg(mc));
    }
    return download(QUrl(server["url"].toString()), FS::PathCombine(m_staging, "server.jar"), server["sha1"].toString());
}

bool ServerBuildTask::commit()
{
    setStatus(tr("Putting the new server in place…"));
    auto target = m_server->serverDir();
    QDir dir(target);
    bool firstBuild = !dir.exists();

    if (firstBuild) {
        if (!FS::move(m_staging, target)) {
            return fail(tr("Could not move the server into %1.").arg(target));
        }
    } else {
        auto preserved = kPreserved;
        auto level = m_server->levelName();
        // Worlds: the level folder plus the _nether/_the_end folders Bukkit-style servers split off.
        for (const auto& entry : dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
            if (entry == level || entry.startsWith(level + "_")) {
                preserved.append(entry);
            }
        }

        auto backup = FS::PathCombine(target, "backups", "rebuild-" + QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss"));
        FS::ensureFolderPathExists(backup);
        for (const auto& entry : dir.entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden)) {
            if (preserved.contains(entry)) {
                continue;
            }
            if (!FS::move(dir.filePath(entry), FS::PathCombine(backup, entry))) {
                return fail(tr("Could not move %1 out of the way.").arg(entry));
            }
        }
        QDir staging(m_staging);
        for (const auto& entry : staging.entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden)) {
            if (preserved.contains(entry) && dir.exists(entry)) {
                continue;
            }
            if (!FS::move(staging.filePath(entry), dir.filePath(entry))) {
                return fail(tr("Could not move %1 into the server.").arg(entry));
            }
        }
        FS::deletePath(m_staging);

        QDir backups(FS::PathCombine(target, "backups"));
        auto old = backups.entryList({ "rebuild-*" }, QDir::Dirs, QDir::Name | QDir::Reversed);
        for (int i = kKeptRebuildBackups; i < old.size(); i++) {
            FS::deletePath(backups.filePath(old[i]));
        }
    }

    // Sensible defaults for a pack server the first time round; the user's edits win after that.
    ServerProperties props;
    props.load(m_server->propertiesPath());
    if (!props.contains("motd")) {
        props.setValue("motd", m_instance->name());
    }
    if (!props.contains("allow-flight")) {
        // Modded movement (jetpacks, elytra mods, climbing) trips the vanilla flight check.
        props.setValue("allow-flight", "true");
    }
    if (!props.contains("server-port")) {
        props.setValue("server-port", "25565");
    }
    props.save(m_server->propertiesPath());

    auto settings = m_server->settings();
    settings->set("BuiltFromVersionId", m_instance->getManagedPackVersionID());
    settings->set("BuiltFromVersionName", m_instance->getManagedPackVersionName());
    settings->set("BuiltFromSource", m_source);
    settings->set("ExcludedMods", m_excluded);
    m_server->refresh();
    return true;
}
