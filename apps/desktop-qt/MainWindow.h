// CodeTrace desktop — main window: symbol browser, call graph, source view, diagnostics.
#pragma once

#include "core/analysis/project.hpp"

#include <QFutureWatcher>
#include <QMainWindow>
#include <memory>

class CodeView;
class GraphView;
class QComboBox;
class QFileSystemWatcher;
class QLabel;
class QLineEdit;
class QListWidget;
class QProgressBar;
class QSpinBox;
class QTimer;
class QTreeWidget;
class QTreeWidgetItem;
class QAction;

using SnapshotPtr = std::shared_ptr<const codetrace::ProjectSnapshot>;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

    void openProject(const QString& path);
    /// Select the first symbol whose qualified name equals (or ends with) `name`.
    bool selectSymbolByName(const QString& name);

signals:
    void analysisCompleted(bool ok);

protected:
    void closeEvent(QCloseEvent* event) override;

private slots:
    void chooseFolder();
    void chooseFile();
    void reanalyze();
    void cancelAnalysis();
    void analysisFinished();
    void applyFilter();
    void symbolSelected(codetrace::SymbolId id, bool updateGraph);
    void refreshGraph();
    void filesChanged(const QString& path);

private:
    void buildUi();
    void startAnalysis();
    void populateSymbols();
    void populateDiagnostics();
    void updateDetails(codetrace::SymbolId id);
    void watchFiles();
    void selectInTree(codetrace::SymbolId id);
    QString relativePath(const std::string& path) const;

    std::shared_ptr<codetrace::AnalyzerRegistry> registry_;
    std::unique_ptr<codetrace::ProjectAnalyzer> analyzer_;
    codetrace::ProjectOptions options_;
    codetrace::CancellationToken cancel_;
    QFutureWatcher<SnapshotPtr> watcher_;
    SnapshotPtr snapshot_;
    bool restartPending_ = false;
    QString analysisError_;
    codetrace::SymbolId current_ = codetrace::kInvalidSymbol;

    // widgets
    QTreeWidget* symbolTree_ = nullptr;
    QLineEdit* search_ = nullptr;
    QComboBox* languageFilter_ = nullptr;
    QComboBox* graphMode_ = nullptr;
    QSpinBox* depth_ = nullptr;
    GraphView* graph_ = nullptr;
    CodeView* code_ = nullptr;
    QLabel* detailsHeader_ = nullptr;
    QListWidget* callers_ = nullptr;
    QListWidget* callees_ = nullptr;
    QListWidget* unresolved_ = nullptr;
    QTreeWidget* diagnostics_ = nullptr;
    QLabel* status_ = nullptr;
    QProgressBar* progress_ = nullptr;
    QAction* cancelAction_ = nullptr;
    QFileSystemWatcher* fsWatcher_ = nullptr;
    QTimer* rescanTimer_ = nullptr;
    QTimer* filterTimer_ = nullptr;
};
