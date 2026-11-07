#include "symbolactions.h"

#include "codelens.h"
#include "commandpalette.h"
#include "delegates.h"
#include "git.h"
#include "lsp.h"
#include "lspclient.h"
#include "mainwindow.h"
#include "roundedrows.h"
#include "settings.h"
#include "theme.h"
#include "vscodeicons.h"
#include "windowmanager.h"

#include <KActionCollection>
#include <KTextEditor/Document>
#include <KTextEditor/Editor>
#include <KTextEditor/Message>
#include <KTextEditor/View>

#include <QAction>
#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QGraphicsDropShadowEffect>
#include <QHash>
#include <QJsonArray>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QTimer>
#include <QTreeWidget>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <functional>
#include <memory>
#include <numeric>
#include <optional>

namespace Lieve
{
namespace
{
constexpr int kVisibleRows = 14;

// The Settings window's description, also written above the key.
const char kCodeLensComment[] = QT_TRANSLATE_NOOP("SettingsDialog", "Shows how many times classes, functions, methods and properties are used, at the end of their line. Click the count to list them.");

KTextEditor::Range lspRange(const QJsonObject &range)
{
    const QJsonObject start = range.value(QLatin1String("start")).toObject();
    const QJsonObject end = range.value(QLatin1String("end")).toObject();
    return KTextEditor::Range(start.value(QLatin1String("line")).toInt(),
                              start.value(QLatin1String("character")).toInt(),
                              end.value(QLatin1String("line")).toInt(),
                              end.value(QLatin1String("character")).toInt());
}

QString canonical(const QString &path)
{
    const QString c = QFileInfo(path).canonicalFilePath();
    return c.isEmpty() ? path : c;
}

QString localPath(const KTextEditor::Document *doc)
{
    return doc->url().isLocalFile() ? doc->url().toLocalFile() : QString();
}

// An open document of the file (in any window).
KTextEditor::Document *openDocument(const QString &path)
{
    const QString wanted = canonical(path);
    const auto docs = KTextEditor::Editor::instance()->documents();
    for (KTextEditor::Document *doc : docs) {
        if (doc->url().isLocalFile() && canonical(doc->url().toLocalFile()) == wanted) {
            return doc;
        }
    }
    return nullptr;
}

QFont editorFont(KTextEditor::View *view)
{
    return view->configValue(QStringLiteral("font")).value<QFont>();
}

// Where the line of `position` is, in `parent`'s coordinates (the cursor's
// line when that one is scrolled away, else the top of the view).
QRect lineRect(KTextEditor::View *view, KTextEditor::Cursor position, QWidget *parent)
{
    QPoint at = view->cursorToCoordinate(position);
    if (at.x() < 0) {
        at = view->cursorToCoordinate(view->cursorPosition());
    }
    if (at.x() < 0) {
        at = view->textAreaRect().topLeft() + QPoint(8, 0);
    }
    return QRect(view->mapTo(parent, at), QSize(1, QFontMetrics(editorFont(view)).height()));
}

void addShadow(QWidget *widget)
{
    auto *shadow = new QGraphicsDropShadowEffect(widget);
    shadow->setBlurRadius(28);
    shadow->setOffset(0, 6);
    shadow->setColor(QColor(0, 0, 0, 140));
    widget->setGraphicsEffect(shadow);
}

// The text of the files a list shows, read when one of their rows is first
// painted: from the editor when the file is open, else from disk once.
class LineSource
{
public:
    QString line(const QString &path, int line)
    {
        auto it = m_files.find(path);
        if (it == m_files.end()) {
            File file;
            file.doc = openDocument(path);
            file.open = file.doc != nullptr;
            it = m_files.insert(path, file);
        }
        if (it->open) {
            return it->doc ? it->doc->line(line) : QString();
        }
        if (!it->read) {
            it->lines = readTextLines(path);
            it->read = true;
        }
        return it->lines.value(line);
    }

private:
    struct File {
        QPointer<KTextEditor::Document> doc;
        bool open = false;
        bool read = false;
        QStringList lines;
    };
    QHash<QString, File> m_files;
};

// A line of the list: its text is found when it is first shown.
class ReferenceItem : public QTreeWidgetItem
{
public:
    ReferenceItem(QTreeWidgetItem *parent, std::shared_ptr<LineSource> source, const QString &path, const ReferenceLine &line)
        : QTreeWidgetItem(parent)
        , m_source(std::move(source))
        , m_path(path)
        , m_line(line)
    {
    }
    QVariant data(int column, int role) const override
    {
        if (column == 0 && (role == Qt::DisplayRole || role == RangesRole)) {
            if (!m_preview) {
                m_preview = previewLine(m_source->line(m_path, m_line.line), m_line.ranges);
            }
            if (role == Qt::DisplayRole) {
                return m_preview->text;
            }
            QVariantList ranges;
            for (const auto &[start, length] : m_preview->ranges) {
                ranges << QPoint(start, length);
            }
            return ranges;
        }
        return QTreeWidgetItem::data(column, role);
    }

private:
    std::shared_ptr<LineSource> m_source;
    QString m_path;
    ReferenceLine m_line;
    mutable std::optional<PreviewLine> m_preview;
};

// The search results' rows; the lines also show their number, dim, before the text.
class ReferenceDelegate : public FileItemDelegate
{
public:
    using FileItemDelegate::FileItemDelegate;
    int numberWidth = 0;

    void paint(QPainter *p, const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        if (index.data(KindRole).toInt() != MatchItem || numberWidth <= 0) {
            FileItemDelegate::paint(p, option, index);
            return;
        }
        const QWidget *widget = option.widget;
        QStyle *style = widget ? widget->style() : QApplication::style();
        if (!Theme::styled()) { // the platform's selection under the number too
            QStyleOptionViewItem full(option);
            initStyleOption(&full, index);
            full.text.clear();
            style->drawPrimitive(QStyle::PE_PanelItemViewItem, &full, p, widget);
        }
        QStyleOptionViewItem text(option);
        text.rect.setLeft(option.rect.left() + numberWidth);
        FileItemDelegate::paint(p, text, index);

        const bool selected = option.state & QStyle::State_Selected;
        const QPalette::ColorGroup group = (option.state & QStyle::State_Active) ? QPalette::Active : QPalette::Inactive;
        QColor dim = option.palette.color(group, selected && !Theme::styled() ? QPalette::HighlightedText : QPalette::Text);
        dim.setAlphaF(0.55f);
        const QFontMetrics fm(option.font);
        const QString number = QString::number(index.data(LineRole).toInt() + 1);
        const int baseline = option.rect.top() + (option.rect.height() - fm.height()) / 2 + fm.ascent();
        p->save();
        p->setFont(option.font);
        p->setPen(dim);
        p->drawText(option.rect.left() + numberWidth - 10 - fm.horizontalAdvance(number), baseline, number);
        p->restore();
    }
};
}

// The references of a symbol, in a card under it (Quick Open's look, the
// search results' rows).
class ReferencesPopup : public QFrame
{
public:
    explicit ReferencesPopup(QWidget *parent)
        : QFrame(parent)
    {
        setObjectName(QStringLiteral("overlay"));
        setFrameShape(QFrame::StyledPanel);
        addShadow(this);
        auto *layout = new QVBoxLayout(this);
        layout->setContentsMargins(6, 6, 6, 6);
        layout->setSpacing(4);
        m_title = new QLabel(this);
        m_title->setForegroundRole(QPalette::PlaceholderText);
        m_title->setContentsMargins(4, 0, 0, 0);
        m_title->setTextFormat(Qt::PlainText);
        m_title->setMinimumWidth(1);
        tree = new RoundedTree<QTreeWidget>(this);
        tree->setObjectName(QStringLiteral("referencesList"));
        tree->setHeaderHidden(true);
        tree->setColumnCount(1);
        tree->setUniformRowHeights(true);
        tree->setFrameShape(QFrame::NoFrame);
        tree->setIndentation(14);
        tree->setIconSize(QSize(16, 16));
        tree->setMouseTracking(true);
        tree->setExpandsOnDoubleClick(false);
        tree->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        m_delegate = new ReferenceDelegate(tree);
        tree->setItemDelegate(m_delegate);
        layout->addWidget(m_title);
        layout->addWidget(tree);
        tree->installEventFilter(this);
        parent->installEventFilter(this);
        connect(tree, &QTreeWidget::itemClicked, this, [this](QTreeWidgetItem *item) {
            activate(item);
        });
        connect(qApp, &QApplication::focusChanged, this, [this](QWidget *, QWidget *now) {
            if (isVisible() && (!now || !isAncestorOf(now))) {
                dismiss(false);
            }
        });
        hide();
    }

