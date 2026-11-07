#include "gitstaging.h"

#include "git.h"

#include <KActionCollection>
#include <KSelectAction>
#include <KTextEditor/Document>
#include <KTextEditor/Message>
#include <KTextEditor/View>

#include <QAction>
#include <QFileInfo>
#include <QMenu>

#include <algorithm>

using namespace Lieve;

namespace
{
// The file is in the middle of a merge: its index has no single version.
bool inConflict(const GitRepo *repo, const QString &path)
{
    const QList<GitEntry> &entries = repo->entries();
    return std::any_of(entries.cbegin(), entries.cend(), [&path](const GitEntry &e) {
        return e.group == GitEntry::Conflicts && e.path == path;
    });
}

KTextEditor::Cursor cursorAt(const QString &text, qsizetype offset)
{
    int line = 0;
    qsizetype lineStart = 0;
    for (qsizetype i = 0; i < offset && i < text.size(); ++i) {
        if (text.at(i) == QLatin1Char('\n')) {
            ++line;
            lineStart = i + 1;
        }
    }
    return KTextEditor::Cursor(line, int(offset - lineStart));
}

bool binary(const QByteArray &data)
{
    return data.left(8000).contains('\0');
}
}

GitStaging::GitStaging(GitRepo *repo, QObject *parent)
    : QObject(parent)
    , m_repo(repo)
{
}

QString GitStaging::relativePath(const KTextEditor::Document *doc) const
{
    const QString root = m_repo->root();
    const QString path = doc && doc->url().isLocalFile() ? doc->url().toLocalFile() : QString();
    return !root.isEmpty() && path.startsWith(root + QLatin1Char('/')) ? path.mid(root.size() + 1) : QString();
}

bool GitStaging::hasChanges(const QString &path) const
{
    const QList<GitEntry> &entries = m_repo->entries();
    return std::any_of(entries.cbegin(), entries.cend(), [&path](const GitEntry &e) {
        return e.group == GitEntry::Changes && e.path == path;
    });
}

QString GitStaging::commandText(Command command, bool selection)
{
    switch (command) {
    case Stage:
        return selection ? tr("Stage Selected Ranges") : tr("Stage Change");
    case Unstage:
        return selection ? tr("Unstage Selected Ranges") : tr("Unstage Change");
    case Revert:
        return selection ? tr("Revert Selected Ranges") : tr("Revert Change");
    }
    return {};
}

QIcon GitStaging::commandIcon(Command command)
{
    // The Source Control panel's icons for the same actions on whole files.
    switch (command) {
    case Stage:
        return QIcon::fromTheme(QStringLiteral("list-add"));
    case Unstage:
        return QIcon::fromTheme(QStringLiteral("list-remove"));
    case Revert:
        return QIcon::fromTheme(QStringLiteral("edit-undo"));
    }
    return {};
}

LineSelection GitStaging::selectionOf(KTextEditor::View *view, DiffSide side)
{
    LineSelection selection;
    selection.side = side;
    if (view->selection()) {
        const KTextEditor::Range range = view->selectionRange();
        selection.first = range.start().line();
        selection.last = range.end().line();
        if (range.end().column() == 0 && selection.last > selection.first) {
            --selection.last; // whole lines selected: the next line is not part of it
        }
        selection.wholeChanges = false;
    } else {
        selection.first = selection.last = view->cursorPosition().line();
        selection.wholeChanges = true;
    }
    return selection;
}

QStringList GitStaging::documentLines(const KTextEditor::Document *doc)
{
    return doc->text().split(QLatin1Char('\n'));
}

bool GitStaging::documentCrlf(KTextEditor::Document *doc)
{
    // The end of line menu of a view knows it (Windows = 1); else the document's setting.
    if (KTextEditor::View *view = doc->views().value(0)) {
        if (auto *action = qobject_cast<KSelectAction *>(view->actionCollection()->action(QStringLiteral("set_eol")))) {
            return action->currentItem() == 1;
        }
    }
    return doc->configValue(QStringLiteral("end-of-line")).toInt() == 1;
}

void GitStaging::showMessage(KTextEditor::Document *doc, const QString &text)
{
    if (!doc) {
        return;
    }
    auto *message = new KTextEditor::Message(QStringLiteral("<p>%1</p>").arg(text.toHtmlEscaped()), KTextEditor::Message::Information);
    message->setPosition(KTextEditor::Message::BottomInView);
    message->setAutoHide(4000);
    message->setAutoHideMode(KTextEditor::Message::Immediate);
    doc->postMessage(message);
}

