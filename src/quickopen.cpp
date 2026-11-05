#include "quickopen.h"

#include "delegates.h"
#include "fuzzy.h"
#include "settings.h"
#include "vscodeicons.h"

#include <QApplication>
#include <QDir>
#include <QFileInfo>
#include <QGraphicsDropShadowEffect>
#include <QKeyEvent>
#include <QLineEdit>
#include <QListWidget>
#include <QProcess>
#include <QRegularExpression>
#include <QVBoxLayout>

#include <algorithm>
#include <climits>

using namespace Lieve;

namespace
{
constexpr int kMaxShown = 60;
constexpr int kVisibleRows = 12;

}

QuickOpen::QuickOpen(QWidget *parent)
    : QFrame(parent)
{
    setObjectName(QStringLiteral("overlay"));
    setFrameShape(QFrame::StyledPanel);
    auto *shadow = new QGraphicsDropShadowEffect(this);
    shadow->setBlurRadius(28);
    shadow->setOffset(0, 6);
    shadow->setColor(QColor(0, 0, 0, 140));
    setGraphicsEffect(shadow);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(6, 6, 6, 6);
    layout->setSpacing(4);
    m_edit = new QLineEdit(this);
    m_edit->setPlaceholderText(QStringLiteral("Cerca file per nome (aggiungi :riga per andare a una riga)"));
    m_list = new QListWidget(this);
    m_list->setFrameShape(QFrame::NoFrame);
    m_list->setUniformItemSizes(true);
    m_list->setIconSize(QSize(16, 16));
    m_list->setItemDelegate(new FileItemDelegate(m_list));
    m_list->setFocusPolicy(Qt::NoFocus);
    m_list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    layout->addWidget(m_edit);
    layout->addWidget(m_list);

    m_edit->installEventFilter(this);
    parent->installEventFilter(this);
    connect(m_edit, &QLineEdit::textChanged, this, &QuickOpen::refilter);
    connect(m_list, &QListWidget::itemClicked, this, &QuickOpen::accept);
    connect(qApp, &QApplication::focusChanged, this, [this](QWidget *, QWidget *now) {
        if (isVisible() && (!now || !isAncestorOf(now))) {
            hide();
        }
    });
    hide();
}

void QuickOpen::setRoot(const QString &dir)
{
    m_root = dir;
    m_files.clear();
    m_filesLower.clear();
}

void QuickOpen::popup(const QStringList &recentFiles)
{
    m_recent = recentFiles;
    m_edit->blockSignals(true);
    m_edit->clear();
    m_edit->blockSignals(false);
    refilter();
    show();
    raise();
    m_edit->setFocus();
    reloadFiles();
}

void QuickOpen::reloadFiles()
{
    if (m_root.isEmpty() || m_proc) {
        return;
    }
    QStringList args{QStringLiteral("--files"), QStringLiteral("--hidden"), QStringLiteral("--glob"), QStringLiteral("!.git")};
    const QStringList excluded = config()
                                     .value(QStringLiteral("search/exclude"),
                                            QStringList{QStringLiteral("**/node_modules"), QStringLiteral("**/bower_components")})
                                     .toStringList();
    for (const QString &e : excluded) {
        if (!e.trimmed().isEmpty()) {
            args << QStringLiteral("--glob") << QLatin1Char('!') + e.trimmed();
        }
    }
    args << QStringLiteral(".");

    m_proc = new QProcess(this);
    m_proc->setWorkingDirectory(m_root);
    m_proc->setStandardInputFile(QProcess::nullDevice());
    const QString root = m_root;
    connect(m_proc, &QProcess::finished, this, [this, root] {
        const QList<QByteArray> lines = m_proc->readAllStandardOutput().split('\n');
        m_proc->deleteLater();
        m_proc = nullptr;
        if (root != m_root) {
            return; // folder changed meanwhile
        }
        QStringList files;
        files.reserve(lines.size());
        for (const QByteArray &l : lines) {
            if (l.isEmpty()) {
                continue;
            }
            QString f = QString::fromUtf8(l);
            if (f.startsWith(QLatin1String("./"))) {
                f = f.mid(2);
            }
            files << f;
        }
        std::sort(files.begin(), files.end(), [](const QString &a, const QString &b) {
            return a.size() != b.size() ? a.size() < b.size() : a < b;
        });
        m_files = files;
        m_filesLower.clear();
        m_filesLower.reserve(files.size());
        for (const QString &f : std::as_const(m_files)) {
            m_filesLower << f.toLower();
        }
        if (isVisible()) {
            refilter();
        }
    });
    connect(m_proc, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart && m_proc) {
            m_proc->deleteLater();
            m_proc = nullptr;
        }
    });
    m_proc->start(QStringLiteral("rg"), args);
}

