#include "MainWindow.h"

#include "CodeView.h"
#include "GraphView.h"
#include "analyzers/registry.hpp"

#include <algorithm>
#include <tuple>

#include <QAction>
#include <QApplication>
#include <QCloseEvent>
#include <QComboBox>
#include <QDir>
#include <QDockWidget>
#include <QFileDialog>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenuBar>
#include <QMessageBox>
#include <QPointer>
#include <QProgressBar>
#include <QSettings>
#include <QSpinBox>
#include <QSplitter>
#include <QStatusBar>
#include <QTimer>
#include <QToolBar>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrentRun>

using namespace codetrace;

namespace {
constexpr int kSymbolRole = Qt::UserRole + 1;
constexpr int kFileRole = Qt::UserRole + 2;
constexpr int kLineRole = Qt::UserRole + 3;

QString kindIcon(SymbolKind k) {
    switch (k) {
        case SymbolKind::Class: return QStringLiteral("C");
        case SymbolKind::Method: return QStringLiteral("m");
        case SymbolKind::Constructor: return QStringLiteral("c");
        case SymbolKind::Module: return QStringLiteral("M");
        case SymbolKind::Lambda: return QStringLiteral("λ");
        default: return QStringLiteral("f");
    }
}
}  // namespace

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent), registry_(createDefaultRegistry()), analyzer_(std::make_unique<ProjectAnalyzer>(registry_)) {
    buildUi();
    connect(&watcher_, &QFutureWatcher<SnapshotPtr>::finished, this, &MainWindow::analysisFinished);
    QSettings s;
    restoreGeometry(s.value("window/geometry").toByteArray());
    restoreState(s.value("window/state").toByteArray());
}

MainWindow::~MainWindow() {
    cancel_.cancel();
    watcher_.waitForFinished();
}