void GitStaging::replaceText(KTextEditor::Document *doc, const QString &text)
{
    const QString old = doc->text();
    if (old == text) {
        return;
    }
    const qsizetype common = std::min(old.size(), text.size());
    qsizetype prefix = 0;
    while (prefix < common && old.at(prefix) == text.at(prefix)) {
        ++prefix;
    }
    if (prefix > 0 && old.at(prefix - 1).isHighSurrogate()) {
        --prefix;
    }
    qsizetype suffix = 0;
    while (suffix < common - prefix && old.at(old.size() - 1 - suffix) == text.at(text.size() - 1 - suffix)) {
        ++suffix;
    }
    if (suffix > 0 && old.at(old.size() - suffix).isLowSurrogate()) {
        --suffix;
    }
    const KTextEditor::Range range(cursorAt(old, prefix), cursorAt(old, old.size() - suffix));
    KTextEditor::Document::EditingTransaction transaction(doc);
    doc->replaceText(range, text.mid(prefix, text.size() - prefix - suffix));
}

void GitStaging::stage(KTextEditor::Document *doc, const QString &path, const LineSelection &selection)
{
    if (!doc || path.isEmpty()) {
        return;
    }
    if (inConflict(m_repo, path)) {
        showMessage(doc, tr("The file has merge conflicts: resolve them first."));
        return;
    }
    const QStringList lines = documentLines(doc);
    const QByteArray encoding = doc->encoding().toLatin1();
    const bool crlf = documentCrlf(doc);
    const bool executable = QFileInfo(m_repo->absolutePath(path)).isExecutable();
    const QPointer<KTextEditor::Document> target(doc);
    m_repo->rewriteIndex(
        path,
        QString(),
        [=](const GitIndexFile &file) -> std::optional<GitIndexWrite> {
            if (file.inIndex && (!file.mode.startsWith(QLatin1String("100")) || binary(file.data))) {
                showMessage(target, tr("Only part of a text file can be staged."));
                return std::nullopt;
            }
            const std::optional<QByteArray> content = stagedContent(file.inIndex ? file.data : QByteArray(), lines, encoding, crlf, selection);
            if (!content) {
                showMessage(target, tr("There are no changes to stage in the selection."));
                return std::nullopt;
            }
            GitIndexWrite write;
            write.data = *content;
            write.mode = file.inIndex ? file.mode : executable ? QStringLiteral("100755") : QStringLiteral("100644");
            return write;
        },
        tr("Couldn't stage the selected changes"));
}

void GitStaging::revert(KTextEditor::Document *doc, const QString &path, const LineSelection &selection)
{
    if (!doc || path.isEmpty() || !doc->isReadWrite()) {
        return;
    }
    if (inConflict(m_repo, path)) {
        showMessage(doc, tr("The file has merge conflicts: resolve them first."));
        return;
    }
    const qint64 revision = doc->revision();
    const QStringList lines = documentLines(doc);
    const QByteArray encoding = doc->encoding().toLatin1();
    const QPointer<KTextEditor::Document> target(doc);
    // A file that is not in the index (new) reverts to nothing.
    m_repo->readFile(QLatin1Char(':') + path, [=](bool found, const QByteArray &data) {
        if (!target || target->revision() != revision) {
            return; // edited meanwhile: the selection is not what it was
        }
        if (binary(data)) {
            return;
        }
        const std::optional<QString> text = revertedText(found ? data : QByteArray(), lines, encoding, selection);
        if (!text) {
            showMessage(target, tr("There are no changes to revert in the selection."));
            return;
        }
        replaceText(target, *text);
    });
}

