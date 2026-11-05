#include "lspcompletion.h"

#include "lsp.h"
#include "lspclient.h"

#include <KTextEditor/Document>
#include <KTextEditor/View>

#include <QJsonArray>
#include <QRegularExpression>

#include <algorithm>

namespace Lieve
{
namespace
{
constexpr int kMaxItems = 2000;

bool isIdentifierChar(QChar c)
{
    return c.isLetterOrNumber() || c == QLatin1Char('_') || c == QLatin1Char('$');
}

KTextEditor::Range lspRange(const QJsonObject &range)
{
    const QJsonObject start = range.value(QLatin1String("start")).toObject();
    const QJsonObject end = range.value(QLatin1String("end")).toObject();
    return KTextEditor::Range(start.value(QLatin1String("line")).toInt(),
                              start.value(QLatin1String("character")).toInt(),
                              end.value(QLatin1String("line")).toInt(),
                              end.value(QLatin1String("character")).toInt());
}

// Servers are told there is no snippet support; strip what still comes.
QString plainSnippet(QString text)
{
    static const QRegularExpression placeholder(QStringLiteral("\\$\\{\\d+(?::([^}]*))?\\}"));
    static const QRegularExpression tabStop(QStringLiteral("\\$\\d+"));
    text.replace(placeholder, QStringLiteral("\\1"));
    text.remove(tabStop);
    return text;
}

QStringList triggerCharacters(LspClient *client)
{
    QStringList chars;
    const QJsonArray values =
        client->capabilities().value(QLatin1String("completionProvider")).toObject().value(QLatin1String("triggerCharacters")).toArray();
    for (const QJsonValue &v : values) {
        chars << v.toString();
    }
    return chars;
}
}

LspCompletionModel::LspCompletionModel(LspManager *manager)
    : KTextEditor::CodeCompletionModel(manager)
    , m_manager(manager)
{
}

QModelIndex LspCompletionModel::index(int row, int column, const QModelIndex &parent) const
{
    if (parent.isValid() || row < 0 || row >= m_items.size() || column < 0 || column >= ColumnCount) {
        return {};
    }
    return createIndex(row, column);
}

int LspCompletionModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : int(m_items.size());
}

QStringList LspCompletionModel::labels() const
{
    QStringList out;
    for (const Item &item : m_items) {
        out << item.label;
    }
    return out;
}

void LspCompletionModel::setItems(QVector<Item> items)
{
    beginResetModel();
    m_items = std::move(items);
    endResetModel();
}

void LspCompletionModel::completionInvoked(KTextEditor::View *view, const KTextEditor::Range &range, InvocationType invocationType)
{
    Q_UNUSED(range)
    m_view = view;
    KTextEditor::Document *doc = view->document();
    LspClient *client = m_manager->readyClient(doc);
    const int request = ++m_request;
    if (!client || !client->hasCapability(QStringLiteral("completionProvider"))) {
        setItems({});
        return;
    }
    m_manager->flush(doc);
    const KTextEditor::Cursor cursor = view->cursorPosition();
    QJsonObject params = m_manager->positionParams(doc, cursor);
    QJsonObject context{{QStringLiteral("triggerKind"), 1}};
    if (invocationType == AutomaticInvocation && cursor.column() > 0) {
        const QString before = doc->line(cursor.line()).mid(cursor.column() - 1, 1);
        if (triggerCharacters(client).contains(before)) {
            context = QJsonObject{{QStringLiteral("triggerKind"), 2}, {QStringLiteral("triggerCharacter"), before}};
        }
    }
    params.insert(QStringLiteral("context"), context);
    setItems({});
    Q_EMIT waitForReset(); // the list shows when the answer comes
    QPointer<KTextEditor::Document> document(doc);
    client->request(
        QStringLiteral("textDocument/completion"),
        params,
        this,
        [this, request](const QJsonValue &result, bool ok) {
            if (request != m_request) {
                return; // a newer completion took over
            }
            QVector<Item> items;
            const QJsonArray list = result.isArray() ? result.toArray() : result.toObject().value(QLatin1String("items")).toArray();
            QVector<QPair<QString, int>> order;
            items.reserve(list.size());
            for (const QJsonValue &value : list) {
                const QJsonObject o = value.toObject();
                Item item;
                item.label = o.value(QLatin1String("label")).toString();
                if (item.label.isEmpty()) {
                    continue;
                }
                const QJsonObject labelDetails = o.value(QLatin1String("labelDetails")).toObject();
                item.detail = o.value(QLatin1String("detail")).toString();
                if (item.detail.isEmpty()) {
                    item.detail = labelDetails.value(QLatin1String("description")).toString();
                }
                item.detail = item.detail.simplified();
                if (item.detail.size() > 60) {
                    item.detail = item.detail.left(59) + QStringLiteral("…");
                }
                const QJsonObject edit = o.value(QLatin1String("textEdit")).toObject();
                if (!edit.isEmpty()) {
                    item.insert = edit.value(QLatin1String("newText")).toString();
                    const QJsonObject r = edit.contains(QLatin1String("range")) ? edit.value(QLatin1String("range")).toObject()
                                                                               : edit.value(QLatin1String("insert")).toObject();
                    item.editRange = lspRange(r);
                } else {
                    item.insert = o.value(QLatin1String("insertText")).toString();
                }
                if (item.insert.isEmpty()) {
                    item.insert = item.label;
                }
                if (o.value(QLatin1String("insertTextFormat")).toInt() == 2) {
                    item.insert = plainSnippet(item.insert);
                }
                item.kind = o.value(QLatin1String("kind")).toInt();
                item.raw = o;
                const QString sortText = o.value(QLatin1String("sortText")).toString();
                order.append({sortText.isEmpty() ? item.label : sortText, int(items.size())});
                items.append(std::move(item));
            }
            Q_UNUSED(ok)
            std::stable_sort(order.begin(), order.end(), [](const auto &a, const auto &b) {
                return a.first < b.first;
            });
            QVector<Item> sorted;
            sorted.reserve(std::min<qsizetype>(order.size(), kMaxItems));
            for (const auto &o : std::as_const(order)) {
                if (sorted.size() >= kMaxItems) {
                    break;
                }
                sorted.append(std::move(items[o.second]));
            }
            setItems(std::move(sorted));
        },
        4000);
}

