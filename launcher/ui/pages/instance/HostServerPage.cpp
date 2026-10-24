// SPDX-License-Identifier: GPL-3.0-only
#include "HostServerPage.h"

#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QFileDialog>
#include <QFontDatabase>
#include <QFormLayout>
#include <QGroupBox>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QNetworkInterface>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollBar>
#include <QSpinBox>
#include <QTabWidget>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>

#include "Application.h"
#include "DesktopServices.h"
#include "minecraft/MinecraftInstance.h"
#include "minecraft/launch/MinecraftTarget.h"
#include "net/ApiDownload.h"
#include "net/NetJob.h"
#include "server/ServerBuildTask.h"
#include "server/ServerProperties.h"
#include "ui/dialogs/CustomMessageBox.h"
#include "ui/dialogs/ProgressDialog.h"

namespace {
QPushButton* button(const QString& text, const QString& icon = {})
{
    auto b = new QPushButton(text);
    if (!icon.isEmpty()) {
        b->setIcon(QIcon::fromTheme(icon));
    }
    return b;
}

QStringList lanAddresses()
{
    QStringList out;
    for (const auto& address : QNetworkInterface::allAddresses()) {
        if (address.protocol() == QAbstractSocket::IPv4Protocol && !address.isLoopback() && address.isPrivateUse()) {
            out.append(address.toString());
        }
    }
    return out;
}
}  // namespace

HostServerPage::HostServerPage(MinecraftInstance* instance, QWidget* parent)
    : QWidget(parent), m_instance(instance), m_server(HostedServer::get(instance))
{
    auto layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);

    m_status = new QLabel;
    m_status->setTextFormat(Qt::RichText);
    m_info = new QLabel;
    m_info->setWordWrap(true);
    m_info->setTextFormat(Qt::RichText);
    m_address = new QLabel;
    m_address->setWordWrap(true);
    m_address->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(m_status);
    layout->addWidget(m_info);
    layout->addWidget(m_address);

    auto buttons = new QHBoxLayout;
    m_buildButton = button(tr("Create Server"), "new");
    m_startButton = button(tr("Start"), "launch");
    m_startJoinButton = button(tr("Start && Join"));
    m_stopButton = button(tr("Stop"), "status-bad");
    m_joinButton = button(tr("Join"));
    auto copyButton = button(tr("Copy Address"), "copy");
    auto folderButton = button(tr("Folder"), "viewfolder");
    for (auto b : { m_buildButton, m_startButton, m_startJoinButton, m_stopButton, m_joinButton, copyButton, folderButton }) {
        buttons->addWidget(b);
    }
    buttons->addStretch();
    layout->addLayout(buttons);

    m_tabs = new QTabWidget;
    m_tabs->addTab(createConsoleTab(), tr("Console"));
    m_tabs->addTab(createPropertiesTab(), tr("Properties"));
    m_tabs->addTab(createPlayersTab(), tr("Players"));
    m_tabs->addTab(createLaunchTab(), tr("Launch"));
    layout->addWidget(m_tabs, 1);

    connect(m_buildButton, &QPushButton::clicked, this, &HostServerPage::buildServer);
    connect(m_startButton, &QPushButton::clicked, this, [this] { startServer(false); });
    connect(m_startJoinButton, &QPushButton::clicked, this, [this] { startServer(true); });
    connect(m_stopButton, &QPushButton::clicked, this, [this] {
        if (m_server->state() == HostedServer::State::Stopping) {
            m_server->kill();
        } else {
            m_server->stop();
        }
    });
    connect(m_joinButton, &QPushButton::clicked, this, &HostServerPage::joinServer);
    connect(copyButton, &QPushButton::clicked, this, [this] {
        auto lan = lanAddresses();
        auto host = lan.isEmpty() ? QString("localhost") : lan.first();
        QGuiApplication::clipboard()->setText(QString("%1:%2").arg(host).arg(m_server->port()));
    });
    connect(folderButton, &QPushButton::clicked, this, [this] { DesktopServices::openPath(m_server->serverDir(), true); });

    connect(m_server, &HostedServer::stateChanged, this, &HostServerPage::updateState);
    connect(m_server, &HostedServer::consoleLine, this, [this](const QString& line) {
        auto bar = m_console->verticalScrollBar();
        bool atBottom = bar->value() == bar->maximum();
        m_console->appendPlainText(line);
        if (atBottom) {
            bar->setValue(bar->maximum());
        }
        // Player lists change when someone runs whitelist/op commands, from here or in game.
        if (line.contains("whitelist", Qt::CaseInsensitive) || line.contains("operator", Qt::CaseInsensitive)) {
            QTimer::singleShot(500, this, &HostServerPage::reloadPlayers);
        }
    });

    m_console->setPlainText(m_server->consoleHistory().join('\n'));
    loadSettings();
    loadProperties();
    reloadPlayers();
    updateState();
}