    // The files and their lines; the line at or after `from` of `fromPath` is selected.
    void fill(const QList<ReferenceFile> &files, const QString &root, const QString &fromPath, KTextEditor::Cursor from)
    {
        tree->clear();
        auto source = std::make_shared<LineSource>();
        QTreeWidgetItem *current = nullptr;
        QTreeWidgetItem *firstOfFile = nullptr;
        QTreeWidgetItem *first = nullptr;
        int maxLine = 0;
        for (const ReferenceFile &file : files) {
            const QString relative = !root.isEmpty() && file.path.startsWith(root + QLatin1Char('/')) ? QDir(root).relativeFilePath(file.path) : file.path;
            const QFileInfo info(relative);
            auto *fileItem = new QTreeWidgetItem(tree);
            fileItem->setText(0, info.fileName());
            fileItem->setIcon(0, VsCodeIcons::instance().fileIcon(info.fileName()));
            fileItem->setToolTip(0, relative);
            fileItem->setData(0, KindRole, FileItem);
            fileItem->setData(0, PathRole, file.path);
            fileItem->setData(0, DirRole, info.path() == QLatin1String(".") ? QString() : info.path());
            fileItem->setData(0, CountRole, file.count);
            for (const ReferenceLine &line : file.lines) {
                auto *item = new ReferenceItem(fileItem, source, file.path, line);
                item->setData(0, KindRole, MatchItem);
                item->setData(0, PathRole, file.path);
                item->setData(0, LineRole, line.line);
                item->setData(0, ColumnRole, line.ranges.isEmpty() ? 0 : line.ranges.first().first);
                item->setToolTip(0, QStringLiteral("%1:%2").arg(relative).arg(line.line + 1));
                maxLine = std::max(maxLine, line.line);
                first = first ? first : item;
                if (file.path == fromPath) {
                    firstOfFile = firstOfFile ? firstOfFile : item;
                    if (!current && line.line >= from.line()) {
                        current = item;
                    }
                }
            }
            fileItem->setExpanded(true);
        }
        m_delegate->numberWidth = QFontMetrics(tree->font()).horizontalAdvance(QString::number(maxLine + 1)) + 14;
        current = current ? current : firstOfFile ? firstOfFile : first;
        if (current) {
            tree->setCurrentItem(current);
        }
    }

    // Below the symbol's line `anchor`, or above it when there is no room.
    void popup(const QString &title, const QRect &anchor, int width)
    {
        QWidget *p = parentWidget();
        width = std::max(200, std::min(width, p->width() - 16));
        m_title->setText(fontMetrics().elidedText(title, Qt::ElideRight, width - 24));
        int rows = 0;
        for (int i = 0; i < tree->topLevelItemCount() && rows <= kVisibleRows; ++i) {
            QTreeWidgetItem *file = tree->topLevelItem(i);
            rows += 1 + (file->isExpanded() ? file->childCount() : 0);
        }
        rows = std::clamp(rows, 1, kVisibleRows);
        const int rowHeight = tree->sizeHintForRow(0) > 0 ? tree->sizeHintForRow(0) : fontMetrics().height() + 6;
        tree->setFixedHeight(rows * rowHeight + 4);
        adjustSize();
        const int height = sizeHint().height();
        int y = anchor.bottom() + 3;
        if (y + height > p->height() - 8) {
            y = std::max(8, anchor.top() - height - 3);
        }
        const int x = std::clamp(anchor.left() - 12, 8, std::max(8, p->width() - width - 8));
        setGeometry(x, y, width, height);
        show();
        raise();
        tree->setFocus();
        if (tree->currentItem()) {
            tree->scrollToItem(tree->currentItem(), QAbstractItemView::PositionAtCenter);
        }
    }

    void dismiss(bool cancelled)
    {
        if (isHidden()) {
            return;
        }
        hide();
        if (closed) {
            closed(cancelled);
        }
    }

    void activate(QTreeWidgetItem *item)
    {
        if (!item) {
            return;
        }
        if (item->data(0, KindRole).toInt() == FileItem) {
            item->setExpanded(!item->isExpanded());
            return;
        }
        const QString path = item->data(0, PathRole).toString();
        const KTextEditor::Cursor at(item->data(0, LineRole).toInt(), item->data(0, ColumnRole).toInt());
        dismiss(false);
        if (chosen) {
            chosen(path, at);
        }
    }

    QString title() const
    {
        return m_title->text();
    }

    QTreeWidget *tree = nullptr;
    std::function<void(const QString &, KTextEditor::Cursor)> chosen;
    std::function<void(bool cancelled)> closed;

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (watched == tree && event->type() == QEvent::KeyPress) {
            const int key = static_cast<QKeyEvent *>(event)->key();
            if (key == Qt::Key_Escape) {
                dismiss(true);
                return true;
            }
            if (key == Qt::Key_Return || key == Qt::Key_Enter) {
                activate(tree->currentItem());
                return true;
            }
        }
        if (watched == parentWidget() && event->type() == QEvent::Resize) {
            dismiss(false);
        }
        return QFrame::eventFilter(watched, event);
    }

private:
    QLabel *m_title = nullptr;
    ReferenceDelegate *m_delegate = nullptr;
};

// The new name, asked in a small box under the symbol like VS Code's.
class RenameBox : public QFrame
{
public:
    explicit RenameBox(QWidget *parent)
        : QFrame(parent)
    {
        setObjectName(QStringLiteral("overlay"));
        setFrameShape(QFrame::StyledPanel);
        addShadow(this);
        auto *layout = new QVBoxLayout(this);
        layout->setContentsMargins(4, 4, 4, 4);
        layout->setSpacing(3);
        edit = new QLineEdit(this);
        auto *hint = new QLabel(SymbolActions::tr("Enter to rename, Escape to cancel"), this);
        hint->setForegroundRole(QPalette::PlaceholderText);
        hint->setContentsMargins(2, 0, 0, 0);
        QFont small = hint->font();
        if (small.pointSizeF() > 0) {
            small.setPointSizeF(small.pointSizeF() * 0.85);
            hint->setFont(small);
        }
        layout->addWidget(edit);
        layout->addWidget(hint);
        edit->installEventFilter(this);
        parent->installEventFilter(this);
        connect(qApp, &QApplication::focusChanged, this, [this](QWidget *, QWidget *now) {
            if (isVisible() && (!now || !isAncestorOf(now))) {
                cancel(); // clicked elsewhere
            }
        });
        hide();
    }