void MainWindow::buildUi() {
    setWindowTitle(tr("CodeTrace"));
    resize(1400, 860);

    // --- actions & menus ------------------------------------------------------
    auto* fileMenu = menuBar()->addMenu(tr("&File"));
    auto* openFolder = fileMenu->addAction(tr("Open &Folder…"), QKeySequence::Open, this, &MainWindow::chooseFolder);
    fileMenu->addAction(tr("Open F&ile…"), this, &MainWindow::chooseFile);
    auto* reanalyzeAct = fileMenu->addAction(tr("&Reanalyze"), QKeySequence::Refresh, this, &MainWindow::reanalyze);
    fileMenu->addSeparator();
    fileMenu->addAction(tr("&Quit"), QKeySequence::Quit, qApp, &QApplication::quit);

    auto* viewMenu = menuBar()->addMenu(tr("&View"));
    viewMenu->addAction(tr("Zoom &In"), QKeySequence::ZoomIn, this, [this] { graph_->zoomBy(1.25); });
    viewMenu->addAction(tr("Zoom &Out"), QKeySequence::ZoomOut, this, [this] { graph_->zoomBy(0.8); });
    viewMenu->addAction(tr("&Fit Graph"), QKeySequence(Qt::CTRL | Qt::Key_0), this, [this] { graph_->fitAll(); });
    viewMenu->addAction(tr("Find &Symbol"), QKeySequence::Find, this, [this] { search_->setFocus(); search_->selectAll(); });

    auto* help = menuBar()->addMenu(tr("&Help"));
    help->addAction(tr("&About CodeTrace"), this, [this] {
        QString backends;
        for (const auto& a : registry_->analyzers())
            backends += QString("<li>%1: %2 (%3)</li>")
                            .arg(QString::fromUtf8(toString(a->language()).data()), QString::fromUtf8(a->backendName().data()),
                                 a->isAvailable() ? tr("available") : tr("unavailable"));
        QMessageBox::about(this, tr("About CodeTrace"),
                           tr("<h3>CodeTrace 0.1.0</h3><p>Static analysis and call graph visualization for C++, "
                              "Python and JavaScript.</p><p>One Core — Multiple Interfaces — Multiple Languages.</p>"
                              "<p>Analyzer backends:</p><ul>%1</ul><p>MIT License.</p>")
                               .arg(backends));
    });

    // --- toolbar ---------------------------------------------------------------
    auto* tb = addToolBar(tr("Main"));
    tb->setObjectName("mainToolbar");
    tb->setMovable(false);
    tb->addAction(openFolder);
    tb->addAction(reanalyzeAct);
    cancelAction_ = tb->addAction(tr("Cancel"), this, &MainWindow::cancelAnalysis);
    cancelAction_->setEnabled(false);
    tb->addSeparator();
    tb->addWidget(new QLabel(tr(" Graph: ")));
    graphMode_ = new QComboBox;
    graphMode_->addItems({tr("Neighbourhood"), tr("Whole project")});
    tb->addWidget(graphMode_);
    tb->addWidget(new QLabel(tr("  Depth: ")));
    depth_ = new QSpinBox;
    depth_->setRange(1, 8);
    depth_->setValue(2);
    tb->addWidget(depth_);
    tb->addSeparator();
    tb->addAction(tr("Fit"), this, [this] { graph_->fitAll(); });
    tb->addAction(tr("−"), this, [this] { graph_->zoomBy(0.8); });
    tb->addAction(tr("+"), this, [this] { graph_->zoomBy(1.25); });
    connect(graphMode_, &QComboBox::currentIndexChanged, this, &MainWindow::refreshGraph);
    connect(depth_, &QSpinBox::valueChanged, this, &MainWindow::refreshGraph);

    // --- left: symbol browser ----------------------------------------------------
    auto* left = new QWidget;
    auto* leftLayout = new QVBoxLayout(left);
    leftLayout->setContentsMargins(4, 4, 4, 4);
    search_ = new QLineEdit;
    search_->setPlaceholderText(tr("Search functions, classes…"));
    search_->setClearButtonEnabled(true);
    languageFilter_ = new QComboBox;
    languageFilter_->addItems({tr("All languages"), "C++", "Python", "JavaScript"});
    symbolTree_ = new QTreeWidget;
    symbolTree_->setHeaderLabels({tr("Symbol"), tr("Line")});
    symbolTree_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    symbolTree_->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    symbolTree_->header()->setStretchLastSection(false);
    symbolTree_->setUniformRowHeights(true);
    leftLayout->addWidget(search_);
    leftLayout->addWidget(languageFilter_);
    leftLayout->addWidget(symbolTree_);

    filterTimer_ = new QTimer(this);
    filterTimer_->setSingleShot(true);
    filterTimer_->setInterval(150);
    connect(filterTimer_, &QTimer::timeout, this, &MainWindow::applyFilter);
    connect(search_, &QLineEdit::textChanged, filterTimer_, qOverload<>(&QTimer::start));
    connect(languageFilter_, &QComboBox::currentIndexChanged, this, &MainWindow::applyFilter);
    connect(search_, &QLineEdit::returnPressed, this, [this] {
        // Enter jumps to the first visible symbol.
        for (int i = 0; i < symbolTree_->topLevelItemCount(); ++i) {
            auto* f = symbolTree_->topLevelItem(i);
            if (f->isHidden()) continue;
            for (int j = 0; j < f->childCount(); ++j)
                if (!f->child(j)->isHidden()) {
                    symbolTree_->setCurrentItem(f->child(j));
                    return;
                }
        }
    });
    connect(symbolTree_, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem* item) {
        if (!item || !item->data(0, kSymbolRole).isValid()) return;
        symbolSelected(item->data(0, kSymbolRole).toUInt(), true);
    });

    // --- centre: graph + code ------------------------------------------------------
    graph_ = new GraphView;
    code_ = new CodeView;
    auto* centre = new QSplitter(Qt::Vertical);
    centre->addWidget(graph_);
    centre->addWidget(code_);
    centre->setStretchFactor(0, 3);
    centre->setStretchFactor(1, 2);
    centre->setSizes({480, 320});
    connect(graph_, &GraphView::symbolClicked, this, [this](SymbolId id) { symbolSelected(id, false); });
    connect(graph_, &GraphView::symbolDoubleClicked, this, [this](SymbolId id) {
        graphMode_->setCurrentIndex(0);
        symbolSelected(id, true);
    });
    connect(graph_, &GraphView::statusMessage, this, [this](const QString& m) { status_->setText(m); });

    auto* split = new QSplitter(Qt::Horizontal);
    split->addWidget(left);
    split->addWidget(centre);
    split->setStretchFactor(0, 1);
    split->setStretchFactor(1, 4);
    split->setSizes({320, 1080});
    setCentralWidget(split);

    // --- right dock: details -------------------------------------------------------
    auto* details = new QWidget;
    auto* dl = new QVBoxLayout(details);
    detailsHeader_ = new QLabel(tr("No symbol selected"));
    detailsHeader_->setWordWrap(true);
    detailsHeader_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    callers_ = new QListWidget;
    callees_ = new QListWidget;
    unresolved_ = new QListWidget;
    dl->addWidget(detailsHeader_);
    dl->addWidget(new QLabel(tr("<b>Called by</b>")));
    dl->addWidget(callers_);
    dl->addWidget(new QLabel(tr("<b>Calls</b>")));
    dl->addWidget(callees_);
    dl->addWidget(new QLabel(tr("<b>Unresolved / external calls</b>")));
    dl->addWidget(unresolved_);
    auto* detailsDock = new QDockWidget(tr("Details"), this);
    detailsDock->setObjectName("detailsDock");
    detailsDock->setWidget(details);
    addDockWidget(Qt::RightDockWidgetArea, detailsDock);
    viewMenu->addSeparator();
    viewMenu->addAction(detailsDock->toggleViewAction());
    auto jump = [this](QListWidgetItem* item) {
        if (!item) return;
        if (item->data(kSymbolRole).isValid()) {
            symbolSelected(item->data(kSymbolRole).toUInt(), true);
        } else if (item->data(kFileRole).isValid() && snapshot_ && current_ != kInvalidSymbol) {
            code_->showLocation(item->data(kFileRole).toString(), item->data(kLineRole).toInt(),
                                snapshot_->symbols[current_].language);
        }
    };
    connect(callers_, &QListWidget::itemActivated, this, jump);
    connect(callees_, &QListWidget::itemActivated, this, jump);
    connect(unresolved_, &QListWidget::itemActivated, this, jump);

    // --- bottom dock: diagnostics --------------------------------------------------
    diagnostics_ = new QTreeWidget;
    diagnostics_->setHeaderLabels({tr("Severity"), tr("File"), tr("Line"), tr("Message")});
    diagnostics_->setRootIsDecorated(false);
    diagnostics_->header()->setStretchLastSection(true);
    auto* diagDock = new QDockWidget(tr("Diagnostics"), this);
    diagDock->setObjectName("diagnosticsDock");
    diagDock->setWidget(diagnostics_);
    addDockWidget(Qt::BottomDockWidgetArea, diagDock);
    viewMenu->addAction(diagDock->toggleViewAction());
    resizeDocks({diagDock}, {140}, Qt::Vertical);
    resizeDocks({detailsDock}, {300}, Qt::Horizontal);
    connect(diagnostics_, &QTreeWidget::itemActivated, this, [this](QTreeWidgetItem* item) {
        QString file = item->data(0, kFileRole).toString();
        if (file.isEmpty() || !snapshot_) return;
        code_->showLocation(file, item->data(0, kLineRole).toInt(),
                            registry_->detectLanguage(file.toStdString()));
    });

    // --- status bar ------------------------------------------------------------------
    status_ = new QLabel(tr("Open a folder to start (File → Open Folder)."));
    progress_ = new QProgressBar;
    progress_->setMaximumWidth(220);
    progress_->setVisible(false);
    statusBar()->addWidget(status_, 1);
    statusBar()->addPermanentWidget(progress_);

    // --- incremental re-analysis on file changes ------------------------------------
    fsWatcher_ = new QFileSystemWatcher(this);
    rescanTimer_ = new QTimer(this);
    rescanTimer_->setSingleShot(true);
    rescanTimer_->setInterval(700);
    connect(fsWatcher_, &QFileSystemWatcher::fileChanged, this, &MainWindow::filesChanged);
    connect(fsWatcher_, &QFileSystemWatcher::directoryChanged, this, &MainWindow::filesChanged);
    connect(rescanTimer_, &QTimer::timeout, this, &MainWindow::startAnalysis);
}