void HostServerPage::openedImpl()
{
    updateState();
}

bool HostServerPage::apply()
{
    saveSettings();
    saveProperties();
    return true;
}

QWidget* HostServerPage::createConsoleTab()
{
    auto tab = new QWidget;
    auto layout = new QVBoxLayout(tab);
    m_console = new QPlainTextEdit;
    m_console->setReadOnly(true);
    m_console->setMaximumBlockCount(5000);
    m_console->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    m_console->setLineWrapMode(QPlainTextEdit::NoWrap);
    layout->addWidget(m_console, 1);

    auto row = new QHBoxLayout;
    m_command = new QLineEdit;
    m_command->setPlaceholderText(tr("Server command, e.g. say hello or time set day"));
    auto send = button(tr("Send"));
    row->addWidget(m_command, 1);
    row->addWidget(send);
    layout->addLayout(row);
    connect(m_command, &QLineEdit::returnPressed, this, &HostServerPage::sendConsoleCommand);
    connect(send, &QPushButton::clicked, this, &HostServerPage::sendConsoleCommand);
    return tab;
}

QWidget* HostServerPage::createPropertiesTab()
{
    auto tab = new QWidget;
    auto layout = new QVBoxLayout(tab);
    auto note = new QLabel(tr("Changes take effect the next time the server starts."));
    layout->addWidget(note);

    auto form = new QFormLayout;
    auto addText = [this, form](const QString& key, const QString& label, const QString& fallback) {
        auto edit = new QLineEdit;
        form->addRow(label, edit);
        m_formKeys.append(key);
        m_propertyLoaders.append([edit, fallback](const QString& v) { edit->setText(v.isNull() ? fallback : v); });
        m_propertySavers.append([edit, key] { return qMakePair(key, edit->text()); });
    };
    auto addNumber = [this, form](const QString& key, const QString& label, int min, int max, int fallback) {
        auto spin = new QSpinBox;
        spin->setRange(min, max);
        form->addRow(label, spin);
        m_formKeys.append(key);
        m_propertyLoaders.append([spin, fallback](const QString& v) {
            bool ok = false;
            auto n = v.toInt(&ok);
            spin->setValue(ok ? n : fallback);
        });
        m_propertySavers.append([spin, key] { return qMakePair(key, QString::number(spin->value())); });
    };
    auto addFlag = [this, form](const QString& key, const QString& label, bool fallback) {
        auto check = new QCheckBox(label);
        form->addRow(QString(), check);
        m_formKeys.append(key);
        m_propertyLoaders.append([check, fallback](const QString& v) { check->setChecked(v.isNull() ? fallback : v == "true"); });
        m_propertySavers.append([check, key] { return qMakePair(key, QString(check->isChecked() ? "true" : "false")); });
    };
    auto addChoice = [this, form](const QString& key, const QString& label, const QStringList& values, const QString& fallback) {
        auto combo = new QComboBox;
        combo->addItems(values);
        form->addRow(label, combo);
        m_formKeys.append(key);
        m_propertyLoaders.append([combo, fallback](const QString& v) { combo->setCurrentText(v.isNull() ? fallback : v); });
        m_propertySavers.append([combo, key] { return qMakePair(key, combo->currentText()); });
    };

    // Fallbacks are vanilla's defaults, shown until the server writes its first server.properties.
    addText("motd", tr("Message of the day"), m_instance->name());
    addNumber("server-port", tr("Port"), 1, 65535, 25565);
    addNumber("max-players", tr("Max players"), 1, 1000, 20);
    addChoice("difficulty", tr("Difficulty"), { "peaceful", "easy", "normal", "hard" }, "easy");
    addChoice("gamemode", tr("Game mode"), { "survival", "creative", "adventure", "spectator" }, "survival");
    addNumber("view-distance", tr("View distance"), 2, 32, 10);
    addNumber("simulation-distance", tr("Simulation distance"), 2, 32, 10);
    addNumber("spawn-protection", tr("Spawn protection radius"), 0, 1000, 16);
    addText("level-name", tr("World folder"), "world");
    addText("level-seed", tr("Seed (new worlds only)"), "");
    addFlag("online-mode", tr("Online mode (players need real Minecraft accounts)"), true);
    addFlag("white-list", tr("Only allow whitelisted players"), false);
    addFlag("enforce-whitelist", tr("Kick players who are removed from the whitelist"), false);
    addFlag("pvp", tr("PvP"), true);
    addFlag("allow-flight", tr("Allow flight (most modpacks need this)"), true);
    addFlag("hardcore", tr("Hardcore"), false);
    layout->addLayout(form);

    layout->addWidget(new QLabel(tr("Other properties")));
    m_otherProperties = new QTableWidget(0, 2);
    m_otherProperties->setHorizontalHeaderLabels({ tr("Property"), tr("Value") });
    m_otherProperties->horizontalHeader()->setStretchLastSection(true);
    m_otherProperties->verticalHeader()->hide();
    layout->addWidget(m_otherProperties, 1);

    auto save = button(tr("Save Properties"), "document-save");
    layout->addWidget(save, 0, Qt::AlignRight);
    connect(save, &QPushButton::clicked, this, &HostServerPage::saveProperties);
    return tab;
}