    // Under the symbol whose top left is `symbol` (or above it), the text in
    // the editor's font, selected.
    void popup(const QString &text, const QFont &font, const QRect &symbol)
    {
        edit->setFont(font);
        edit->setText(text);
        edit->selectAll();
        QWidget *p = parentWidget();
        const int width = std::clamp(QFontMetrics(font).horizontalAdvance(text) + 80, 240, std::max(240, std::min(480, p->width() - 16)));
        adjustSize();
        const int height = sizeHint().height();
        // The name in the box starts where it does in the editor (frame, margin, the box's border and padding).
        const int x = std::clamp(symbol.left() - 14, 8, std::max(8, p->width() - width - 8));
        int y = symbol.bottom() + 3;
        if (y + height > p->height() - 8) {
            y = std::max(8, symbol.top() - height - 3);
        }
        setGeometry(x, y, width, height);
        m_closing = false;
        show();
        raise();
        edit->setFocus();
    }

    void accept()
    {
        if (m_closing) {
            return;
        }
        m_closing = true;
        hide();
        if (accepted) {
            accepted(edit->text());
        }
    }
    void cancel()
    {
        if (m_closing || isHidden()) {
            return;
        }
        m_closing = true;
        hide();
        if (cancelled) {
            cancelled();
        }
    }

    QLineEdit *edit = nullptr;
    std::function<void(const QString &)> accepted;
    std::function<void()> cancelled;

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (watched == edit && event->type() == QEvent::KeyPress) {
            const int key = static_cast<QKeyEvent *>(event)->key();
            if (key == Qt::Key_Escape) {
                cancel();
                return true;
            }
            if (key == Qt::Key_Return || key == Qt::Key_Enter) {
                accept();
                return true;
            }
        }
        if (watched == parentWidget() && event->type() == QEvent::Resize) {
            cancel();
        }
        return QFrame::eventFilter(watched, event);
    }

private:
    bool m_closing = false;
};

// ---------------------------------------------------------------------------

SymbolActions::SymbolActions(LspManager *lsp, MainWindow *window, QWidget *overlayParent)
    : QObject(lsp)
    , m_lsp(lsp)
    , m_window(window)
    , m_overlayParent(overlayParent)
{
    // The context menu's entries; the keys themselves are the window's
    // (Shift+F12) and the editor's (F2: in the Explorer it renames files).
    m_referencesMenu = new QAction(QCoreApplication::translate("Lieve::LspManager", "Go to References"), this);
    m_referencesMenu->setShortcut(QKeySequence(Qt::SHIFT | Qt::Key_F12));
    m_renameMenu = new QAction(tr("Rename Symbol"), this);
    m_renameMenu->setShortcut(QKeySequence(Qt::Key_F2));
    for (QAction *action : {m_referencesMenu, m_renameMenu}) {
        action->setShortcutContext(Qt::WidgetShortcut);
        action->setShortcutVisibleInContextMenu(true);
    }
    connect(m_referencesMenu, &QAction::triggered, this, [this] {
        const auto [view, cursor] = m_lsp->menuTarget();
        findReferences(view, cursor);
    });
    connect(m_renameMenu, &QAction::triggered, this, [this] {
        const auto [view, cursor] = m_lsp->menuTarget();
        renameSymbol(view, cursor);
    });

    // Command palette: View: Toggle Reference Counts
    auto *toggle = new QAction(tr("Toggle Reference Counts"), window);
    toggle->setObjectName(QStringLiteral("actionToggleReferenceCounts"));
    setCommandCategory(toggle, CommandCategory::View);
    connect(toggle, &QAction::triggered, this, &SymbolActions::toggleLenses);
    window->addAction(toggle);
    applySettings();
}

SymbolActions::~SymbolActions()
{
    delete m_lenses; // leaves the views while they live
    m_lenses = nullptr;
    QObject::disconnect(m_popupScroll);
    delete m_popup.data(); // their callbacks call this object
    delete m_renameBox.data();
}

QList<QAction *> SymbolActions::menuActions() const
{
    return {m_referencesMenu, m_renameMenu};
}

void SymbolActions::addView(KTextEditor::View *view)
{
    m_views.removeAll(nullptr);
    if (!view || m_views.contains(view)) {
        return;
    }
    m_views.append(view);
    // F2 only in the editor: the Explorer renames files with it.
    auto *rename = new QAction(tr("Rename Symbol"), view);
    rename->setShortcut(QKeySequence(Qt::Key_F2));
    rename->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    KActionCollection *collection = view->actionCollection();
    const QList<QAction *> actions = collection->actions();
    for (QAction *action : actions) { // none of the editor's own has it, but two would cancel each other
        if (action->shortcuts().contains(QKeySequence(Qt::Key_F2))) {
            QList<QKeySequence> keys = action->shortcuts();
            keys.removeAll(QKeySequence(Qt::Key_F2));
            action->setShortcuts(keys);
        }
    }
    collection->addAction(QStringLiteral("lieve_rename_symbol"), rename); // listed in the command palette
    view->addAction(rename);
    QPointer<KTextEditor::View> v(view);
    connect(rename, &QAction::triggered, this, [this, v] {
        if (v) {
            renameSymbol(v, v->cursorPosition());
        }
    });
    if (m_lenses) {
        m_lenses->addView(view);
    }
}

void SymbolActions::addDocument(KTextEditor::Document *doc)
{
    connect(doc, &KTextEditor::Document::documentSavedOrUploaded, this, [this] {
        if (m_lenses) {
            m_lenses->filesChanged(); // counts in the other files may change
        }
    });
}

void SymbolActions::removeDocument(KTextEditor::Document *doc)
{
    if (m_lenses) {
        m_lenses->removeDocument(doc);
    }
}

void SymbolActions::setCurrentView(KTextEditor::View *view)
{
    m_current = view;
    if (view && m_lensesOn && !m_lenses && m_lsp->readyClient(view->document())) {
        setLenses(true);
    }
    if (m_lenses) {
        m_lenses->schedule(view, 300);
    }
}

void SymbolActions::serverReady()
{
    if (m_lensesOn && !m_lenses) {
        for (const QPointer<KTextEditor::View> &view : std::as_const(m_views)) {
            if (view && view->isVisible() && m_lsp->readyClient(view->document())) {
                setLenses(true);
                break;
            }
        }
    }
    if (m_lenses) {
        m_lenses->scheduleAll(300);
    }
}

void SymbolActions::applySettings()
{
    m_lensesOn = config().value(QStringLiteral("lsp/codeLens"), true).toBool();
    if (!m_lensesOn) {
        setLenses(false);
    } else if (!m_lenses && m_current && m_lsp->readyClient(m_current->document())) {
        setLenses(true); // made when first needed otherwise
    }
}