void GitStaging::unstage(const QString &path, const QString &headPath, const LineSelection &selection, KTextEditor::Document *messages)
{
    if (path.isEmpty()) {
        return;
    }
    const QPointer<KTextEditor::Document> target(messages);
    m_repo->rewriteIndex(
        path,
        headPath.isEmpty() ? path : headPath,
        [=](const GitIndexFile &file) -> std::optional<GitIndexWrite> {
            if (!file.inIndex || !file.mode.startsWith(QLatin1String("100")) || binary(file.data)) {
                showMessage(target, tr("There are no staged changes to unstage in the selection."));
                return std::nullopt;
            }
            const std::optional<QByteArray> content = unstagedContent(file.inHead ? file.head : QByteArray(), file.data, selection);
            if (!content) {
                showMessage(target, tr("There are no staged changes to unstage in the selection."));
                return std::nullopt;
            }
            GitIndexWrite write;
            if (!file.inHead && content->isEmpty()) {
                write.remove = true; // a new file with nothing staged left: untracked again
            } else {
                write.data = *content;
                write.mode = file.mode;
            }
            return write;
        },
        tr("Couldn't unstage the selected changes"));
}

void GitStaging::run(Command command, KTextEditor::View *view)
{
    if (!view) {
        return;
    }
    KTextEditor::Document *doc = view->document();
    const QString path = relativePath(doc);
    if (path.isEmpty()) {
        showMessage(doc, tr("The file is not in the git repository."));
        return;
    }
    switch (command) {
    case Stage:
        stage(doc, path, selectionOf(view, DiffSide::Modified));
        break;
    case Revert:
        revert(doc, path, selectionOf(view, DiffSide::Modified));
        break;
    case Unstage:
        showMessage(doc, tr("Staged changes are unstaged in their diff: open the file from Staged Changes in the Source Control panel."));
        break;
    }
}

void GitStaging::addView(KTextEditor::View *view)
{
    connect(view, &KTextEditor::View::contextMenuAboutToShow, this, [this](KTextEditor::View *v, QMenu *menu) {
        if (!menu) {
            return;
        }
        auto *stageAction = menu->findChild<QAction *>(QStringLiteral("lieveStageRanges"), Qt::FindDirectChildrenOnly);
        auto *revertAction = menu->findChild<QAction *>(QStringLiteral("lieveRevertRanges"), Qt::FindDirectChildrenOnly);
        auto *separator = menu->findChild<QAction *>(QStringLiteral("lieveStagingSeparator"), Qt::FindDirectChildrenOnly);
        if (!stageAction) {
            separator = menu->addSeparator();
            separator->setObjectName(QStringLiteral("lieveStagingSeparator"));
            stageAction = new QAction(menu);
            stageAction->setObjectName(QStringLiteral("lieveStageRanges"));
            stageAction->setIcon(commandIcon(Stage));
            revertAction = new QAction(menu);
            revertAction->setObjectName(QStringLiteral("lieveRevertRanges"));
            revertAction->setIcon(commandIcon(Revert));
            menu->addAction(stageAction);
            menu->addAction(revertAction);
            connect(stageAction, &QAction::triggered, this, [this] {
                run(Stage, m_menuView);
            });
            connect(revertAction, &QAction::triggered, this, [this] {
                run(Revert, m_menuView);
            });
        }
        m_menuView = v;
        const QString path = relativePath(v->document());
        const bool shown = !path.isEmpty() && hasChanges(path);
        stageAction->setText(commandText(Stage, v->selection()));
        revertAction->setText(commandText(Revert, v->selection()));
        stageAction->setVisible(shown);
        revertAction->setVisible(shown && v->document()->isReadWrite());
        separator->setVisible(shown);
    });
}

void GitStaging::showChangeMenu(QWidget *parent, KTextEditor::Document *doc, int line, const QPoint &globalPos, const std::function<void()> &openChange)
{
    const QString path = relativePath(doc);
    if (path.isEmpty()) {
        return;
    }
    const QPointer<KTextEditor::Document> target(doc);
    LineSelection selection;
    selection.first = selection.last = line;
    selection.wholeChanges = true;
    auto *menu = new QMenu(parent);
    menu->setAttribute(Qt::WA_DeleteOnClose);
    menu->addAction(QIcon::fromTheme(QStringLiteral("view-split-left-right"), QIcon::fromTheme(QStringLiteral("document-preview"))),
                    tr("Open Changes"),
                    this,
                    openChange);
    menu->addSeparator();
    menu->addAction(commandIcon(Stage), commandText(Stage, false), this, [this, target, path, selection] {
        stage(target, path, selection);
    });
    QAction *revertAction = menu->addAction(commandIcon(Revert), commandText(Revert, false), this, [this, target, path, selection] {
        revert(target, path, selection);
    });
    revertAction->setEnabled(doc->isReadWrite());
    menu->popup(globalPos);
}
