#include "blame.h"

#include <KTextEditor/Document>

#include <QDir>
#include <QProcess>

DocumentBlame::DocumentBlame(KTextEditor::Document *document, const QString &root, QObject *parent)
    : QObject(parent)
    , m_document(document)
    , m_root(root)
{
    // KTextEditor reports every change as elementary steps; only splitting and
    // joining lines move lines around. Typing inside a line needs no work: the
    // line's text is compared with the blamed one when asked.
    connect(document, &KTextEditor::Document::lineWrapped, this, [this](KTextEditor::Document *, KTextEditor::Cursor position) {
        lineWrapped(position.line(), position.column());
    });
    connect(document, &KTextEditor::Document::lineUnwrapped, this, [this](KTextEditor::Document *, int line) {
        lineUnwrapped(line);
    });
    connect(document, &KTextEditor::Document::reloaded, this, &DocumentBlame::refresh);
}

DocumentBlame::~DocumentBlame()
{
    if (m_process) {
        m_process->disconnect(this);
        m_process->kill();
        m_process->waitForFinished(500);
    }
}

size_t DocumentBlame::lineHash(int line) const
{
    return qHash(m_document->line(line));
}

const DocumentBlame::Layer &DocumentBlame::answering() const
{
    // Until the first run is over, answer from it as git streams lines in.
    return m_shown.origin.isEmpty() && m_process ? m_next : m_shown;
}

int DocumentBlame::entry(int line) const
{
    const Layer &layer = answering();
    if (!m_document || line < 0 || line >= layer.origin.size()) {
        return Unknown;
    }
    const int origin = layer.origin.at(line);
    if (origin < 0) {
        return Uncommitted;
    }
    const int value = layer.entry.at(origin);
    if (value == Unknown || layer.hash.at(origin) == lineHash(line)) {
        return value;
    }
    return Uncommitted; // edited since
}

void DocumentBlame::refresh()
{
    if (!m_document) {
        return;
    }
    if (m_process) {
        m_process->disconnect(this);
        m_process->kill();
        m_process->deleteLater();
        m_process = nullptr;
    }
    const QString path = m_document->url().toLocalFile();
    if (path.isEmpty() || !path.startsWith(m_root + QLatin1Char('/'))) {
        return;
    }
    const int lines = m_document->lines();
    m_next = Layer();
    m_next.origin.resize(lines);
    m_next.hash.resize(lines);
    m_next.entry = QVector<int>(lines, Unknown);
    for (int i = 0; i < lines; ++i) {
        m_next.origin[i] = i;
        m_next.hash[i] = lineHash(i);
    }
    m_buffer.clear();
    m_current = -1;

    m_process = new QProcess(this);
    m_process->setWorkingDirectory(m_root);
    connect(m_process, &QProcess::readyReadStandardOutput, this, &DocumentBlame::readOutput);
    connect(m_process, &QProcess::finished, this, &DocumentBlame::finished);
    // --incremental streams lines as git finds them, the newest first.
    m_process->start(QStringLiteral("git"),
                     {QStringLiteral("blame"),
                      QStringLiteral("--incremental"),
                      QStringLiteral("--contents"),
                      QStringLiteral("-"),
                      QStringLiteral("--"),
                      QDir(m_root).relativeFilePath(path)});
    m_process->write(m_document->text().toUtf8());
    m_process->closeWriteChannel();
}

int DocumentBlame::commitIndex(const QString &hash)
{
    auto it = m_commitIndex.constFind(hash);
    if (it != m_commitIndex.constEnd()) {
        return it.value();
    }
    m_commits.append({hash, {}, {}, {}, 0});
    m_commitIndex.insert(hash, int(m_commits.size() - 1));
    return int(m_commits.size() - 1);
}

void DocumentBlame::readOutput()
{
    m_buffer += m_process->readAllStandardOutput();
    bool changed = false;
    qsizetype start = 0;
    for (qsizetype end = m_buffer.indexOf('\n'); end >= 0; start = end + 1, end = m_buffer.indexOf('\n', start)) {
        const QByteArray line = m_buffer.mid(start, end - start);
        if (m_current < 0) {
            // "<hash> <original line> <final line> <count>" starts a block
            const QList<QByteArray> parts = line.split(' ');
            if (parts.size() == 4 && parts.at(0).size() >= 40) {
                m_current = commitIndex(QString::fromLatin1(parts.at(0)));
                m_finalLine = parts.at(2).toInt() - 1;
                m_count = parts.at(3).toInt();
            }
            continue;
        }
        Commit &c = m_commits[m_current];
        if (line.startsWith("author ")) {
            c.author = QString::fromUtf8(line.mid(7));
        } else if (line.startsWith("author-mail ")) {
            c.mail = QString::fromUtf8(line.mid(12)).remove(QLatin1Char('<')).remove(QLatin1Char('>'));
        } else if (line.startsWith("author-time ")) {
            c.time = line.mid(12).toLongLong();
        } else if (line.startsWith("summary ")) {
            c.summary = QString::fromUtf8(line.mid(8));
        } else if (line.startsWith("filename ")) { // ends the block
            const bool uncommitted = c.hash.count(QLatin1Char('0')) == c.hash.size();
            const int value = uncommitted ? Uncommitted : m_current;
            for (int i = qMax(0, m_finalLine); i < m_finalLine + m_count && i < m_next.entry.size(); ++i) {
                m_next.entry[i] = value;
            }
            changed = true;
            m_current = -1;
        }
    }
    m_buffer.remove(0, start);
    if (changed && &answering() == &m_next) {
        Q_EMIT updated();
    }
}

void DocumentBlame::finished(int code)
{
    readOutput();
    m_process->deleteLater();
    m_process = nullptr;
    if (code == 0) {
        m_shown = std::move(m_next);
    } // else: untracked file, binary, git error: keep what was there (if anything)
    m_next = Layer();
    Q_EMIT updated();
}

void DocumentBlame::lineWrapped(int line, int column)
{
    // The line was split at `column`: its rest is now line + 1. The blamed line
    // goes with the half that still has its text, or with the rest when the
    // split was at the start (Enter before the text).
    for (Layer *layer : {&m_shown, &m_next}) {
        if (line < 0 || line >= layer->origin.size()) {
            continue;
        }
        const int origin = layer->origin.at(line);
        layer->origin.insert(line + 1, -1);
        if (origin < 0) {
            continue;
        }
        const size_t blamed = layer->hash.at(origin);
        const bool restMatches = lineHash(line + 1) == blamed;
        if (lineHash(line) != blamed && (restMatches || column == 0)) {
            layer->origin[line] = -1;
            layer->origin[line + 1] = origin;
        }
    }
}

void DocumentBlame::lineUnwrapped(int line)
{
    // `line` was appended to line - 1 and removed. Keep the blamed line whose
    // text the joined line has (e.g. an emptied line followed by an intact one).
    for (Layer *layer : {&m_shown, &m_next}) {
        if (line <= 0 || line >= layer->origin.size()) {
            continue;
        }
        const int above = layer->origin.at(line - 1);
        const int below = layer->origin.at(line);
        const size_t joined = lineHash(line - 1);
        if (below >= 0 && layer->hash.at(below) == joined) {
            layer->origin[line - 1] = below;
        } else if (above < 0 || layer->hash.at(above) != joined) {
            layer->origin[line - 1] = above >= 0 ? above : below;
        }
        layer->origin.remove(line);
    }
}