void MainWindow::chooseFolder() {
    QSettings s;
    QString dir = QFileDialog::getExistingDirectory(this, tr("Open Project Folder"), s.value("lastFolder").toString());
    if (!dir.isEmpty()) openProject(dir);
}

void MainWindow::chooseFile() {
    QSettings s;
    QString file = QFileDialog::getOpenFileName(this, tr("Open Source File"), s.value("lastFolder").toString(),
                                                tr("Source files (*.cpp *.cc *.cxx *.c *.h *.hpp *.py *.js *.mjs *.cjs *.jsx);;All files (*)"));
    if (!file.isEmpty()) openProject(file);
}

void MainWindow::openProject(const QString& path) {
    QFileInfo info(path);
    if (!info.exists()) {
        QMessageBox::warning(this, tr("CodeTrace"), tr("%1 does not exist.").arg(path));
        return;
    }
    QSettings().setValue("lastFolder", info.isDir() ? info.absoluteFilePath() : info.absolutePath());
    options_ = ProjectOptions{};
    options_.root = QDir::cleanPath(info.absoluteFilePath()).toStdString();
    analyzer_->clearCache();
    snapshot_.reset();
    current_ = kInvalidSymbol;
    setWindowTitle(tr("CodeTrace — %1").arg(info.fileName()));
    startAnalysis();
}