void SymbolActions::setLenses(bool on)
{
    if (on == (m_lenses != nullptr)) {
        return;
    }
    if (!on) {
        delete m_lenses;
        m_lenses = nullptr;
        return;
    }
    m_lenses = new ReferenceLenses(this);
    m_views.removeAll(nullptr);
    for (const QPointer<KTextEditor::View> &view : std::as_const(m_views)) {
        m_lenses->addView(view);
    }
}

void SymbolActions::toggleLenses()
{
    setConfigValue(QStringLiteral("lsp"),
                   QStringLiteral("codeLens"),
                   m_lensesOn ? QStringLiteral("false") : QStringLiteral("true"),
                   QCoreApplication::translate("SettingsDialog", kCodeLensComment));
    const auto windows = WindowManager::instance().windows();
    for (MainWindow *window : windows) {
        window->applySettings();
    }
}

void SymbolActions::showMessage(KTextEditor::View *view, const QString &text, int ms)
{
    m_lastMessage = text;
    ++m_messages;
    if (qEnvironmentVariableIsSet("LIEVE_TEST_REFS")) {
        qInfo().noquote() << "refs-test message" << text;
    }
    if (!view) {
        return;
    }
    auto *message = new KTextEditor::Message(QStringLiteral("<p>%1</p>").arg(text.toHtmlEscaped()), KTextEditor::Message::Information);
    message->setPosition(KTextEditor::Message::BottomInView);
    message->setAutoHide(ms);
    message->setAutoHideMode(KTextEditor::Message::Immediate);
    message->setView(view);
    view->document()->postMessage(message);
}

LspClient *SymbolActions::serverOrMessage(KTextEditor::View *view, const QString &capability, const QString &needs)
{
    LspClient *client = m_lsp->serverFor(view->document());
    if (!client) {
        showMessage(view, needs);
        return nullptr;
    }
    if (client->state() == LspClient::State::Failed) {
        showMessage(view, QCoreApplication::translate("Lieve::LspManager", "The %1 language server stopped: %2").arg(client->spec().name, client->lastError()));
        return nullptr;
    }
    if (client->state() == LspClient::State::Running && !client->hasCapability(capability)) {
        showMessage(view, needs);
        return nullptr;
    }
    return client;
}

// ---------------------------------------------------------------------------
// Go to References

void SymbolActions::findReferences(KTextEditor::View *view, KTextEditor::Cursor position, bool includeDeclaration)
{
    if (!view || !position.isValid()) {
        return;
    }
    LspClient *client = serverOrMessage(view,
                                        QStringLiteral("referencesProvider"),
                                        tr("Go to References needs a language server for this file (see [lsp] in Settings, Ctrl+,)."));
    if (!client) {
        return;
    }
    KTextEditor::Document *doc = view->document();
    m_lsp->flush(doc);
    if (m_referencesRequest >= 0 && m_referencesClient) {
        m_referencesClient->cancel(m_referencesRequest);
    }
    QJsonObject params = m_lsp->positionParams(doc, position);
    params.insert(QStringLiteral("context"), QJsonObject{{QStringLiteral("includeDeclaration"), includeDeclaration}});
    const QString word = doc->wordAt(position);
    QPointer<KTextEditor::View> v(view);
    QPointer<LspClient> c(client);
    m_referencesClient = client;
    // Asked even while the server starts: it waits for the project to load.
    m_referencesRequest = client->request(
        QStringLiteral("textDocument/references"),
        params,
        view,
        [this, v, c, position, word, includeDeclaration](const QJsonValue &result, bool ok) {
            m_referencesRequest = -1;
            if (!v) {
                return;
            }
            if (!ok) {
                const QString error = c ? serverErrorText(c->requestError()) : QString();
                if (!error.isEmpty()) {
                    showMessage(v, error);
                }
                return; // cancelled or timed out
            }
            showReferences(v, position, word, parseLocations(result), includeDeclaration);
        },
        20000);
}

void SymbolActions::showReferences(KTextEditor::View *view,
                                   KTextEditor::Cursor position,
                                   const QString &word,
                                   const QList<ReferenceLocation> &found,
                                   bool jumpToOne)
{
    QHash<QString, QString> paths; // as the server says them -> canonical
    QList<ReferenceLocation> locations = found;
    for (ReferenceLocation &l : locations) {
        auto it = paths.find(l.path);
        if (it == paths.end()) {
            it = paths.insert(l.path, canonical(l.path));
        }
        l.path = *it;
    }
    if (qEnvironmentVariableIsSet("LIEVE_TEST_REFS")) {
        qInfo().noquote() << "refs-test references" << locations.size();
    }
    const QString from = canonical(localPath(view->document()));
    const QList<ReferenceFile> files = groupReferences(locations, from);
    int total = 0;
    for (const ReferenceFile &file : files) {
        total += file.count;
    }
    if (total == 0) {
        showMessage(view, word.isEmpty() ? tr("No references found.") : tr("No references found for “%1”.").arg(word));
        return;
    }
    if (total == 1 && jumpToOne) {
        const ReferenceLine &line = files.first().lines.first();
        m_lsp->jumpTo(files.first().path, KTextEditor::Cursor(line.line, line.ranges.first().first), true);
        return;
    }
    if (!m_popup) {
        m_popup = new ReferencesPopup(m_overlayParent);
    }
    QPointer<KTextEditor::View> v(view);
    m_popup->chosen = [this](const QString &path, KTextEditor::Cursor at) {
        m_lsp->jumpTo(path, at, true); // Alt+Left comes back
    };
    // Scrolling the editor away from the symbol closes the list.
    QObject::disconnect(m_popupScroll);
    m_popupScroll = connect(view, &KTextEditor::View::verticalScrollPositionChanged, m_popup, [this] {
        m_popup->dismiss(false);
    });
    m_popup->closed = [this, v](bool cancelled) {
        QObject::disconnect(m_popupScroll);
        if (cancelled && v) {
            v->setFocus();
        }
    };
    m_popup->fill(files, m_lsp->root(), from, position);
    const KTextEditor::Range symbol = view->document()->wordRangeAt(position);
    QString title;
    if (word.isEmpty()) {
        title = total == 1 ? ReferenceLenses::tr("1 reference") : ReferenceLenses::tr("%1 references").arg(total);
    } else {
        title = total == 1 ? tr("1 reference to “%1”").arg(word) : tr("%1 references to “%2”").arg(total).arg(word);
    }
    m_popup->popup(title,
                   lineRect(view, symbol.isValid() && !symbol.isEmpty() ? symbol.start() : position, m_overlayParent),
                   std::clamp(view->width() * 3 / 5, 440, 760));
}

// ---------------------------------------------------------------------------
// Rename Symbol

