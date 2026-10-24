// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <QWidget>
#include <functional>

#include "server/HostedServer.h"
#include "tasks/Task.h"
#include "ui/pages/BasePage.h"

class MinecraftInstance;
class QCheckBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPlainTextEdit;
class QPushButton;
class QSpinBox;
class QTableWidget;
class QTabWidget;

// Builds, runs and configures a dedicated server for the instance, so friends can join the same pack.
class HostServerPage : public QWidget, public BasePage {
    Q_OBJECT
   public:
    explicit HostServerPage(MinecraftInstance* instance, QWidget* parent = nullptr);

    QString displayName() const override { return tr("Host Server"); }
    QIcon icon() const override { return QIcon::fromTheme("server"); }
    QString id() const override { return "host-server"; }
    bool apply() override;
    void openedImpl() override;

   private:
    QWidget* createConsoleTab();
    QWidget* createPropertiesTab();
    QWidget* createPlayersTab();
    QWidget* createLaunchTab();

    void buildServer();
    void startServer(bool join);
    void joinServer();
    void sendConsoleCommand();

    void loadProperties();
    bool saveProperties();
    void loadSettings();
    void saveSettings();
    void reloadPlayers();
    void addPlayer(const QString& file, QLineEdit* input);

    void updateState();

    MinecraftInstance* m_instance;
    HostedServer* m_server;
    Task::Ptr m_lookupJob;

    QLabel* m_status;
    QLabel* m_info;
    QLabel* m_address;
    QPushButton* m_buildButton;
    QPushButton* m_startButton;
    QPushButton* m_startJoinButton;
    QPushButton* m_stopButton;
    QPushButton* m_joinButton;
    QTabWidget* m_tabs;

    QPlainTextEdit* m_console;
    QLineEdit* m_command;

    // Each well-known property has a widget; these move values between the file and the widgets.
    QList<std::function<void(const QString&)>> m_propertyLoaders;
    QList<std::function<QPair<QString, QString>()>> m_propertySavers;
    QStringList m_formKeys;
    QTableWidget* m_otherProperties;
    bool m_propertiesLoaded = false;

    QListWidget* m_whitelist;
    QListWidget* m_ops;
    QLineEdit* m_whitelistInput;
    QLineEdit* m_opsInput;

    QSpinBox* m_minMem;
    QSpinBox* m_maxMem;
    QLineEdit* m_javaPath;
    QPlainTextEdit* m_jvmArgs;
    QLineEdit* m_wrapper;
    QCheckBox* m_eula;
    QPlainTextEdit* m_excluded;
};