void MainWindow::reanalyze() {
    if (options_.root.empty()) return;
    analyzer_->clearCache();
    startAnalysis();
}

void MainWindow::startAnalysis() {
    if (options_.root.empty()) return;
    if (watcher_.isRunning()) {
        restartPending_ = true;
        cancel_.cancel();
        return;
    }
    cancel_.reset();
    restartPending_ = false;
    analysisError_.clear();
    progress_->setVisible(true);
    progress_->setRange(0, 0);
    cancelAction_->setEnabled(true);
    status_->setText(tr("Analyzing %1…").arg(QString::fromStdString(options_.root)));

    QPointer<MainWindow> self(this);
    auto progress = [self](const Progress& p) {
        // Called from worker threads: hop to the GUI thread.
        QMetaObject::invokeMethod(qApp, [self, p] {
            if (!self) return;
            self->progress_->setRange(0, static_cast<int>(std::max<std::size_t>(1, p.total)));
            self->progress_->setValue(static_cast<int>(p.done));
            if (!p.currentFile.empty())
                self->status_->setText(tr("Analyzing %1/%2: %3").arg(p.done).arg(p.total).arg(self->relativePath(p.currentFile)));
        }, Qt::QueuedConnection);
    };
    ProjectOptions opts = options_;
    ProjectAnalyzer* analyzer = analyzer_.get();
    CancellationToken* cancel = &cancel_;
    QString* error = &analysisError_;
    watcher_.setFuture(QtConcurrent::run([analyzer, opts, progress, cancel, error]() -> SnapshotPtr {
        try {
            return analyzer->analyze(opts, progress, cancel);
        } catch (const std::exception& e) {
            *error = QString::fromUtf8(e.what());
        } catch (...) {
            *error = QObject::tr("unknown error");
        }
        return nullptr;
    }));
}

void MainWindow::cancelAnalysis() {
    restartPending_ = false;
    cancel_.cancel();
}