void SymbolActions::renameSymbol(KTextEditor::View *view, KTextEditor::Cursor position)
{
    if (!view || !position.isValid()) {
        return;
    }
    LspClient *client = serverOrMessage(view,
                                        QStringLiteral("renameProvider"),
                                        tr("Rename Symbol needs a language server for this file (see [lsp] in Settings, Ctrl+,)."));
    if (!client) {
        return;
    }
    KTextEditor::Document *doc = view->document();
    m_lsp->flush(doc);
    const bool prepare = client->state() == LspClient::State::Running
        && client->capabilities().value(QLatin1String("renameProvider")).toObject().value(QLatin1String("prepareProvider")).toBool();
    if (!prepare) { // the word at the cursor
        const KTextEditor::Range word = doc->wordRangeAt(position);
        if (!word.isValid() || word.isEmpty()) {
            showMessage(view, tr("This element cannot be renamed."));
            return;
        }
        showRenameBox(view, word, doc->text(word));
        return;
    }
    if (m_prepareRequest >= 0 && m_prepareClient) {
        m_prepareClient->cancel(m_prepareRequest);
    }
    const qint64 revision = doc->revision();
    QPointer<KTextEditor::View> v(view);
    QPointer<LspClient> c(client);
    m_prepareClient = client;
    m_prepareRequest = client->request(
        QStringLiteral("textDocument/prepareRename"),
        m_lsp->positionParams(doc, position),
        view,
        [this, v, c, position, revision](const QJsonValue &result, bool ok) {
            m_prepareRequest = -1;
            if (!v) {
                return;
            }
            if (!ok) {
                const QString error = c ? serverErrorText(c->requestError()) : QString();
                if (!error.isEmpty()) {
                    showMessage(v, error); // "You cannot rename this element."
                }
                return;
            }
            KTextEditor::Document *doc = v->document();
            if (doc->revision() != revision) {
                return; // typed meanwhile
            }
            // Range, {range, placeholder} or {defaultBehavior}
            KTextEditor::Range range = KTextEditor::Range::invalid();
            QString placeholder;
            const QJsonObject o = result.toObject();
            if (o.contains(QLatin1String("range"))) {
                range = lspRange(o.value(QLatin1String("range")).toObject());
                placeholder = o.value(QLatin1String("placeholder")).toString();
            } else if (o.contains(QLatin1String("start"))) {
                range = lspRange(o);
            } else if (o.value(QLatin1String("defaultBehavior")).toBool()) {
                range = doc->wordRangeAt(position);
            }
            if (!range.isValid() || range.isEmpty() || !doc->documentRange().contains(range)) {
                showMessage(v, tr("This element cannot be renamed."));
                return;
            }
            showRenameBox(v, range, placeholder.isEmpty() ? doc->text(range) : placeholder);
        },
        10000);
}

void SymbolActions::showRenameBox(KTextEditor::View *view, KTextEditor::Range range, const QString &placeholder)
{
    if (!m_renameBox) {
        m_renameBox = new RenameBox(m_overlayParent);
    }
    QPointer<KTextEditor::View> v(view);
    const KTextEditor::Cursor at = range.start();
    m_renameBox->accepted = [this, v, at, placeholder](const QString &text) {
        if (!v) {
            return;
        }
        v->setFocus();
        const QString name = text.trimmed();
        if (!name.isEmpty() && name != placeholder) {
            rename(v, at, name);
        }
    };
    m_renameBox->cancelled = [v] {
        if (v) {
            v->setFocus();
        }
    };
    m_renameBox->popup(placeholder, editorFont(view), lineRect(view, at, m_overlayParent));
}

void SymbolActions::rename(KTextEditor::View *view, KTextEditor::Cursor position, const QString &newName)
{
    LspClient *client = serverOrMessage(view,
                                        QStringLiteral("renameProvider"),
                                        tr("Rename Symbol needs a language server for this file (see [lsp] in Settings, Ctrl+,)."));
    if (!client) {
        return;
    }
    KTextEditor::Document *doc = view->document();
    m_lsp->flushAll(); // the server has the text of every open file...
    // ...whose revisions are kept: the edits follow what is typed meanwhile.
    struct Lock {
        QPointer<KTextEditor::Document> doc;
        qint64 revision = -1;
        bool valid = true; // not reloaded since
        QMetaObject::Connection reload;
    };
    auto locks = std::make_shared<QList<Lock>>();
    const QList<KTextEditor::Document *> open = m_lsp->openDocuments(client);
    for (KTextEditor::Document *d : open) {
        Lock lock;
        lock.doc = d;
        lock.revision = d->revision();
        d->lockRevision(lock.revision);
        locks->append(lock);
    }
    for (qsizetype i = 0; i < locks->size(); ++i) {
        (*locks)[i].reload = connect((*locks)[i].doc, &KTextEditor::Document::aboutToInvalidateMovingInterfaceContent, this, [locks, i] {
            (*locks)[i].valid = false; // its revisions are gone: never unlock them
        });
    }
    const auto release = [locks] {
        for (Lock &lock : *locks) {
            QObject::disconnect(lock.reload);
            if (lock.doc && lock.valid) {
                lock.doc->unlockRevision(lock.revision);
            }
        }
        locks->clear();
    };
    QJsonObject params = m_lsp->positionParams(doc, position);
    params.insert(QStringLiteral("newName"), newName);
    QPointer<KTextEditor::View> v(view);
    QPointer<LspClient> c(client);
    client->request(
        QStringLiteral("textDocument/rename"),
        params,
        this,
        [this, v, c, locks, release](const QJsonValue &result, bool ok) {
            KTextEditor::View *target = v ? v.data() : m_current.data();
            if (!ok) {
                release();
                const QString error = c ? serverErrorText(c->requestError()) : QString();
                showMessage(target, error.isEmpty() ? tr("Rename failed: the language server did not answer.") : error, 6000);
                return;
            }
            const WorkspaceEditPlan plan = parseWorkspaceEdit(result.toObject());
            if (plan.files.isEmpty() && plan.operations.isEmpty()) {
                release();
                showMessage(target, tr("This element cannot be renamed."));
                return;
            }
            QList<QPair<QPointer<KTextEditor::Document>, qint64>> known;
            for (const Lock &lock : std::as_const(*locks)) {
                if (lock.doc && lock.valid) {
                    known.append({lock.doc, lock.revision});
                }
            }
            const RenameOutcome outcome = apply(plan, known);
            release();
            if (outcome.wroteFiles) {
                if (auto *git = m_window->findChild<GitRepo *>()) {
                    git->scheduleRefresh();
                }
            }
            if (m_lenses && outcome.files > 0) {
                m_lenses->filesChanged();
            }
            showMessage(target, summary(outcome), outcome.problems.isEmpty() && outcome.notApplied.isEmpty() ? 4000 : 10000);
        },
        30000);
}

