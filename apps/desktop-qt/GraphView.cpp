#include "GraphView.h"

#include "core/graph/layout.hpp"

#include <QFileInfo>
#include <QFontMetricsF>
#include <QGraphicsPathItem>
#include <QGraphicsRectItem>
#include <QGraphicsScene>
#include <QGraphicsSimpleTextItem>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QScrollBar>
#include <QSet>
#include <QWheelEvent>
#include <QtMath>

#include <deque>
#include <algorithm>
#include <cmath>
#include <map>
#include <set>

using namespace codetrace;

namespace {

QColor languageColor(Language l) {
    switch (l) {
        case Language::Cpp: return QColor(0x3b, 0x82, 0xf6);
        case Language::Python: return QColor(0x16, 0xa3, 0x4a);
        case Language::JavaScript: return QColor(0xd9, 0x77, 0x06);
        default: return QColor(0x6b, 0x72, 0x80);
    }
}

enum class Mark { None, Focus, Upstream, Downstream, Dimmed };

}  // namespace

class NodeItem : public QGraphicsRectItem {
public:
    NodeItem(const Symbol& s, bool external) : symbol(s.id), lang(s.language) {
        setFlag(ItemIsSelectable);
        setAcceptHoverEvents(true);
        label = new QGraphicsSimpleTextItem(QString::fromStdString(s.name == "<module>" ? "<module " +
                                                    QFileInfo(QString::fromStdString(s.file)).fileName().toStdString() + ">"
                                                                                       : s.name),
                                            this);
        QFont f = label->font();
        f.setPointSizeF(10);
        label->setFont(f);
        QRectF tr = label->boundingRect();
        const double w = std::max(80.0, tr.width() + 20), h = tr.height() + 12;
        setRect(-w / 2, -h / 2, w, h);
        label->setPos(-tr.width() / 2, -tr.height() / 2);
        setToolTip(QString::fromStdString(s.qualifiedName + "\n" + s.signature + "\n" + s.file + ":" +
                                          std::to_string(s.range.line)));
        (void)external;
        applyMark(Mark::None);
        setZValue(1);
    }

    void applyMark(Mark m) {
        mark = m;
        QColor base = languageColor(lang);
        QColor fill = base.lighter(185), border = base;
        double width = 1.2;
        switch (m) {
            case Mark::Focus: fill = base.lighter(130); border = base.darker(150); width = 2.6; break;
            case Mark::Upstream: border = QColor(0x7c, 0x3a, 0xed); width = 2.2; break;
            case Mark::Downstream: border = QColor(0xdc, 0x26, 0x26); width = 2.2; break;
            case Mark::Dimmed: fill = QColor(0xf3, 0xf4, 0xf6); border = QColor(0xd1, 0xd5, 0xdb); break;
            case Mark::None: break;
        }
        setBrush(fill);
        setPen(QPen(border, width));
        label->setBrush(m == Mark::Dimmed ? QColor(0x9c, 0xa3, 0xaf) : QColor(0x11, 0x18, 0x27));
    }

    void paint(QPainter* p, const QStyleOptionGraphicsItem*, QWidget*) override {
        p->setRenderHint(QPainter::Antialiasing);
        p->setPen(pen());
        p->setBrush(brush());
        p->drawRoundedRect(rect(), 6, 6);
        if (isSelected()) {
            p->setPen(QPen(QColor(0x11, 0x18, 0x27), 1, Qt::DashLine));
            p->setBrush(Qt::NoBrush);
            p->drawRoundedRect(rect().adjusted(-3, -3, 3, 3), 8, 8);
        }
    }

    SymbolId symbol;
    Language lang;
    Mark mark = Mark::None;
    QGraphicsSimpleTextItem* label;
};