QWidget* HostServerPage::createPlayersTab()
{
    auto tab = new QWidget;
    auto layout = new QHBoxLayout(tab);
    auto makeList = [this, layout](const QString& title, const QString& file, QListWidget*& list, QLineEdit*& input) {
        auto box = new QGroupBox(title);
        auto boxLayout = new QVBoxLayout(box);
        list = new QListWidget;
        boxLayout->addWidget(list, 1);
        auto row = new QHBoxLayout;
        input = new QLineEdit;
        input->setPlaceholderText(tr("Player name"));
        auto add = button(tr("Add"));
        auto remove = button(tr("Remove"));
        row->addWidget(input, 1);
        row->addWidget(add);
        row->addWidget(remove);
        boxLayout->addLayout(row);
        layout->addWidget(box);

        auto lineEdit = input;
        auto listWidget = list;
        connect(add, &QPushButton::clicked, this, [this, file, lineEdit] { addPlayer(file, lineEdit); });
        connect(input, &QLineEdit::returnPressed, this, [this, file, lineEdit] { addPlayer(file, lineEdit); });
        connect(remove, &QPushButton::clicked, this, [this, file, listWidget] {
            if (auto item = listWidget->currentItem()) {
                m_server->removePlayer(file, item->text());
                reloadPlayers();
            }
        });
    };
    makeList(tr("Whitelist"), "whitelist.json", m_whitelist, m_whitelistInput);
    makeList(tr("Operators"), "ops.json", m_ops, m_opsInput);
    return tab;
}