// Open files are changed in their editor as one undo step each, without
// saving them; the others on disk (UTF-8 text only), like Replace All.
SymbolActions::RenameOutcome SymbolActions::apply(const WorkspaceEditPlan &plan, const QList<QPair<QPointer<KTextEditor::Document>, qint64>> &locked)
{
    RenameOutcome out;
    const QString root = m_lsp->root();
    const auto shown = [&root](const QString &path) {
        return !root.isEmpty() && path.startsWith(root + QLatin1Char('/')) ? QDir(root).relativeFilePath(path) : path;
    };
    for (const FileTextEdits &file : plan.files) {
        const QString path = QUrl(file.uri).toLocalFile();
        const QString name = shown(canonical(path));
        if (path.isEmpty()) {
            continue;
        }
        if (KTextEditor::Document *doc = openDocument(path)) {
            if (!doc->isReadWrite()) {
                out.problems << QCoreApplication::translate("SearchPanel", "%1 is read-only.").arg(name);
                continue;
            }
            qint64 revision = -1;
            for (const auto &[d, r] : locked) {
                if (d == doc) {
                    revision = r;
                }
            }
            if (revision < 0 && doc->isModified()) { // the server read the file, not these changes
                out.problems << tr("%1 has unsaved changes.").arg(name);
                continue;
            }
            // From the bottom up; at one place, the last edit first.
            QList<int> order(file.edits.size());
            std::iota(order.begin(), order.end(), 0);
            std::sort(order.begin(), order.end(), [&file](int a, int b) {
                const LspPosition &pa = file.edits.at(a).start;
                const LspPosition &pb = file.edits.at(b).start;
                return pa != pb ? pb < pa : b < a;
            });
            {
                KTextEditor::Document::EditingTransaction transaction(doc);
                for (const int i : std::as_const(order)) {
                    const LspTextEdit &edit = file.edits.at(i);
                    KTextEditor::Range range(edit.start.line, edit.start.character, edit.end.line, edit.end.character);
                    if (revision >= 0) {
                        doc->transformRange(range, KTextEditor::MovingRange::DoNotExpand, KTextEditor::MovingRange::AllowEmpty, revision);
                    }
                    if (range.isValid()) {
                        doc->replaceText(range, edit.newText);
                    }
                }
            }
            out.occurrences += int(file.edits.size());
            ++out.files;
            continue;
        }
        QString error;
        switch (applyTextEditsToFile(path, file.edits, &error)) {
        case FileEditResult::Done:
            out.occurrences += int(file.edits.size());
            ++out.files;
            out.wroteFiles = true;
            break;
        case FileEditResult::Missing:
            out.problems << QCoreApplication::translate("SearchPanel", "%1 no longer exists.").arg(name);
            break;
        case FileEditResult::Unreadable:
            out.problems << QCoreApplication::translate("SearchPanel", "%1 could not be read.").arg(name);
            break;
        case FileEditResult::NotText:
            out.problems << QCoreApplication::translate("SearchPanel", "%1 is not a UTF-8 text file.").arg(name);
            break;
        case FileEditResult::Overlapping:
            out.problems << tr("%1: the changes overlap.").arg(name);
            break;
        case FileEditResult::ReadOnly:
            out.problems << QCoreApplication::translate("SearchPanel", "%1 is read-only.").arg(name);
            break;
        case FileEditResult::WriteFailed:
            out.problems << QCoreApplication::translate("SearchPanel", "%1 could not be written: %2").arg(name, error);
            break;
        }
    }
    for (const ResourceOperation &op : plan.operations) {
        const QString file = shown(QUrl(op.uri).toLocalFile());
        out.notApplied << (op.kind == QLatin1String("rename") ? QStringLiteral("%1 → %2").arg(file, shown(QUrl(op.newUri).toLocalFile())) : file);
    }
    return out;
}

QString SymbolActions::summary(const RenameOutcome &o) const
{
    QStringList parts;
    if (o.occurrences == 1) {
        parts << tr("Renamed 1 occurrence in 1 file.");
    } else if (o.occurrences > 0) {
        parts << (o.files == 1 ? tr("Renamed %1 occurrences in 1 file.").arg(o.occurrences)
                               : tr("Renamed %1 occurrences in %2 files.").arg(o.occurrences).arg(o.files));
    } else {
        parts << tr("Nothing was renamed.");
    }
    if (!o.problems.isEmpty()) {
        parts << tr("Not changed: %1").arg(o.problems.join(QLatin1Char(' ')));
    }
    if (!o.notApplied.isEmpty()) {
        parts << tr("Files to create, rename or delete were left as they are: %1").arg(o.notApplied.join(QStringLiteral(", ")));
    }
    return parts.join(QLatin1Char(' '));
}

// ---------------------------------------------------------------------------
// Automated checks (LIEVE_TEST_REFS), steps separated by '|', lines and
// columns 1-based, paths relative to the project:
//   open:<file>[:line[:col]]  cursor:<line>:<col>  ready[:ms]  where  back
//   refs[:line:col]  rows  key:<down|up|enter|escape|left|right|pagedown|home|end>  pick:<n>
//   f2  rename:<new name>  message  lens[:ms]  lensrefs:<line>[:n]  lensclick:<line>[:n]
//   lenshover:<line>[:n]  dump[:file]  disk:<file>  undo  save  edit:<line>:<col>:<text>
//   top:<line>  toggle  keys  shot:<name>  sleep:<ms>