void QuickOpen::refilter()
{
    QString query = m_edit->text().trimmed();
    m_line = -1;
    static const QRegularExpression lineSuffix(QStringLiteral("^(.*?):(\\d+)$"));
    if (const auto match = lineSuffix.match(query); match.hasMatch()) {
        query = match.captured(1);
        m_line = match.captured(2).toInt() - 1;
    }
    query.remove(QLatin1Char(' '));
    const QString lowerQuery = query.toLower();

    QStringList chosen;
    if (lowerQuery.isEmpty()) {
        // Recently opened first, like VS Code.
        for (const QString &abs : std::as_const(m_recent)) {
            if (!m_root.isEmpty() && abs.startsWith(m_root + QLatin1Char('/'))) {
                chosen << abs.mid(m_root.size() + 1);
            }
        }
        for (const QString &f : std::as_const(m_files)) {
            if (chosen.size() >= kMaxShown) {
                break;
            }
            if (!chosen.contains(f)) {
                chosen << f;
            }
        }
    } else {
        QList<QPair<int, int>> scored; // score, index
        for (int i = 0; i < m_files.size(); ++i) {
            const int s = fuzzyScore(m_files.at(i), m_filesLower.at(i), lowerQuery);
            if (s != INT_MIN) {
                scored.append({s, i});
            }
        }
        const auto middle = scored.begin() + std::min<qsizetype>(kMaxShown, scored.size());
        std::partial_sort(scored.begin(), middle, scored.end(), [](const auto &a, const auto &b) {
            return a.first > b.first;
        });
        for (auto it = scored.begin(); it != middle; ++it) {
            chosen << m_files.at(it->second);
        }
    }

    m_list->clear();
    for (const QString &rel : std::as_const(chosen)) {
        const QFileInfo fi(rel);
        auto *item = new QListWidgetItem(VsCodeIcons::instance().fileIcon(fi.fileName()), fi.fileName(), m_list);
        item->setData(KindRole, FileItem);
        item->setData(PathRole, QDir(m_root).filePath(rel));
        item->setData(DirRole, fi.path() == QLatin1String(".") ? QString() : fi.path());
        item->setToolTip(rel);
    }
    if (m_list->count() > 0) {
        m_list->setCurrentRow(0);
    }
    place();
}

void QuickOpen::accept()
{
    QListWidgetItem *item = m_list->currentItem();
    if (!item) {
        return;
    }
    const QString path = item->data(PathRole).toString();
    const int line = m_line;
    hide();
    Q_EMIT fileChosen(path, line);
}

void QuickOpen::place()
{
    QWidget *p = parentWidget();
    const int width = std::min(680, p->width() - 40);
    const int rowHeight = m_list->sizeHintForRow(0) > 0 ? m_list->sizeHintForRow(0) : fontMetrics().height() + 8;
    const int rows = std::min<int>(kVisibleRows, m_list->count());
    m_list->setFixedHeight(rows * rowHeight + (rows > 0 ? 4 : 0));
    m_list->setVisible(rows > 0);
    adjustSize();
    setGeometry((p->width() - width) / 2, 8, width, sizeHint().height());
}

bool QuickOpen::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == parentWidget() && event->type() == QEvent::Resize && isVisible()) {
        place();
    }
    if (watched == m_edit && event->type() == QEvent::KeyPress) {
        auto *key = static_cast<QKeyEvent *>(event);
        const int row = m_list->currentRow();
        switch (key->key()) {
        case Qt::Key_Escape:
            hide();
            return true;
        case Qt::Key_Return:
        case Qt::Key_Enter:
            accept();
            return true;
        case Qt::Key_Down:
            m_list->setCurrentRow(std::min(row + 1, m_list->count() - 1));
            return true;
        case Qt::Key_Up:
            m_list->setCurrentRow(std::max(row - 1, 0));
            return true;
        case Qt::Key_PageDown:
            m_list->setCurrentRow(std::min(row + kVisibleRows, m_list->count() - 1));
            return true;
        case Qt::Key_PageUp:
            m_list->setCurrentRow(std::max(row - kVisibleRows, 0));
            return true;
        default:
            break;
        }
    }
    return QFrame::eventFilter(watched, event);
}
