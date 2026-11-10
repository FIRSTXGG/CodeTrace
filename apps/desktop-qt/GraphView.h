// CodeTrace desktop — interactive call graph (zoom, pan, click-to-source, chain highlighting).
#pragma once

#include "core/analysis/project.hpp"

#include <QGraphicsView>
#include <QHash>
#include <memory>

class QGraphicsScene;
class NodeItem;
class EdgeItem;

class GraphView : public QGraphicsView {
    Q_OBJECT
public:
    explicit GraphView(QWidget* parent = nullptr);

    void setSnapshot(std::shared_ptr<const codetrace::ProjectSnapshot> snapshot);

    /// Show `focus` with callers/callees up to `depth` hops.
    void showNeighbourhood(codetrace::SymbolId focus, int depth);
    /// Show every callable that takes part in a call (capped for very large projects).
    void showWholeGraph(int maxNodes = 400);

    void highlightChains(codetrace::SymbolId id);
    void clearHighlight();
    void fitAll();
    void zoomBy(double factor);

    codetrace::SymbolId focusSymbol() const { return focus_; }

signals:
    void symbolClicked(codetrace::SymbolId id);
    void symbolDoubleClicked(codetrace::SymbolId id);
    void statusMessage(const QString& text);

protected:
    void wheelEvent(QWheelEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;

private:
    void build(const std::vector<codetrace::SymbolId>& nodes, const std::vector<int>& layers);
    NodeItem* nodeAt(const QPoint& viewPos) const;

    QGraphicsScene* scene_;
    std::shared_ptr<const codetrace::ProjectSnapshot> snap_;
    QHash<codetrace::SymbolId, NodeItem*> nodes_;
    std::vector<EdgeItem*> edges_;
    codetrace::SymbolId focus_ = codetrace::kInvalidSymbol;
};