QWidget* HostServerPage::createLaunchTab()
{
    auto tab = new QWidget;
    auto layout = new QVBoxLayout(tab);
    auto form = new QFormLayout;

    m_minMem = new QSpinBox;
    m_maxMem = new QSpinBox;
    for (auto spin : { m_minMem, m_maxMem }) {
        spin->setRange(512, 256 * 1024);
        spin->setSingleStep(512);
        spin->setSuffix(" MiB");
    }
    form->addRow(tr("Minimum memory"), m_minMem);
    form->addRow(tr("Maximum memory"), m_maxMem);

    auto javaRow = new QHBoxLayout;
    m_javaPath = new QLineEdit;
    m_javaPath->setPlaceholderText(tr("Same Java as the instance"));
    auto browse = button(tr("Browse…"));
    javaRow->addWidget(m_javaPath, 1);
    javaRow->addWidget(browse);
    form->addRow(tr("Java"), javaRow);
    connect(browse, &QPushButton::clicked, this, [this] {
        auto path = QFileDialog::getOpenFileName(this, tr("Select Java"));
        if (!path.isEmpty()) {
            m_javaPath->setText(path);
        }
    });

    m_jvmArgs = new QPlainTextEdit;
    m_jvmArgs->setMaximumHeight(80);
    form->addRow(tr("JVM arguments"), m_jvmArgs);

    m_wrapper = new QLineEdit;
    m_wrapper->setToolTip(tr("Runs before Java, e.g. a resource broker or nice. Variables: $INST_NAME, $INST_ID, $SERVER_DIR, "
                             "$SERVER_MAX_MEM_MB, $SERVER_LEASE_GB (maximum memory plus 2 GiB, rounded up)."));
    form->addRow(tr("Wrapper command"), m_wrapper);
    layout->addLayout(form);

    m_eula = new QCheckBox(tr("I agree to the Minecraft End User License Agreement"));
    auto eulaLink = new QLabel(tr("<a href=\"https://aka.ms/MinecraftEULA\">Read the EULA</a>"));
    eulaLink->setOpenExternalLinks(true);
    layout->addWidget(m_eula);
    layout->addWidget(eulaLink);

    layout->addWidget(new QLabel(tr("Left out of the server because they only run on the client:")));
    m_excluded = new QPlainTextEdit;
    m_excluded->setReadOnly(true);
    layout->addWidget(m_excluded, 1);

    auto save = button(tr("Save"), "document-save");
    layout->addWidget(save, 0, Qt::AlignRight);
    connect(save, &QPushButton::clicked, this, &HostServerPage::saveSettings);
    return tab;
}

void HostServerPage::loadSettings()
{
    auto s = m_server->settings();
    m_minMem->setValue(s->get("MinMemAlloc").toInt());
    m_maxMem->setValue(s->get("MaxMemAlloc").toInt());
    m_javaPath->setText(s->get("JavaPath").toString());
    m_jvmArgs->setPlainText(s->get("JvmArgs").toString());
    m_wrapper->setText(s->get("WrapperCommand").toString());
    m_eula->setChecked(s->get("EulaAccepted").toBool());
    m_excluded->setPlainText(s->get("ExcludedMods").toStringList().join('\n'));
}

void HostServerPage::saveSettings()
{
    auto s = m_server->settings();
    s->set("MinMemAlloc", std::min(m_minMem->value(), m_maxMem->value()));
    s->set("MaxMemAlloc", std::max(m_minMem->value(), m_maxMem->value()));
    s->set("JavaPath", m_javaPath->text().trimmed());
    s->set("JvmArgs", m_jvmArgs->toPlainText().simplified());
    s->set("WrapperCommand", m_wrapper->text().trimmed());
    s->set("EulaAccepted", m_eula->isChecked());
}

void HostServerPage::loadProperties()
{
    ServerProperties props;
    m_propertiesLoaded = props.load(m_server->propertiesPath());
    for (int i = 0; i < m_formKeys.size(); i++) {
        m_propertyLoaders[i](props.contains(m_formKeys[i]) ? props.value(m_formKeys[i]) : QString());
    }
    m_otherProperties->setRowCount(0);
    for (const auto& key : props.keys()) {
        if (m_formKeys.contains(key)) {
            continue;
        }
        int row = m_otherProperties->rowCount();
        m_otherProperties->insertRow(row);
        auto keyItem = new QTableWidgetItem(key);
        keyItem->setFlags(keyItem->flags() & ~Qt::ItemIsEditable);
        m_otherProperties->setItem(row, 0, keyItem);
        m_otherProperties->setItem(row, 1, new QTableWidgetItem(props.value(key)));
    }
}

