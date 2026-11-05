#pragma once

#include <KTextEditor/CodeCompletionModel>
#include <KTextEditor/CodeCompletionModelControllerInterface>

#include <QIcon>
#include <QJsonObject>
#include <QPointer>
#include <QVector>

#ifdef Q_MOC_RUN
// moc runs without include paths (see the Makefile): it has to see the interface here.
Q_DECLARE_INTERFACE(KTextEditor::CodeCompletionModelControllerInterface, "org.kde.KTextEditor.CodeCompletionModelControllerInterface")
#endif

namespace Lieve
{
class LspManager;

// Completion from the language server (textDocument/completion): asked when
// the editor starts a completion (while typing, after "." and the like, or
// Ctrl+Space), shown when the answer comes. Imports that a completion needs
// arrive afterwards (completionItem/resolve) and are added then.
class LspCompletionModel : public KTextEditor::CodeCompletionModel, public KTextEditor::CodeCompletionModelControllerInterface
{
    Q_OBJECT
    Q_INTERFACES(KTextEditor::CodeCompletionModelControllerInterface)
public:
    explicit LspCompletionModel(LspManager *manager);

    void completionInvoked(KTextEditor::View *view, const KTextEditor::Range &range, InvocationType invocationType) override;
    void executeCompletionItem(KTextEditor::View *view, const KTextEditor::Range &word, const QModelIndex &index) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QModelIndex index(int row, int column, const QModelIndex &parent = QModelIndex()) const override;
    int rowCount(const QModelIndex &parent = QModelIndex()) const override;

    bool shouldStartCompletion(KTextEditor::View *view, const QString &insertedText, bool userInsertion, const KTextEditor::Cursor &position) override;
    KTextEditor::Range completionRange(KTextEditor::View *view, const KTextEditor::Cursor &position) override;
    bool shouldAbortCompletion(KTextEditor::View *view, const KTextEditor::Range &range, const QString &currentCompletion) override;

    // For the automated checks: the labels of the last answer.
    QStringList labels() const;

private:
    struct Item {
        QString label;
        QString detail;
        QString insert; // text to insert
        KTextEditor::Range editRange = KTextEditor::Range::invalid(); // where the server wants it
        int kind = 0;
        QJsonObject raw; // for completionItem/resolve
    };
    void setItems(QVector<Item> items);

    LspManager *m_manager;
    QVector<Item> m_items;
    QPointer<KTextEditor::View> m_view;
    int m_request = -1;
};
}