QVariant LspCompletionModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() >= m_items.size()) {
        return {};
    }
    const Item &item = m_items.at(index.row());
    switch (role) {
    case Qt::DisplayRole:
        if (index.column() == Name) {
            return item.label;
        }
        if (index.column() == Postfix) {
            return item.detail;
        }
        return {};
    case Qt::DecorationRole:
        if (index.column() == Icon) {
            return completionKindIcon(item.kind);
        }
        return {};
    case CompletionRole:
        return int(KTextEditor::CodeCompletionModel::NoProperty);
    case InheritanceDepth:
        return index.row(); // keeps the server's order
    default:
        return {};
    }
}

void LspCompletionModel::executeCompletionItem(KTextEditor::View *view, const KTextEditor::Range &word, const QModelIndex &index) const
{
    if (!index.isValid() || index.row() >= m_items.size()) {
        return;
    }
    const Item item = m_items.at(index.row());
    KTextEditor::Document *doc = view->document();
    KTextEditor::Range range = word;
    // The server may replace from an earlier point (e.g. "this.#x", import paths).
    if (item.editRange.isValid() && item.editRange.start().line() == word.start().line() && item.editRange.start() < word.start()) {
        range.setStart(item.editRange.start());
    }
    const qint64 revision = doc->revision();
    doc->lockRevision(revision); // the extra edits are placed against this text
    doc->replaceText(range, item.insert);

    const QJsonArray extra = item.raw.value(QLatin1String("additionalTextEdits")).toArray();
    LspClient *client = m_manager->readyClient(doc);
    const bool resolve = client
        && client->capabilities().value(QLatin1String("completionProvider")).toObject().value(QLatin1String("resolveProvider")).toBool();
    if (!extra.isEmpty() || !resolve) {
        m_manager->applyEdits(doc, extra, revision);
        return;
    }
    // Auto-imports: the server tells which import to add when asked about the item.
    LspManager *manager = m_manager;
    QPointer<KTextEditor::Document> document(doc);
    client->request(
        QStringLiteral("completionItem/resolve"),
        item.raw,
        doc,
        [manager, document, revision](const QJsonValue &result, bool) {
            if (document) {
                manager->applyEdits(document, result.toObject().value(QLatin1String("additionalTextEdits")).toArray(), revision);
            }
        },
        3000);
}

bool LspCompletionModel::shouldStartCompletion(KTextEditor::View *view, const QString &insertedText, bool userInsertion, const KTextEditor::Cursor &position)
{
    Q_UNUSED(position)
    LspClient *client = m_manager->readyClient(view->document());
    if (!client || insertedText.isEmpty() || !client->hasCapability(QStringLiteral("completionProvider"))) {
        return false;
    }
    const QChar last = insertedText.back();
    if (triggerCharacters(client).contains(QString(last))) {
        return true;
    }
    return userInsertion && isIdentifierChar(last);
}

KTextEditor::Range LspCompletionModel::completionRange(KTextEditor::View *view, const KTextEditor::Cursor &position)
{
    // The word around the cursor, "$" included (foo$ in RxJS code).
    const QString line = view->document()->line(position.line());
    int start = std::min<int>(position.column(), int(line.size()));
    while (start > 0 && isIdentifierChar(line.at(start - 1))) {
        --start;
    }
    int end = std::min<int>(position.column(), int(line.size()));
    while (end < line.size() && isIdentifierChar(line.at(end))) {
        ++end;
    }
    return KTextEditor::Range(position.line(), start, position.line(), end);
}

bool LspCompletionModel::shouldAbortCompletion(KTextEditor::View *view, const KTextEditor::Range &range, const QString &currentCompletion)
{
    const KTextEditor::Cursor cursor = view->cursorPosition();
    if (cursor < range.start() || cursor > range.end()) {
        return true;
    }
    return !std::all_of(currentCompletion.begin(), currentCompletion.end(), isIdentifierChar);
}
}