void SymbolActions::runTest(const QString &script)
{
    auto steps = std::make_shared<QStringList>(script.split(QLatin1Char('|'), Qt::SkipEmptyParts));
    auto run = std::make_shared<std::function<void()>>();
    QPointer<SymbolActions> self(this);
    *run = [this, self, steps, run] {
        if (!self) {
            return;
        }
        if (steps->isEmpty()) {
            qInfo().noquote() << "refs-test done";
            return;
        }
        const QString step = steps->takeFirst().trimmed();
        const QStringList parts = step.split(QLatin1Char(':'));
        const QString name = parts.value(0);
        const auto next = [this, run](int ms) {
            QTimer::singleShot(ms, this, [run] {
                (*run)();
            });
        };
        // Every 100 ms until `done` (or `limit` ms), then `then(done)` and the next step.
        const auto poll = [this, next](const std::function<bool()> &done, int limit, const std::function<void(bool)> &then) {
            auto clock = std::make_shared<QElapsedTimer>();
            clock->start();
            auto tick = std::make_shared<std::function<void()>>();
            *tick = [this, done, limit, then, next, clock, tick] {
                const bool ok = done();
                if (ok || clock->elapsed() > limit) {
                    then(ok);
                    next(0);
                    QTimer::singleShot(0, this, [tick] {
                        *tick = nullptr; // breaks the cycle
                    });
                    return;
                }
                QTimer::singleShot(100, this, [tick] {
                    if (*tick) {
                        (*tick)();
                    }
                });
            };
            (*tick)();
        };
        const auto at = [&parts](int i) {
            return KTextEditor::Cursor(parts.value(i).toInt() - 1, std::max(0, parts.value(i + 1).toInt() - 1));
        };
        const QString root = m_lsp->root();
        const auto relative = [root](const QString &path) { // also used once this step is over
            return root.isEmpty() ? path : QDir(root).relativeFilePath(path);
        };
        KTextEditor::View *view = m_current;
        const auto where = [this, relative] {
            KTextEditor::View *v = m_current;
            return v ? QStringLiteral("%1 %2 %3").arg(relative(localPath(v->document()))).arg(v->cursorPosition().line() + 1).arg(v->cursorPosition().column() + 1)
                     : QStringLiteral("(no view)");
        };
        const auto popupShown = [this] {
            return m_popup && m_popup->isVisible();
        };
        // Waits for the list, a jump or a message after a references request.
        const auto awaitReferences = [this, poll, where, popupShown] {
            const int messages = m_messages;
            const QString from = where();
            poll(
                [this, messages, from, where, popupShown] {
                    return popupShown() || m_messages != messages || where() != from;
                },
                20000,
                [this, messages, where, popupShown](bool) {
                    if (popupShown()) {
                        qInfo().noquote() << "refs-test refs list" << m_popup->title();
                    } else if (m_messages != messages) {
                        qInfo().noquote() << "refs-test refs message" << m_lastMessage;
                    } else {
                        qInfo().noquote() << "refs-test refs jump" << where();
                    }
                });
        };
        const auto mouse = [](KTextEditor::View *v, QEvent::Type type, const QPoint &inView, Qt::MouseButton button) {
            QWidget *area = v->focusProxy();
            if (!area) {
                return;
            }
            const QPoint local = area->mapFrom(v, inView);
            QMouseEvent event(type, QPointF(local), QPointF(area->mapToGlobal(local)), button, type == QEvent::MouseButtonRelease ? Qt::NoButton : button,
                              Qt::NoModifier);
            QApplication::sendEvent(area, &event);
        };
        qInfo().noquote() << "refs-test step" << step;

        if (name == QLatin1String("sleep")) {
            next(parts.value(1).toInt());
            return;
        }
        if (name == QLatin1String("open")) {
            m_window->openFile(QDir(root).filePath(parts.value(1)), parts.value(2).toInt() - 1, std::max(0, parts.value(3).toInt() - 1));
            next(500);
            return;
        }
        if (name == QLatin1String("shot")) {
            const QString file = qEnvironmentVariable("LIEVE_SCREENSHOT");
            if (!file.isEmpty()) {
                m_window->grab().save(QString(file).replace(QStringLiteral(".png"), QStringLiteral("-%1.png").arg(parts.value(1))));
            }
            next(0);
            return;
        }
        if (name == QLatin1String("toggle")) {
            if (auto *action = m_window->findChild<QAction *>(QStringLiteral("actionToggleReferenceCounts"))) {
                action->trigger();
            }
            qInfo().noquote() << "refs-test lenses" << (m_lensesOn ? "on" : "off");
            next(500);
            return;
        }
        if (name == QLatin1String("key")) {
            static const QHash<QString, int> keys{
                {QStringLiteral("down"), Qt::Key_Down},         {QStringLiteral("up"), Qt::Key_Up},     {QStringLiteral("enter"), Qt::Key_Return},
                {QStringLiteral("escape"), Qt::Key_Escape},     {QStringLiteral("left"), Qt::Key_Left}, {QStringLiteral("right"), Qt::Key_Right},
                {QStringLiteral("pagedown"), Qt::Key_PageDown}, {QStringLiteral("home"), Qt::Key_Home}, {QStringLiteral("end"), Qt::Key_End},
            };
            QWidget *focus = QApplication::focusWidget();
            if (focus && keys.contains(parts.value(1))) {
                QKeyEvent press(QEvent::KeyPress, keys.value(parts.value(1)), Qt::NoModifier);
                QApplication::sendEvent(focus, &press);
                QKeyEvent release(QEvent::KeyRelease, keys.value(parts.value(1)), Qt::NoModifier);
                QApplication::sendEvent(focus, &release);
            }
            qInfo().noquote() << "refs-test key" << parts.value(1) << "to" << (focus ? focus->metaObject()->className() : "nothing") << "| list"
                              << (popupShown() ? "shown" : "hidden") << "| rename box" << (m_renameBox && m_renameBox->isVisible() ? "shown" : "hidden");
            next(400);
            return;
        }
        if (name == QLatin1String("message")) {
            qInfo().noquote() << "refs-test message" << m_lastMessage;
            next(0);
            return;
        }
        if (name == QLatin1String("rows")) {
            if (!popupShown()) {
                qInfo().noquote() << "refs-test rows: no list";
                next(0);
                return;
            }
            qInfo().noquote() << "refs-test title" << m_popup->title() << "| size" << m_popup->size() << "at" << m_popup->pos();
            QTreeWidget *tree = m_popup->tree;
            for (int i = 0; i < tree->topLevelItemCount(); ++i) {
                QTreeWidgetItem *file = tree->topLevelItem(i);
                qInfo().noquote() << "refs-test file" << file->text(0) << "|" << file->data(0, DirRole).toString() << "|" << file->data(0, CountRole).toInt()
                                  << (file == tree->currentItem() ? "(current)" : "");
                for (int j = 0; j < file->childCount(); ++j) {
                    QTreeWidgetItem *row = file->child(j);
                    QString text = row->data(0, Qt::DisplayRole).toString();
                    const QVariantList ranges = row->data(0, RangesRole).toList();
                    for (qsizetype k = ranges.size() - 1; k >= 0; --k) {
                        const QPoint r = ranges.at(k).toPoint();
                        text.insert(r.x() + r.y(), QStringLiteral("»"));
                        text.insert(r.x(), QStringLiteral("«"));
                    }
                    qInfo().noquote() << "refs-test line" << row->data(0, LineRole).toInt() + 1 << text << (row == tree->currentItem() ? "(current)" : "");
                }
            }
            next(0);
            return;
        }
        if (name == QLatin1String("pick")) {
            int n = 0;
            QTreeWidgetItem *chosen = nullptr;
            if (popupShown()) {
                QTreeWidget *tree = m_popup->tree;
                for (int i = 0; i < tree->topLevelItemCount() && !chosen; ++i) {
                    for (int j = 0; j < tree->topLevelItem(i)->childCount() && !chosen; ++j) {
                        if (++n == parts.value(1).toInt()) {
                            chosen = tree->topLevelItem(i)->child(j);
                        }
                    }
                }
            }
            qInfo().noquote() << "refs-test pick" << (chosen ? chosen->toolTip(0) : QStringLiteral("(none)"));
            if (chosen) {
                m_popup->tree->setCurrentItem(chosen);
                m_popup->activate(chosen);
            }
            next(800);
            return;
        }
        if (!view) {
            qInfo().noquote() << "refs-test no view";
            next(0);
            return;
        }
        KTextEditor::Document *doc = view->document();
        if (name == QLatin1String("cursor")) {
            view->setCursorPosition(at(1));
            next(200);
        } else if (name == QLatin1String("where")) {
            qInfo().noquote() << "refs-test where" << where();
            next(0);
        } else if (name == QLatin1String("back")) {
            m_lsp->navigate(-1);
            next(500);
        } else if (name == QLatin1String("top")) {
            view->setScrollPosition(KTextEditor::Cursor(parts.value(1).toInt() - 1, 0));
            next(300);
        } else if (name == QLatin1String("ready")) {
            // The server runs and has loaded the project (a hover is answered).
            QPointer<KTextEditor::View> v(view);
            auto answered = std::make_shared<bool>(false);
            auto asked = std::make_shared<bool>(false);
            auto clock = std::make_shared<QElapsedTimer>();
            clock->start();
            poll(
                [this, v, answered, asked] {
                    LspClient *client = v ? m_lsp->readyClient(v->document()) : nullptr;
                    if (client && !*asked) {
                        *asked = true;
                        client->request(QStringLiteral("textDocument/hover"), m_lsp->positionParams(v->document(), v->cursorPosition()), this, [answered](const QJsonValue &, bool) {
                            *answered = true;
                        }, 60000);
                    }
                    return *answered;
                },
                parts.value(1).toInt() > 0 ? parts.value(1).toInt() : 60000,
                [answered, clock](bool) {
                    qInfo().noquote() << "refs-test ready" << *answered << clock->elapsed() << "ms";
                });
        } else if (name == QLatin1String("refs")) {
            findReferences(view, parts.size() >= 3 ? at(1) : view->cursorPosition());
            awaitReferences();
        } else if (name == QLatin1String("lensrefs")) {
            if (m_lenses) {
                m_lenses->open(view, parts.value(1).toInt() - 1, parts.value(2).toInt());
            }
            awaitReferences();
        } else if (name == QLatin1String("f2") || name == QLatin1String("rename")) {
            // Through the editor's own F2 action.
            QAction *action = view->actionCollection()->action(QStringLiteral("lieve_rename_symbol"));
            const int messages = m_messages;
            if (action) {
                action->trigger();
            }
            const QString newName = parts.mid(1).join(QLatin1Char(':'));
            const bool rename = name == QLatin1String("rename");
            poll(
                [this, messages] {
                    return (m_renameBox && m_renameBox->isVisible()) || m_messages != messages;
                },
                10000,
                [this, messages, newName, rename, poll](bool) {
                    if (!m_renameBox || !m_renameBox->isVisible()) {
                        qInfo().noquote() << "refs-test rename no box:" << (m_messages != messages ? m_lastMessage : QStringLiteral("(nothing)"));
                        return;
                    }
                    QLineEdit *edit = m_renameBox->edit;
                    qInfo().noquote() << "refs-test rename box" << edit->text() << "| selected" << edit->selectedText() << "| focus"
                                      << (QApplication::focusWidget() == edit) << "| at" << m_renameBox->geometry();
                    if (!rename) {
                        return;
                    }
                    edit->setText(newName);
                    m_testMark = m_messages;
                    QKeyEvent press(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
                    QApplication::sendEvent(edit, &press);
                    // The summary comes as a message: "awaitmessage" waits for it.
                });
            if (rename) {
                steps->prepend(QStringLiteral("awaitmessage"));
            }
        } else if (name == QLatin1String("awaitmessage")) {
            poll(
                [this] {
                    return m_messages != m_testMark;
                },
                30000,
                [this](bool ok) {
                    qInfo().noquote() << "refs-test rename result" << (ok ? m_lastMessage : QStringLiteral("(no message)"));
                });
        } else if (name == QLatin1String("lens")) {
            if (!m_lenses) {
                qInfo().noquote() << "refs-test lens: off" << (m_lensesOn ? "(no server yet)" : "(setting)");
                next(0);
                return;
            }
            QPointer<KTextEditor::View> v(view);
            auto settled = std::make_shared<int>(0);
            poll(
                [this, settled] {
                    *settled = m_lenses && m_lenses->idle() ? *settled + 1 : 0;
                    return *settled >= 5; // idle for half a second
                },
                parts.value(1).toInt() > 0 ? parts.value(1).toInt() : 30000,
                [this, v](bool ok) {
                    if (!v || !m_lenses) {
                        return;
                    }
                    qInfo().noquote() << "refs-test lens idle" << ok << "lines" << v->firstDisplayedLine() + 1 << "-" << v->lastDisplayedLine() + 1;
                    const QList<ReferenceLenses::Shown> shown = m_lenses->shown(v);
                    for (const ReferenceLenses::Shown &s : shown) {
                        QStringList counts;
                        for (const int c : s.counts) {
                            counts << QString::number(c);
                        }
                        qInfo().noquote() << "refs-test lens" << s.line + 1 << counts.join(QLatin1Char(',')) << "|" << v->document()->line(s.line).trimmed()
                                          << "| blame yields" << ReferenceLenses::hasNote(v->document(), s.line);
                    }
                });
        } else if (name == QLatin1String("lensclick") || name == QLatin1String("lenshover")) {
            const QRect rect = m_lenses ? m_lenses->noteRect(view, parts.value(1).toInt() - 1, parts.value(2).toInt()) : QRect();
            qInfo().noquote() << "refs-test lens rect" << rect;
            if (rect.isEmpty()) {
                next(0);
                return;
            }
            mouse(view, QEvent::MouseMove, rect.center(), Qt::NoButton);
            mouse(view, QEvent::MouseMove, rect.center() + QPoint(1, 0), Qt::NoButton);
            if (QWidget *area = view->focusProxy()) {
                qInfo().noquote() << "refs-test pointer" << (area->cursor().shape() == Qt::PointingHandCursor ? "hand" : "other");
            }
            if (name == QLatin1String("lenshover")) {
                next(300);
                return;
            }
            mouse(view, QEvent::MouseButtonPress, rect.center(), Qt::LeftButton);
            mouse(view, QEvent::MouseButtonRelease, rect.center(), Qt::LeftButton);
            awaitReferences();
        } else if (name == QLatin1String("dump") || name == QLatin1String("disk")) {
            KTextEditor::Document *target = parts.size() > 1 ? openDocument(QDir(root).filePath(parts.value(1))) : doc;
            QStringList lines;
            if (name == QLatin1String("disk")) {
                QFile file(QDir(root).filePath(parts.value(1)));
                const QByteArray bytes = file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
                qInfo().noquote() << "refs-test disk" << parts.value(1) << bytes.size() << "bytes, BOM" << bytes.startsWith("\xEF\xBB\xBF") << "CRLF"
                                  << bytes.contains("\r\n");
                lines = readTextLines(file.fileName());
            } else if (target) {
                qInfo().noquote() << "refs-test dump" << relative(localPath(target)) << "modified" << target->isModified();
                for (int i = 0; i < target->lines(); ++i) {
                    lines << target->line(i);
                }
            } else {
                qInfo().noquote() << "refs-test dump: not open";
            }
            for (const QString &line : std::as_const(lines)) {
                qInfo().noquote() << "refs-test |" << line;
            }
            next(0);
        } else if (name == QLatin1String("undo")) {
            if (QAction *undo = view->actionCollection()->action(QStringLiteral("edit_undo"))) {
                undo->trigger();
            }
            next(300);
        } else if (name == QLatin1String("save")) {
            doc->documentSave();
            next(500);
        } else if (name == QLatin1String("edit")) {
            QString text = parts.mid(3).join(QLatin1Char(':'));
            text.replace(QStringLiteral("\\n"), QStringLiteral("\n"));
            doc->insertText(at(1), text);
            next(100);
        } else if (name == QLatin1String("keys")) {
            // Who holds F2 and Shift+F12: the editor's action and the window's.
            for (const QKeySequence &key : {QKeySequence(Qt::Key_F2), QKeySequence(Qt::SHIFT | Qt::Key_F12)}) {
                QStringList holders;
                const QList<QAction *> editor = view->actionCollection()->actions();
                for (QAction *a : editor) {
                    if (a->shortcuts().contains(key)) {
                        holders << QStringLiteral("editor:%1(%2)").arg(a->objectName()).arg(int(a->shortcutContext()));
                    }
                }
                const QList<QAction *> window = m_window->actions();
                for (QAction *a : window) {
                    if (a->shortcuts().contains(key)) {
                        holders << QStringLiteral("window:%1(%2)").arg(a->text()).arg(int(a->shortcutContext()));
                    }
                }
                qInfo().noquote() << "refs-test key" << key.toString() << holders.join(QStringLiteral(", "));
            }
            next(0);
        } else {
            qInfo().noquote() << "refs-test unknown step" << name;
            next(0);
        }
    };
    (*run)();
}
}
