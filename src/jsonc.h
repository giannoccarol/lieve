#pragma once

#include <QByteArray>
#include <QChar>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>

// VS Code stores its files as JSONC: JSON with comments and trailing commas.
// QJsonDocument rejects both, so strip them first.
inline QByteArray stripJsonc(const QByteArray &in)
{
    QByteArray noComments;
    noComments.reserve(in.size());
    bool inString = false;
    for (qsizetype i = 0; i < in.size(); ++i) {
        const char c = in.at(i);
        if (inString) {
            noComments += c;
            if (c == '\\' && i + 1 < in.size()) {
                noComments += in.at(++i);
            } else if (c == '"') {
                inString = false;
            }
            continue;
        }
        if (c == '"') {
            inString = true;
            noComments += c;
        } else if (c == '/' && i + 1 < in.size() && in.at(i + 1) == '/') {
            while (i < in.size() && in.at(i) != '\n') {
                ++i;
            }
            noComments += '\n';
        } else if (c == '/' && i + 1 < in.size() && in.at(i + 1) == '*') {
            i += 2;
            while (i + 1 < in.size() && !(in.at(i) == '*' && in.at(i + 1) == '/')) {
                ++i;
            }
            ++i;
        } else {
            noComments += c;
        }
    }

    QByteArray out;
    out.reserve(noComments.size());
    inString = false;
    for (qsizetype i = 0; i < noComments.size(); ++i) {
        const char c = noComments.at(i);
        if (inString) {
            out += c;
            if (c == '\\' && i + 1 < noComments.size()) {
                out += noComments.at(++i);
            } else if (c == '"') {
                inString = false;
            }
            continue;
        }
        if (c == '"') {
            inString = true;
        } else if (c == ',') {
            qsizetype j = i + 1;
            while (j < noComments.size() && QChar::isSpace(uchar(noComments.at(j)))) {
                ++j;
            }
            if (j < noComments.size() && (noComments.at(j) == '}' || noComments.at(j) == ']')) {
                continue;
            }
        }
        out += c;
    }
    return out;
}

inline QJsonDocument readJsoncDocument(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        return {};
    }
    return QJsonDocument::fromJson(stripJsonc(f.readAll()));
}

inline QJsonObject readJsonc(const QString &path)
{
    return readJsoncDocument(path).object();
}