bool HostServerPage::saveProperties()
{
    if (!m_propertiesLoaded) {
        return false;
    }
    ServerProperties props;
    props.load(m_server->propertiesPath());
    for (const auto& saver : m_propertySavers) {
        auto [key, value] = saver();
        props.setValue(key, value);
    }
    for (int row = 0; row < m_otherProperties->rowCount(); row++) {
        props.setValue(m_otherProperties->item(row, 0)->text(), m_otherProperties->item(row, 1)->text());
    }
    if (!props.save(m_server->propertiesPath())) {
        CustomMessageBox::selectable(this, tr("Error"), tr("Could not save server.properties."), QMessageBox::Critical)->show();
        return false;
    }
    updateState();
    return true;
}

void HostServerPage::reloadPlayers()
{
    m_whitelist->clear();
    m_whitelist->addItems(m_server->playerList("whitelist.json"));
    m_ops->clear();
    m_ops->addItems(m_server->playerList("ops.json"));
}

void HostServerPage::addPlayer(const QString& file, QLineEdit* input)
{
    auto name = input->text().trimmed();
    if (name.isEmpty()) {
        return;
    }
    input->clear();
    ServerProperties props;
    props.load(m_server->propertiesPath());
    // A running server looks the player up itself; offline servers derive the UUID from the name.
    if (m_server->state() == HostedServer::State::Running || props.value("online-mode", "true") != "true") {
        m_server->addPlayer(file, name, HostedServer::offlineUuid(name));
        reloadPlayers();
        return;
    }
    auto job = makeShared<NetJob>(tr("Look up player"), APPLICATION->network());
    auto [action, response] = Net::ApiDownload::makeByteArray(QUrl("https://api.mojang.com/users/profiles/minecraft/" + name));
    job->addNetAction(action);
    connect(job.get(), &Task::succeeded, this, [this, file, response] {
        auto obj = QJsonDocument::fromJson(*response).object();
        auto id = obj["id"].toString();
        if (id.size() != 32) {
            return;
        }
        auto uuid = QString("%1-%2-%3-%4-%5").arg(id.mid(0, 8), id.mid(8, 4), id.mid(12, 4), id.mid(16, 4), id.mid(20));
        m_server->addPlayer(file, obj["name"].toString(), uuid);
        reloadPlayers();
    });
    connect(job.get(), &Task::failed, this, [this, name] {
        CustomMessageBox::selectable(this, tr("Player not found"), tr("There is no Minecraft account called %1.").arg(name),
                                     QMessageBox::Warning)
            ->show();
    });
    m_lookupJob = job;
    job->start();
}

void HostServerPage::buildServer()
{
    bool rebuild = m_server->isBuilt();
    if (rebuild) {
        auto answer = QMessageBox::question(
            this, tr("Update Server"),
            tr("Rebuild the server from the instance's current pack version?\n\nWorlds, server.properties and player lists are kept. Mods, "
               "configs and the loader are replaced; the old ones go to server/backups."));
        if (answer != QMessageBox::Yes) {
            return;
        }
    }
    saveSettings();
    ServerBuildTask task(m_instance, m_server);
    ProgressDialog dialog(this);
    dialog.setSkipButton(true, tr("Abort"));
    dialog.execWithTask(&task);
    if (task.getState() == Task::State::Failed) {
        CustomMessageBox::selectable(this, tr("Server not built"), task.failReason(), QMessageBox::Critical)->show();
    }
    loadSettings();
    loadProperties();
    reloadPlayers();
    updateState();
}

