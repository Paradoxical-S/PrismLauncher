// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <QList>
#include <QPair>
#include <QString>

// Reads and writes a Minecraft server.properties file, keeping comments and key order intact.
class ServerProperties {
   public:
    bool load(const QString& path);
    bool save(const QString& path) const;

    bool contains(const QString& key) const;
    QString value(const QString& key, const QString& fallback = {}) const;
    void setValue(const QString& key, const QString& value);
    QStringList keys() const;

    static QString escape(const QString& value);
    static QString unescape(const QString& value);

   private:
    struct Line {
        QString key;  // empty for comments and blank lines
        QString raw;  // original text for comments, value for entries
    };
    QList<Line> m_lines;
};
