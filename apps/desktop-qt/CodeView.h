// CodeTrace desktop — read-only source viewer with line numbers and highlighting.
#pragma once

#include "core/analysis/model.hpp"

#include <QPlainTextEdit>

class QSyntaxHighlighter;

class CodeView : public QPlainTextEdit {
    Q_OBJECT
public:
    explicit CodeView(QWidget* parent = nullptr);

    /// Loads `path` (if not already shown) and highlights `line` (1-based).
    bool showLocation(const QString& path, int line, codetrace::Language lang);
    const QString& currentFile() const { return file_; }
    void reloadIfShowing(const QString& path);

    int lineNumberAreaWidth() const;
    void paintLineNumbers(QPaintEvent* event);

signals:
    void lineActivated(const QString& file, int line);

protected:
    void resizeEvent(QResizeEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;

private:
    void highlightLine(int line);

    QWidget* lineArea_;
    QSyntaxHighlighter* highlighter_ = nullptr;
    QString file_;
    int markedLine_ = 0;
};