class EdgeItem : public QGraphicsPathItem {
public:
    EdgeItem(NodeItem* from, NodeItem* to, Resolution r) : from(from), to(to), resolution(r) {
        setZValue(0);
        QPointF a = from->pos() + QPointF(from->rect().right(), 0);
        QPointF b = to->pos() + QPointF(to->rect().left(), 0);
        QPainterPath path;
        if (from == to) {   // recursion: small loop above the node
            QPointF top = from->pos() + QPointF(0, from->rect().top());
            path.moveTo(top + QPointF(-12, 0));
            path.cubicTo(top + QPointF(-30, -40), top + QPointF(30, -40), top + QPointF(12, 0));
            arrowTip = top + QPointF(12, 0);
            arrowDir = QPointF(0.3, 1);
        } else if (b.x() <= a.x() + 10) {   // back edge: route around
            a = from->pos() + QPointF(0, from->rect().bottom());
            b = to->pos() + QPointF(0, to->rect().bottom());
            double dy = 40 + std::abs(a.x() - b.x()) * 0.15;
            path.moveTo(a);
            path.cubicTo(a + QPointF(0, dy), b + QPointF(0, dy), b);
            arrowTip = b;
            arrowDir = QPointF(0, -1);
        } else {
            double dx = (b.x() - a.x()) * 0.5;
            path.moveTo(a);
            path.cubicTo(a + QPointF(dx, 0), b - QPointF(dx, 0), b);
            arrowTip = b;
            arrowDir = QPointF(1, 0);
        }
        setPath(path);
        applyMark(Mark::None);
    }

    void applyMark(Mark m) {
        QColor c(0x9c, 0xa3, 0xaf);
        double w = 1.2;
        if (m == Mark::Upstream) { c = QColor(0x7c, 0x3a, 0xed); w = 2.2; }
        if (m == Mark::Downstream) { c = QColor(0xdc, 0x26, 0x26); w = 2.2; }
        if (m == Mark::Dimmed) c = QColor(0xe5, 0xe7, 0xeb);
        if (resolution == Resolution::CrossLanguage && m != Mark::Dimmed) c = QColor(0xdb, 0x27, 0x77);
        QPen pen(c, w);
        if (resolution == Resolution::Likely) pen.setStyle(Qt::DashLine);
        setPen(pen);
        color = c;
        update();
    }

    void paint(QPainter* p, const QStyleOptionGraphicsItem* o, QWidget* w) override {
        p->setRenderHint(QPainter::Antialiasing);
        QGraphicsPathItem::paint(p, o, w);
        QPointF d = arrowDir / std::max(1e-6, std::hypot(arrowDir.x(), arrowDir.y()));
        QPointF n(-d.y(), d.x());
        QPolygonF head;
        head << arrowTip << arrowTip - d * 9 + n * 4.5 << arrowTip - d * 9 - n * 4.5;
        p->setPen(Qt::NoPen);
        p->setBrush(color);
        p->drawPolygon(head);
    }

    QRectF boundingRect() const override { return QGraphicsPathItem::boundingRect().adjusted(-10, -10, 10, 10); }

    NodeItem* from;
    NodeItem* to;
    Resolution resolution;
    QPointF arrowTip, arrowDir;
    QColor color;
};

GraphView::GraphView(QWidget* parent) : QGraphicsView(parent), scene_(new QGraphicsScene(this)) {
    setScene(scene_);
    setRenderHint(QPainter::Antialiasing);
    setDragMode(QGraphicsView::ScrollHandDrag);
    setTransformationAnchor(QGraphicsView::AnchorUnderMouse);
    setViewportUpdateMode(QGraphicsView::BoundingRectViewportUpdate);
    setBackgroundBrush(QColor(0xfa, 0xfa, 0xfb));
    setFocusPolicy(Qt::StrongFocus);
}

void GraphView::setSnapshot(std::shared_ptr<const ProjectSnapshot> snapshot) {
    snap_ = std::move(snapshot);
    if (snap_ && focus_ != kInvalidSymbol && !snap_->symbol(focus_)) focus_ = kInvalidSymbol;
}