void HostServerPage::startServer(bool join)
{
    saveSettings();
    saveProperties();
    QString error;
    if (!m_server->start(&error)) {
        CustomMessageBox::selectable(this, tr("Server not started"), error, QMessageBox::Warning)->show();
        if (!m_eula->isChecked()) {
            m_tabs->setCurrentIndex(3);
        }
        return;
    }
    m_tabs->setCurrentIndex(0);
    if (join) {
        auto instance = m_instance;
        auto server = m_server;
        connect(m_server, &HostedServer::ready, APPLICATION, [instance, server] {
            auto target = std::make_shared<MinecraftTarget>(MinecraftTarget::parse(QString("localhost:%1").arg(server->port()), false));
            APPLICATION->launch(instance, LaunchMode::Normal, target);
        }, Qt::SingleShotConnection);
    }
}

void HostServerPage::joinServer()
{
    auto target = std::make_shared<MinecraftTarget>(MinecraftTarget::parse(QString("localhost:%1").arg(m_server->port()), false));
    APPLICATION->launch(m_instance, LaunchMode::Normal, target);
}

void HostServerPage::sendConsoleCommand()
{
    auto command = m_command->text().trimmed();
    if (command.startsWith('/')) {
        command.remove(0, 1);
    }
    if (!command.isEmpty() && m_server->sendCommand(command)) {
        m_command->clear();
    }
}

void HostServerPage::updateState()
{
    using State = HostedServer::State;
    auto state = m_server->state();
    bool built = m_server->isBuilt();
    bool active = m_server->isActive();

    QString color = "gray";
    QString text;
    switch (state) {
        case State::Missing:
            text = tr("No server yet");
            break;
        case State::Stopped:
            text = tr("Stopped");
            break;
        case State::Starting:
            color = "orange";
            text = tr("Starting…");
            break;
        case State::Running:
            color = "green";
            text = tr("Running");
            break;
        case State::Stopping:
            color = "orange";
            text = tr("Stopping…");
            break;
    }
    m_status->setText(QString("<b>%1</b> <span style=\"color:%2\">●</span> %3").arg(tr("Server"), color, text));

    QStringList info;
    auto s = m_server->settings();
    if (!built) {
        info << tr("Creates a dedicated server with the same pack, mods and loader as this instance.");
    } else {
        auto version = s->get("BuiltFromVersionName").toString();
        info << tr("Built from %1%2.")
                    .arg(s->get("BuiltFromSource").toString(), version.isEmpty() ? QString() : tr(", pack version %1").arg(version));
        if (m_instance->isManagedPack() && s->get("BuiltFromVersionId").toString() != m_instance->getManagedPackVersionID()) {
            info << tr("<b style=\"color:orange\">The instance is now on pack version %1. Update the server so friends on the new "
                       "version can join.</b>")
                        .arg(m_instance->getManagedPackVersionName());
        }
    }
    m_info->setText(info.join("<br>"));

    if (built) {
        auto lan = lanAddresses();
        auto port = m_server->port();
        QStringList lines;
        lines << tr("On this PC: localhost:%1").arg(port);
        if (!lan.isEmpty()) {
            QStringList withPort;
            for (const auto& a : lan) {
                withPort << QString("%1:%2").arg(a).arg(port);
            }
            lines << tr("Friends on your network: %1").arg(withPort.join(", "));
        }
        lines << tr("Friends over the internet: forward TCP port %1 on your router, or share the server over Tailscale or playit.gg.")
                     .arg(port);
        m_address->setText(lines.join('\n'));
    } else {
        m_address->clear();
    }

    m_buildButton->setText(built ? tr("Update Server") : tr("Create Server"));
    m_buildButton->setEnabled(!active);
    m_startButton->setEnabled(built && !active);
    m_startJoinButton->setEnabled(built && !active);
    m_stopButton->setEnabled(active);
    m_stopButton->setText(state == State::Stopping ? tr("Kill") : tr("Stop"));
    m_joinButton->setEnabled(state == State::Running);
    m_command->setEnabled(active);
}