void MainWindow::analysisFinished() {
    progress_->setVisible(false);
    cancelAction_->setEnabled(false);
    SnapshotPtr result = watcher_.result();
    if (restartPending_) {
        startAnalysis();
        return;
    }
    if (!result) {
        status_->setText(tr("Analysis failed: %1").arg(analysisError_));
        emit analysisCompleted(false);
        return;
    }
    // Keep the selection across re-analysis by qualified name + file.
    std::string prevName, prevFile;
    if (snapshot_ && current_ != kInvalidSymbol && snapshot_->symbol(current_)) {
        prevName = snapshot_->symbols[current_].qualifiedName;
        prevFile = snapshot_->symbols[current_].file;
    }
    snapshot_ = result;
    graph_->setSnapshot(snapshot_);
    current_ = kInvalidSymbol;
    for (const auto& s : snapshot_->symbols)
        if (s.qualifiedName == prevName && s.file == prevFile) current_ = s.id;

    populateSymbols();
    populateDiagnostics();
    watchFiles();

    auto counts = countDiagnostics(snapshot_->diagnostics);
    status_->setText(tr("%1 files · %2 symbols · %3 calls · %4 re-analyzed · %5 errors, %6 warnings%7")
                         .arg(snapshot_->files.size())
                         .arg(snapshot_->symbols.size())
                         .arg(snapshot_->callGraph.edges().size())
                         .arg(snapshot_->reanalyzedFiles)
                         .arg(counts.errors)
                         .arg(counts.warnings)
                         .arg(snapshot_->cancelled ? tr(" · cancelled (partial results)") : QString()));
    if (current_ != kInvalidSymbol) {
        selectInTree(current_);
        code_->reloadIfShowing(QString::fromStdString(snapshot_->symbols[current_].file));
    } else {
        graphMode_->setCurrentIndex(1);
        refreshGraph();
    }
    emit analysisCompleted(true);
}

bool MainWindow::selectSymbolByName(const QString& name) {
    if (!snapshot_) return false;
    const std::string n = name.toStdString();
    for (const auto& s : snapshot_->symbols) {
        const auto& q = s.qualifiedName;
        bool match = q == n || (q.size() > n.size() && q.compare(q.size() - n.size(), n.size(), n) == 0 &&
                                (q[q.size() - n.size() - 1] == ':' || q[q.size() - n.size() - 1] == '.'));
        if (match && isCallable(s.kind)) {
            selectInTree(s.id);
            return true;
        }
    }
    return false;
}

QString MainWindow::relativePath(const std::string& path) const {
    QString root = QString::fromStdString(snapshot_ ? snapshot_->root : options_.root);
    QString p = QString::fromStdString(path);
    if (p.startsWith(root + '/')) return p.mid(root.size() + 1);
    return p;
}

void MainWindow::populateSymbols() {
    symbolTree_->setUpdatesEnabled(false);
    symbolTree_->clear();
    QTreeWidgetItem* fileItem = nullptr;
    std::string lastFile;
    std::vector<SymbolId> order(snapshot_->symbols.size());
    for (SymbolId i = 0; i < order.size(); ++i) order[i] = i;
    std::sort(order.begin(), order.end(), [&](SymbolId a, SymbolId b) {
        const auto &x = snapshot_->symbols[a], &y = snapshot_->symbols[b];
        return std::tie(x.file, x.range.line) < std::tie(y.file, y.range.line);
    });
    for (SymbolId id : order) {
        const Symbol& s = snapshot_->symbols[id];
        if (s.file != lastFile) {
            fileItem = new QTreeWidgetItem(symbolTree_, {relativePath(s.file)});
            fileItem->setData(0, kFileRole, QString::fromStdString(s.file));
            fileItem->setToolTip(0, QString::fromStdString(s.file));
            fileItem->setFirstColumnSpanned(true);
            lastFile = s.file;
        }
        QString label = kindIcon(s.kind) + "  " + QString::fromStdString(s.qualifiedName);
        auto* item = new QTreeWidgetItem(fileItem, {label, QString::number(s.range.line)});
        item->setData(0, kSymbolRole, id);
        item->setToolTip(0, QString::fromStdString(s.signature));
        if (!isCallable(s.kind)) {
            QFont f = item->font(0);
            f.setBold(true);
            item->setFont(0, f);
        }
    }
    symbolTree_->expandAll();
    symbolTree_->setUpdatesEnabled(true);
    applyFilter();
}