void GraphView::showNeighbourhood(SymbolId focus, int depth) {
    if (!snap_ || !snap_->symbol(focus)) return;
    focus_ = focus;
    const auto& g = snap_->callGraph;
    // BFS both directions; layer = signed distance (callers negative).
    std::map<SymbolId, int> layer{{focus, 0}};
    for (int dir : {1, -1}) {
        std::deque<std::pair<SymbolId, int>> q{{focus, 0}};
        while (!q.empty()) {
            auto [cur, d] = q.front();
            q.pop_front();
            if (d >= depth) continue;
            for (SymbolId next : dir > 0 ? g.callees(cur) : g.callers(cur)) {
                if (layer.count(next)) continue;
                layer[next] = dir * (d + 1);
                q.push_back({next, d + 1});
            }
        }
    }
    std::vector<SymbolId> nodes;
    std::vector<int> layers;
    for (auto [id, l] : layer) {
        nodes.push_back(id);
        layers.push_back(l);
    }
    build(nodes, layers);
    highlightChains(focus);
    fitAll();
    if (auto it = nodes_.find(focus); it != nodes_.end()) centerOn(it.value());
    emit statusMessage(tr("%1 nodes around %2").arg(nodes.size()).arg(QString::fromStdString(snap_->symbols[focus].qualifiedName)));
}

void GraphView::showWholeGraph(int maxNodes) {
    if (!snap_) return;
    focus_ = kInvalidSymbol;
    std::set<SymbolId> used;
    for (const auto& e : snap_->callGraph.edges()) {
        used.insert(e.caller);
        used.insert(e.callee);
    }
    std::vector<SymbolId> nodes(used.begin(), used.end());
    bool truncated = false;
    if (static_cast<int>(nodes.size()) > maxNodes) {
        // Keep the most connected nodes.
        std::sort(nodes.begin(), nodes.end(), [&](SymbolId a, SymbolId b) {
            auto deg = [&](SymbolId s) { return snap_->callGraph.outgoing(s).size() + snap_->callGraph.incoming(s).size(); };
            return deg(a) > deg(b);
        });
        nodes.resize(static_cast<std::size_t>(maxNodes));
        truncated = true;
    }
    build(nodes, {});
    fitAll();
    emit statusMessage(truncated ? tr("Showing the %1 most connected functions (graph truncated)").arg(maxNodes)
                                 : tr("%1 functions with calls").arg(nodes.size()));
}

void GraphView::build(const std::vector<SymbolId>& nodes, const std::vector<int>& layers) {
    scene_->clear();
    nodes_.clear();
    edges_.clear();
    if (!snap_ || nodes.empty()) return;

    std::map<SymbolId, std::uint32_t> index;
    for (std::uint32_t i = 0; i < nodes.size(); ++i) index[nodes[i]] = i;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> edgeIdx;
    std::set<std::pair<SymbolId, SymbolId>> seen;
    std::vector<const CallEdge*> edgeRefs;
    for (const auto& e : snap_->callGraph.edges()) {
        auto a = index.find(e.caller), b = index.find(e.callee);
        if (a == index.end() || b == index.end()) continue;
        if (!seen.insert({e.caller, e.callee}).second) continue;
        edgeIdx.emplace_back(a->second, b->second);
        edgeRefs.push_back(&e);
    }

    LayoutOptions lo;
    lo.layerSpacing = 230;
    lo.nodeSpacing = 54;
    auto layout = layeredLayout(nodes.size(), edgeIdx, lo, layers);

    for (std::uint32_t i = 0; i < nodes.size(); ++i) {
        auto* item = new NodeItem(snap_->symbols[nodes[i]], false);
        item->setPos(layout[i].x, layout[i].y);
        scene_->addItem(item);
        nodes_.insert(nodes[i], item);
    }
    for (std::size_t k = 0; k < edgeIdx.size(); ++k) {
        auto* e = new EdgeItem(nodes_[nodes[edgeIdx[k].first]], nodes_[nodes[edgeIdx[k].second]], edgeRefs[k]->resolution);
        scene_->addItem(e);
        edges_.push_back(e);
    }
    scene_->setSceneRect(scene_->itemsBoundingRect().adjusted(-80, -80, 80, 80));
}

