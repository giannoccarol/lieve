#pragma once

#include <QHash>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QVector>

class QProcess;
namespace KTextEditor
{
class Document;
}

// Who last changed each line of a document, like GitLens. git blame runs once
// for the whole file, in the background; after that every line is answered
// from memory. Edits are followed as they happen: each line remembers which
// blamed line it comes from, and counts as committed while its text is still
// the same (so undoing an edit brings the blame back). git runs again only
// when the file is saved or reloaded, or HEAD moves.
class DocumentBlame : public QObject
{
    Q_OBJECT
public:
    struct Commit {
        QString hash;
        QString author;
        QString mail;
        QString summary;
        qint64 time = 0;
    };
    enum : int {
        Unknown = -1, // not blamed (yet): git is running, failed, or the file is not tracked
        Uncommitted = -2, // new or changed, in the working tree or in the editor
    };

    DocumentBlame(KTextEditor::Document *document, const QString &root, QObject *parent);
    ~DocumentBlame() override;

    // Index into commit(), Unknown or Uncommitted.
    int entry(int line) const;
    const Commit &commit(int index) const
    {
        return m_commits.at(index);
    }
    // Runs git blame on the current text (unsaved edits included).
    void refresh();

Q_SIGNALS:
    void updated();

private:
    // A blamed text and where each current line comes from in it.
    struct Layer {
        QVector<int> origin; // per current line: line in the blamed text, -1 = new
        QVector<size_t> hash; // per blamed line: hash of its text
        QVector<int> entry; // per blamed line: commit index, Unknown or Uncommitted
    };
    void lineWrapped(int line, int column);
    void lineUnwrapped(int line);
    void readOutput();
    void finished(int code);
    int commitIndex(const QString &hash);
    const Layer &answering() const;
    size_t lineHash(int line) const;

    QPointer<KTextEditor::Document> m_document;
    QString m_root;
    QVector<Commit> m_commits;
    QHash<QString, int> m_commitIndex;
    Layer m_shown; // the last finished blame
    Layer m_next; // the run in progress, followed through edits as well

    QProcess *m_process = nullptr;
    QByteArray m_buffer;
    int m_current = -1; // commit of the block being read
    int m_finalLine = 0;
    int m_count = 0;
};