void MainWindow::applyFilter() {
    if (!snapshot_) return;
    const QString q = search_->text().trimmed();
    const int langIdx = languageFilter_->currentIndex();
    const Language lang = langIdx == 1 ? Language::Cpp : langIdx == 2 ? Language::Python : langIdx == 3 ? Language::JavaScript
                                                                                                       : Language::Unknown;
    symbolTree_->setUpdatesEnabled(false);
    for (int i = 0; i < symbolTree_->topLevelItemCount(); ++i) {
        auto* f = symbolTree_->topLevelItem(i);
        int visible = 0;
        for (int j = 0; j < f->childCount(); ++j) {
            auto* c = f->child(j);
            const Symbol& s = snapshot_->symbols[c->data(0, kSymbolRole).toUInt()];
            bool show = (lang == Language::Unknown || s.language == lang) &&
                        (q.isEmpty() || QString::fromStdString(s.qualifiedName).contains(q, Qt::CaseInsensitive));
            c->setHidden(!show);
            visible += show;
        }
        f->setHidden(visible == 0 && !(q.isEmpty() && lang == Language::Unknown));
    }
    symbolTree_->setUpdatesEnabled(true);
}

void MainWindow::populateDiagnostics() {
    diagnostics_->clear();
    for (const auto& d : snapshot_->diagnostics) {
        auto* item = new QTreeWidgetItem(diagnostics_, {QString::fromUtf8(toString(d.severity).data()), relativePath(d.file),
                                                        d.line ? QString::number(d.line) : QString(),
                                                        QString::fromStdString(d.message)});
        item->setData(0, kFileRole, QString::fromStdString(d.file));
        item->setData(0, kLineRole, static_cast<int>(d.line));
        item->setToolTip(3, QString::fromStdString(d.message + "  [" + d.source + "]"));
        if (d.severity == Severity::Error) item->setForeground(0, QColor(0xdc, 0x26, 0x26));
        else if (d.severity == Severity::Warning) item->setForeground(0, QColor(0xb4, 0x53, 0x09));
    }
    diagnostics_->resizeColumnToContents(0);
    diagnostics_->resizeColumnToContents(1);
}

void MainWindow::selectInTree(SymbolId id) {
    for (int i = 0; i < symbolTree_->topLevelItemCount(); ++i) {
        auto* f = symbolTree_->topLevelItem(i);
        for (int j = 0; j < f->childCount(); ++j) {
            if (f->child(j)->data(0, kSymbolRole).toUInt() == id) {
                symbolTree_->setCurrentItem(f->child(j));   // triggers symbolSelected
                symbolTree_->scrollToItem(f->child(j));
                return;
            }
        }
    }
}

void MainWindow::symbolSelected(SymbolId id, bool updateGraph) {
    if (!snapshot_ || !snapshot_->symbol(id)) return;
    current_ = id;
    const Symbol& s = snapshot_->symbols[id];
    code_->showLocation(QString::fromStdString(s.file), static_cast<int>(s.range.line), s.language);
    updateDetails(id);
    if (updateGraph && isCallable(s.kind)) {
        if (graphMode_->currentIndex() != 0) {
            QSignalBlocker block(graphMode_);
            graphMode_->setCurrentIndex(0);
        }
        graph_->showNeighbourhood(id, depth_->value());
    } else if (!updateGraph) {
        graph_->highlightChains(id);
    }
    // keep tree in sync without re-triggering
    auto* cur = symbolTree_->currentItem();
    if (!cur || cur->data(0, kSymbolRole).toUInt() != id) {
        QSignalBlocker block(symbolTree_);
        for (int i = 0; i < symbolTree_->topLevelItemCount(); ++i) {
            auto* f = symbolTree_->topLevelItem(i);
            for (int j = 0; j < f->childCount(); ++j)
                if (f->child(j)->data(0, kSymbolRole).toUInt() == id) {
                    symbolTree_->setCurrentItem(f->child(j));
                    symbolTree_->scrollToItem(f->child(j));
                }
        }
    }
}