void GraphView::highlightChains(SymbolId id) {
    if (!snap_ || !nodes_.contains(id)) return clearHighlight();
    // Everything reachable downstream (callees) and upstream (callers) inside the visible graph.
    auto reach = [&](bool forward) {
        QSet<SymbolId> r;
        std::deque<SymbolId> q{id};
        while (!q.empty()) {
            SymbolId cur = q.front();
            q.pop_front();
            for (auto* e : edges_) {
                NodeItem* a = forward ? e->from : e->to;
                NodeItem* b = forward ? e->to : e->from;
                if (a->symbol == cur && !r.contains(b->symbol)) {
                    r.insert(b->symbol);
                    q.push_back(b->symbol);
                }
            }
        }
        return r;
    };
    QSet<SymbolId> down = reach(true), up = reach(false);
    for (auto* n : nodes_) {
        if (n->symbol == id) n->applyMark(Mark::Focus);
        else if (down.contains(n->symbol)) n->applyMark(Mark::Downstream);
        else if (up.contains(n->symbol)) n->applyMark(Mark::Upstream);
        else n->applyMark(Mark::Dimmed);
    }
    for (auto* e : edges_) {
        bool d = (e->from->symbol == id || down.contains(e->from->symbol)) && down.contains(e->to->symbol);
        bool u = (e->to->symbol == id || up.contains(e->to->symbol)) && up.contains(e->from->symbol);
        e->applyMark(d ? Mark::Downstream : u ? Mark::Upstream : Mark::Dimmed);
    }
    for (auto* n : nodes_) n->setSelected(n->symbol == id);
}

void GraphView::clearHighlight() {
    for (auto* n : nodes_) n->applyMark(Mark::None);
    for (auto* e : edges_) e->applyMark(Mark::None);
}

void GraphView::fitAll() {
    if (scene_->items().isEmpty()) return;
    fitInView(scene_->itemsBoundingRect().adjusted(-40, -40, 40, 40), Qt::KeepAspectRatio);
    // Do not blow small graphs up beyond 1:1.
    if (transform().m11() > 1.0) setTransform(QTransform());
}

void GraphView::zoomBy(double factor) {
    double current = transform().m11();
    double next = std::clamp(current * factor, 0.05, 4.0);
    scale(next / current, next / current);
}

void GraphView::wheelEvent(QWheelEvent* event) {
    if (event->modifiers() & (Qt::ControlModifier | Qt::MetaModifier) || event->angleDelta().y() != 0) {
        zoomBy(std::pow(1.0015, event->angleDelta().y()));
        event->accept();
        return;
    }
    QGraphicsView::wheelEvent(event);
}

NodeItem* GraphView::nodeAt(const QPoint& viewPos) const {
    for (QGraphicsItem* it : items(viewPos)) {
        if (auto* n = dynamic_cast<NodeItem*>(it)) return n;
        if (auto* n = dynamic_cast<NodeItem*>(it->parentItem())) return n;
    }
    return nullptr;
}

void GraphView::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        if (NodeItem* n = nodeAt(event->position().toPoint())) {
            highlightChains(n->symbol);
            emit symbolClicked(n->symbol);
            event->accept();
            return;
        }
    }
    QGraphicsView::mousePressEvent(event);
}

void GraphView::mouseDoubleClickEvent(QMouseEvent* event) {
    if (NodeItem* n = nodeAt(event->position().toPoint())) {
        emit symbolDoubleClicked(n->symbol);
        event->accept();
        return;
    }
    QGraphicsView::mouseDoubleClickEvent(event);
}

void GraphView::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Plus || event->key() == Qt::Key_Equal) return zoomBy(1.2);
    if (event->key() == Qt::Key_Minus) return zoomBy(1 / 1.2);
    if (event->key() == Qt::Key_0) return fitAll();
    if (event->key() == Qt::Key_Escape) return clearHighlight();
    QGraphicsView::keyPressEvent(event);
}
