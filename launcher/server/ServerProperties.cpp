// SPDX-License-Identifier: GPL-3.0-only
#include "ServerProperties.h"

#include <QFile>
#include <QSaveFile>
#include <QTextStream>

bool ServerProperties::load(const QString& path)
{
    m_lines.clear();
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return false;
    }
    QTextStream in(&file);
    while (!in.atEnd()) {
        auto text = in.readLine();
        auto trimmed = text.trimmed();
        if (trimmed.isEmpty() || trimmed.startsWith('#') || trimmed.startsWith('!')) {
            m_lines.append({ {}, text });
            continue;
        }
        auto sep = text.indexOf('=');
        if (sep < 0) {
            m_lines.append({ {}, text });
            continue;
        }
        m_lines.append({ text.left(sep).trimmed(), unescape(text.mid(sep + 1)) });
    }
    return true;
}

bool ServerProperties::save(const QString& path) const
{
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        return false;
    }
    QTextStream out(&file);
    for (const auto& line : m_lines) {
        if (line.key.isEmpty()) {
            out << line.raw << '\n';
        } else {
            out << line.key << '=' << escape(line.raw) << '\n';
        }
    }
    out.flush();
    return file.commit();
}

bool ServerProperties::contains(const QString& key) const
{
    for (const auto& line : m_lines) {
        if (line.key == key) {
            return true;
        }
    }
    return false;
}

QString ServerProperties::value(const QString& key, const QString& fallback) const
{
    for (const auto& line : m_lines) {
        if (line.key == key) {
            return line.raw;
        }
    }
    return fallback;
}

void ServerProperties::setValue(const QString& key, const QString& value)
{
    for (auto& line : m_lines) {
        if (line.key == key) {
            line.raw = value;
            return;
        }
    }
    m_lines.append({ key, value });
}

QStringList ServerProperties::keys() const
{
    QStringList out;
    for (const auto& line : m_lines) {
        if (!line.key.isEmpty()) {
            out.append(line.key);
        }
    }
    return out;
}

// Minecraft writes properties with java.util.Properties, which escapes ':', '=', '\' and non-latin1 characters.
QString ServerProperties::escape(const QString& value)
{
    QString out;
    for (auto c : value) {
        if (c == '\\' || c == ':' || c == '=') {
            out += '\\';
            out += c;
        } else if (c.unicode() > 0x7e) {
            out += QString("\\u%1").arg(static_cast<int>(c.unicode()), 4, 16, QChar('0'));
        } else {
            out += c;
        }
    }
    return out;
}

QString ServerProperties::unescape(const QString& value)
{
    QString out;
    for (int i = 0; i < value.size(); i++) {
        auto c = value[i];
        if (c != '\\' || i + 1 >= value.size()) {
            out += c;
            continue;
        }
        auto next = value[++i];
        if (next == 'u' && i + 4 < value.size()) {
            bool ok = false;
            auto code = value.mid(i + 1, 4).toUShort(&ok, 16);
            if (ok) {
                out += QChar(code);
                i += 4;
                continue;
            }
        }
        switch (next.unicode()) {
            case 'n':
                out += '\n';
                break;
            case 't':
                out += '\t';
                break;
            default:
                out += next;
        }
    }
    return out;
}
