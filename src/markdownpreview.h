#pragma once

#include <QFont>
#include <QHash>
#include <QImage>
#include <QPointer>
#include <QTextBrowser>
#include <QTimer>

#include <vector>

namespace KTextEditor
{
class Document;
class View;
}

// Markdown rendered by Qt's own Markdown support (GitHub dialect) in a
// read-only text browser, styled like VS Code's preview with the window's
// colours. No web engine: what CSS cannot do in a QTextDocument (rounded code
// backgrounds, quote bars, heading rules) is painted under the text.
class MarkdownView : public QTextBrowser
{
    Q_OBJECT
public:
    explicit MarkdownView(QWidget *parent = nullptr);

    // Renders `markdown` (relative links and images resolved against
    // `baseDir`), keeping the scroll position.
    void setMarkdownText(const QString &markdown, const QString &baseDir);
    void setCodeFont(const QFont &font);
    // Scrolls to about where line `line` of the source is, by the headings
    // around it (or the line's proportion when they cannot be matched).
    void scrollToSourceLine(int line, int lineCount);

Q_SIGNALS:
    void openFileRequested(const QString &path);

protected:
    QVariant loadResource(int type, const QUrl &name) override;
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    QMimeData *createMimeDataFromSelection() const override;

private:
    struct Decoration {
        enum Kind {
            TableStripe,
            CodeBlock,
            InlineCode,
            QuoteBar,
            Rule,
            TaskOpen,
            TaskDone,
        } kind;
        QRectF rect;
    };
    void applyStyle();
    void sizeImages();
    void updateColumn(bool force);
    void computeDecorations();
    void openLink(const QUrl &url);
    bool scrollToHeading(const QString &slug);
    qreal sidePadding() const;

    QString m_baseDir;
    QFont m_bodyFont;
    QFont m_codeFont;
    qreal m_em = 14; // body font size in pixels: spacing is relative to it
    int m_column = 0; // width of the text column
    std::vector<Decoration> m_decorations;
    bool m_decorationsValid = false;
    std::vector<int> m_headingLines; // source line of each heading
    std::vector<int> m_headingBlocks; // block number of each heading
    QHash<QString, QImage> m_images; // scaled images of the last rendering
    QTimer m_resizeTimer; // images fit the new width when resizing stops
};

// A preview tab ("Preview README.md") that follows its document as it is
// typed (debounced) and its editor as it scrolls. It goes away with the
// document.
class MarkdownPreview : public MarkdownView
{
    Q_OBJECT
public:
    explicit MarkdownPreview(KTextEditor::Document *doc, QWidget *parent = nullptr);

    KTextEditor::Document *source() const;
    QString title() const;
    // .md, .markdown... or a document in the Markdown language mode.
    static bool isMarkdown(const KTextEditor::Document *doc);
    // The preview glyph: tab icon and the editor tab bar's button.
    static QIcon icon();

Q_SIGNALS:
    void focused();
    void titleChanged();

protected:
    void showEvent(QShowEvent *event) override;
    void focusInEvent(QFocusEvent *event) override;

private:
    void refresh();
    void watch(KTextEditor::View *view);
    void follow(KTextEditor::View *view);

    QPointer<KTextEditor::Document> m_doc;
    QTimer m_renderTimer;
    bool m_dirty = true;
    bool m_shown = false;
};