void MainWindow::refreshGraph() {
    if (!snapshot_) return;
    if (graphMode_->currentIndex() == 1 || current_ == kInvalidSymbol) {
        graph_->showWholeGraph();
        if (current_ != kInvalidSymbol) graph_->highlightChains(current_);
    } else {
        graph_->showNeighbourhood(current_, depth_->value());
    }
}

void MainWindow::updateDetails(SymbolId id) {
    const Symbol& s = snapshot_->symbols[id];
    detailsHeader_->setText(QString("<b>%1</b><br><code>%2</code><br>%3 · %4<br><small>%5:%6</small>")
                                .arg(QString::fromStdString(s.qualifiedName).toHtmlEscaped(),
                                     QString::fromStdString(s.signature).toHtmlEscaped(),
                                     QString::fromUtf8(toString(s.kind).data()), QString::fromUtf8(toString(s.language).data()),
                                     relativePath(s.file).toHtmlEscaped())
                                .arg(s.range.line));
    callers_->clear();
    callees_->clear();
    unresolved_->clear();
    const auto& g = snapshot_->callGraph;
    auto addSym = [&](QListWidget* list, SymbolId other, std::uint32_t line, Resolution r) {
        const Symbol& o = snapshot_->symbols[other];
        QString text = QString::fromStdString(o.qualifiedName) + QString("  (line %1)").arg(line);
        if (r != Resolution::Exact) text += QString("  [%1]").arg(QString::fromUtf8(toString(r).data()));
        auto* item = new QListWidgetItem(text, list);
        item->setData(kSymbolRole, other);
        item->setToolTip(QString::fromStdString(o.file + ":" + std::to_string(o.range.line)));
    };
    for (auto e : g.incoming(id)) addSym(callers_, g.edges()[e].caller, g.edges()[e].line, g.edges()[e].resolution);
    for (auto e : g.outgoing(id)) addSym(callees_, g.edges()[e].callee, g.edges()[e].line, g.edges()[e].resolution);
    for (const auto& u : g.unresolved()) {
        if (u.caller != id) continue;
        QString text = QString::fromStdString(u.calleeName) + QString("  (line %1)").arg(u.line);
        if (u.candidates > 1) text += tr("  [ambiguous: %1 candidates]").arg(u.candidates);
        auto* item = new QListWidgetItem(text, unresolved_);
        item->setData(kFileRole, QString::fromStdString(s.file));
        item->setData(kLineRole, static_cast<int>(u.line));
    }
}

void MainWindow::watchFiles() {
    if (!fsWatcher_->files().isEmpty()) fsWatcher_->removePaths(fsWatcher_->files());
    if (!fsWatcher_->directories().isEmpty()) fsWatcher_->removePaths(fsWatcher_->directories());
    QStringList files, dirs;
    for (const auto& f : snapshot_->files) {
        if (files.size() >= 4000) break;   // OS limits on watched descriptors
        QString p = QString::fromStdString(f.file);
        files << p;
        QString d = QFileInfo(p).absolutePath();
        if (!dirs.contains(d) && dirs.size() < 1000) dirs << d;
    }
    if (!files.isEmpty()) fsWatcher_->addPaths(files);
    if (!dirs.isEmpty()) fsWatcher_->addPaths(dirs);
}

void MainWindow::filesChanged(const QString& path) {
    analyzer_->invalidate({path.toStdString()});
    // Editors often replace files atomically: re-add the watch.
    if (QFileInfo::exists(path) && !fsWatcher_->files().contains(path) && QFileInfo(path).isFile()) fsWatcher_->addPath(path);
    rescanTimer_->start();
}

void MainWindow::closeEvent(QCloseEvent* event) {
    QSettings s;
    s.setValue("window/geometry", saveGeometry());
    s.setValue("window/state", saveState());
    cancel_.cancel();
    QMainWindow::closeEvent(event);
}
