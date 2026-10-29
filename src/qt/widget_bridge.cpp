#include <QApplication>
#include <QBoxLayout>
#include <QCheckBox>
#include <QGridLayout>
#include <QLabel>
#include <QHeaderView>
#include <QScrollBar>
#include <QStackedWidget>
#include <QStandardItemModel>
#include <QStyledItemDelegate>
#include <QTextDocument>
#include <QTextCursor>
#include <QAbstractTextDocumentLayout>
#include <QPainter>
#include <QRegularExpression>
#include <QTreeView>
#include <QToolTip>
#include <QHelpEvent>
#include <QLineEdit>
#include <QSplitter>
#include <QComboBox>
#include <QProgressBar>
#include <QMouseEvent>
#include <QKeyEvent>
#include <QScreen>
#include <QSet>
#include <QScrollArea>
#include <QGroupBox>
#include <QPlainTextEdit>
#include <QShowEvent>
#include <QTimer>
#include <QMenu>
#include <QActionGroup>
#include <QCursor>
#include "widget_bridge.h"
#include "theme.h"
#include <cstdarg>
#undef g_object_unref

namespace {
QSet<void *> qtObjects;
QString pangoMarkup(QString text) {
    // Pango accepts both quote forms and sizes in 1/1024 points. Keep the
    // source plugins' typography when translating their markup to Qt HTML.
    const QRegularExpression spans("<span([^>]*)>");
    const QRegularExpression attributes(R"((\w+)\s*=\s*(["'])(.*?)\2)");
    auto matches = spans.globalMatch(text); QList<QPair<QString, QString>> replacements;
    const QMap<QString, QString> properties{{"foreground", "color"}, {"background", "background-color"},
        {"font_family", "font-family"}, {"family", "font-family"}, {"weight", "font-weight"}, {"style", "font-style"}};
    while (matches.hasNext()) {
        const auto match = matches.next(); QString style;
        auto attrs = attributes.globalMatch(match.captured(1));
        while (attrs.hasNext()) {
            const auto attr = attrs.next(); const QString key = attr.captured(1), value = attr.captured(3);
            if (properties.contains(key)) style += properties.value(key) + ':' + value.toHtmlEscaped() + ';';
            else if (key == "size") {
                bool numeric; const double size = value.toDouble(&numeric);
                style += "font-size:" + (numeric ? QString::number(size / 1024.0) + "pt" : value) + ';';
            } else if (key == "underline" && value != "none") style += "text-decoration:underline;";
        }
        replacements.append({match.captured(), "<span style=\"" + style + "\">"});
    }
    for (const auto &pair : replacements) text.replace(pair.first, pair.second);
    return text.replace('\n', "<br>");
}
class InspectorLabel : public QLabel {
public:
    using QLabel::QLabel;
    int maximumCharacters = -1;
    int requestedWidth() const {
        return std::max(1, std::min(qCeil(QFontMetricsF(font()).horizontalAdvance(text())) + 2,
            fontMetrics().averageCharWidth() * maximumCharacters));
    }
    int heightForWidth(int width) const override {
        if (wordWrap() && maximumCharacters >= 0) width = std::min(width, requestedWidth());
        return QLabel::heightForWidth(width);
    }
    QSize sizeHint() const override {
        if (!wordWrap() || maximumCharacters < 0) return QLabel::sizeHint();
        // GTK's max-width-chars limits the requested width, not allocation.
        // Keep the natural text width until wrapping is actually necessary.
        const int width = requestedWidth();
        return {width, heightForWidth(width)};
    }
};
class ChartWidget : public QWidget {
public:
    QMap<gulong, QPair<GCallback, void *>> draws;
    GCallback motion = nullptr, leave = nullptr;
    void *motionData = nullptr, *leaveData = nullptr;
    void paintEvent(QPaintEvent *) override {
        if (draws.isEmpty() || size().isEmpty()) return;
        const qreal scale = devicePixelRatioF();
        QImage buffer(size() * scale, QImage::Format_ARGB32_Premultiplied); buffer.fill(Qt::transparent);
        auto *surface = cairo_image_surface_create_for_data(buffer.bits(), CAIRO_FORMAT_ARGB32, buffer.width(), buffer.height(), buffer.bytesPerLine());
        auto *cr = cairo_create(surface); cairo_scale(cr, scale, scale);
        for (auto it = draws.cbegin(); it != draws.cend(); ++it)
            if (reinterpret_cast<gboolean (*)(GtkWidget *, cairo_t *, gpointer)>(it.value().first)(this, cr, it.value().second)) break;
        cairo_destroy(cr); cairo_surface_flush(surface); cairo_surface_destroy(surface);
        buffer.setDevicePixelRatio(scale); QPainter painter(this); painter.drawImage(QPoint(0, 0), buffer);
    }
    void mouseMoveEvent(QMouseEvent *event) override {
        if (motion) { GdkEventMotion e{event->position().x(), event->position().y()}; reinterpret_cast<gboolean (*)(GtkWidget *, GdkEventMotion *, gpointer)>(motion)(this, &e, motionData); }
    }
    void leaveEvent(QEvent *) override {
        if (leave) { GdkEventCrossing e{}; reinterpret_cast<gboolean (*)(GtkWidget *, GdkEventCrossing *, gpointer)>(leave)(this, &e, leaveData); }
    }
};
class ButtonFilter : public QObject {
public:
    QWidget *target;
    GCallback callback;
    void *data;
    ButtonFilter(QWidget *target, GCallback callback, void *data) : QObject(target), target(target), callback(callback), data(data) {}
    bool eventFilter(QObject *, QEvent *event) override {
        if (event->type() != QEvent::MouseButtonPress) return false;
        auto *mouse = static_cast<QMouseEvent *>(event);
        GdkEventButton e{GDK_BUTTON_PRESS, mouse->button() == Qt::RightButton ? 3u : mouse->button() == Qt::MiddleButton ? 2u : 1u, mouse->position().x(), mouse->position().y()};
        return reinterpret_cast<gboolean (*)(GtkWidget *, GdkEventButton *, gpointer)>(callback)(target, &e, data);
    }
};
class InspectorSplitter : public QSplitter {
public:
    using QSplitter::QSplitter;
    int initialPosition = -1;
    bool initialized = false;
    void showEvent(QShowEvent *event) override {
        QSplitter::showEvent(event);
        if (!initialized && count() == 2) {
            initialized = true;
            QTimer::singleShot(0, this, [this] {
                const int available = (orientation() == Qt::Horizontal ? width() : height()) - handleWidth();
                const int position = initialPosition >= 0 ? initialPosition : available / 2;
                setSizes({position, std::max(1, available - position)});
            });
        }
    }
};
class FlagLayout : public QLayout {
public:
    explicit FlagLayout(QWidget *parent) : QLayout(parent) { setContentsMargins(0, 0, 0, 0); }
    ~FlagLayout() override { while (auto *i = takeAt(0)) delete i; }
    QList<QLayoutItem *> items;
    int rowSpacing = 2, columnSpacing = 6, minColumns = 4, maxColumns = 16;
    void addItem(QLayoutItem *i) override { items.append(i); }
    int count() const override { return items.size(); }
    QLayoutItem *itemAt(int i) const override { return items.value(i); }
    QLayoutItem *takeAt(int i) override { return i >= 0 && i < items.size() ? items.takeAt(i) : nullptr; }
    bool hasHeightForWidth() const override { return true; }
    int heightForWidth(int width) const override { return arrange(QRect(0, 0, width, 0), false); }
    QSize sizeHint() const override { return {300, heightForWidth(300)}; }
    QSize minimumSize() const override { return {0, 0}; }
    void setGeometry(const QRect &rect) override { QLayout::setGeometry(rect); arrange(rect, true); }
    int arrange(const QRect &rect, bool apply) const {
        if (items.isEmpty()) return 0;
        int columns = std::min(maxColumns, int(items.size())), totalWidth = 0;
        QList<int> widths;
        for (; columns > 0; --columns) {
            widths.fill(0, columns);
            for (int i = 0; i < items.size(); ++i)
                widths[i % columns] = std::max(widths[i % columns], items[i]->sizeHint().width() + 6);
            totalWidth = (columns - 1) * columnSpacing;
            for (int width : widths) totalWidth += width;
            if (totalWidth <= rect.width() || columns <= minColumns) break;
        }
        const int extra = std::max(0, rect.width() - totalWidth);
        for (int c = 0; c < columns; ++c) widths[c] += extra / columns + (c < extra % columns);
        int y = rect.y();
        for (int first = 0; first < items.size(); first += columns) {
            int rowHeight = 0, x = rect.x();
            for (int c = 0; c < columns && first + c < items.size(); ++c)
                rowHeight = std::max(rowHeight, items[first + c]->sizeHint().height() + 6);
            for (int c = 0; c < columns && first + c < items.size(); ++c) {
                auto *item = items[first + c]; const QSize size = item->sizeHint();
                if (apply) item->setGeometry(QRect(x + (widths[c] - size.width()) / 2,
                    y + (rowHeight - size.height()) / 2, size.width(), size.height()));
                x += widths[c] + columnSpacing;
            }
            y += rowHeight + rowSpacing;
        }
        return y - rect.y() - rowSpacing;
    }
};
constexpr int ValueRole = Qt::UserRole + 100;
class InspectorModel : public QStandardItemModel {
public:
    bool listOnly = false;
    QList<GType> types;
    QList<QObject *> columns;
};
class InspectorDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    void paint(QPainter *p, const QStyleOptionViewItem &option, const QModelIndex &index) const override {
        auto o = option; initStyleOption(&o, index); o.text.clear();
        QApplication::style()->drawControl(QStyle::CE_ItemViewItem, &o, p, o.widget);
        auto *model = static_cast<const InspectorModel *>(index.model());
        auto *column = model->columns.value(index.column());
        if (!column) return;
        if (column->property("level-l").isValid()) {
            const auto canonical = index.siblingAtColumn(0);
            const int levels[] = {canonical.data(ValueRole + column->property("level-l").toInt()).toInt(), canonical.data(ValueRole + column->property("level-r").toInt()).toInt()};
            if (!levels[0] && !levels[1]) return;
            const int x = o.rect.left() + 2, y = o.rect.top() + (o.rect.height() - 11) / 2;
            p->save(); p->setClipRect(o.rect);
            for (int ch = 0; ch < 2; ++ch) {
                p->fillRect(x, y + ch * 6, 60, 5, QColor::fromRgbF(.15, .15, .15));
                for (int px = 0; px < qRound(std::clamp(levels[ch] / 1000.0, 0.0, 1.0) * 60); ++px) {
                    const double level = px / 60.0; const double t = level < .6 ? level / .6 : (level - .6) / .4;
                    p->fillRect(x + px, y + ch * 6, 1, 5, QColor::fromRgbF(level < .6 ? t : .9 + .1 * t, level < .6 ? .8 : .8 * (1 - t), level < .6 ? .1 : .05));
                }
            }
            p->restore(); return;
        }
        QString markup = index.siblingAtColumn(0).data(ValueRole + column->property("sourceColumn").toInt()).toString();
        if (!column->property("isMarkup").toBool()) markup = markup.toHtmlEscaped();
        markup = pangoMarkup(markup);
        if (column->property("foreground").isValid()) markup = "<span style=\"color:" + column->property("foreground").toString() + "\">" + markup + "</span>";
        if (column->property("family").isValid()) o.font.setFamily(column->property("family").toString());
        QTextDocument doc; doc.setDocumentMargin(0); doc.setDefaultFont(o.font); doc.setHtml(markup);
        const int available = std::max(0, o.rect.width() - 4);
        if (column->property("ellipsize").toInt() && doc.idealWidth() > available) {
            const qreal end = std::max(0, available - o.fontMetrics.horizontalAdvance(QStringLiteral("…")));
            const int position = doc.documentLayout()->hitTest(QPointF(end, doc.size().height() / 2), Qt::FuzzyHit);
            QTextCursor cursor(&doc); cursor.setPosition(std::max(0, position));
            cursor.movePosition(QTextCursor::End, QTextCursor::KeepAnchor); cursor.insertText(QStringLiteral("…"));
        }
        QAbstractTextDocumentLayout::PaintContext context;
        context.palette = o.palette;
        if (o.state & QStyle::State_Selected) context.palette.setColor(QPalette::Text, o.palette.highlightedText().color());
        const qreal xalign = column->property("xalign").toDouble();
        p->save(); p->setClipRect(o.rect); p->translate(o.rect.left() + 2 + std::max(0.0, o.rect.width() - 4 - doc.size().width()) * xalign, o.rect.top() + (o.rect.height() - doc.size().height()) / 2);
        doc.documentLayout()->draw(p, context); p->restore();
    }
};
class InspectorTree : public QTreeView {
public:
    bool treeLines = false;
    void drawBranches(QPainter *painter, const QRect &rect, const QModelIndex &index) const override {
        if (treeLines) { QTreeView::drawBranches(painter, rect, index); return; }
        if (!model()->hasChildren(index)) return;
        const int x = layoutDirection() == Qt::RightToLeft ? rect.left() + indentation() / 2 : rect.right() - indentation() / 2;
        const int y = rect.center().y();
        QPolygon arrow = isExpanded(index) ? QPolygon{{x - 4, y - 2}, {x + 4, y - 2}, {x, y + 3}}
            : layoutDirection() == Qt::RightToLeft ? QPolygon{{x + 2, y - 4}, {x + 2, y + 4}, {x - 3, y}}
            : QPolygon{{x - 2, y - 4}, {x - 2, y + 4}, {x + 3, y}};
        painter->save(); painter->setPen(Qt::NoPen);
        QColor color = palette().color(QPalette::Text); color.setAlpha(160);
        painter->setBrush(color); painter->drawPolygon(arrow); painter->restore();
    }
    GCallback keypress = nullptr;
    void *keyContext = nullptr;
    void keyPressEvent(QKeyEvent *event) override {
        if (keypress && (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter)) {
            GdkEventKey key{GDK_KEY_Return};
            if (reinterpret_cast<gboolean (*)(GtkWidget *, GdkEventKey *, gpointer)>(keypress)(this, &key, keyContext)) return;
        }
        QTreeView::keyPressEvent(event);
    }
    GCallback tooltip = nullptr;
    void *tooltipContext = nullptr;
    bool viewportEvent(QEvent *event) override {
        if (event->type() == QEvent::ToolTip && tooltip) {
            auto *help = static_cast<QHelpEvent *>(event); QString text;
            using Callback = gboolean (*)(GtkWidget *, gint, gint, gboolean, GtkTooltip *, gpointer);
            if (reinterpret_cast<Callback>(tooltip)(this, help->pos().x(), help->pos().y(), false, &text, tooltipContext)) {
                QToolTip::showText(help->globalPos(), text, this); return true;
            }
        }
        return QTreeView::viewportEvent(event);
    }
};
QWidget *widget(void *p) { return static_cast<QWidget *>(p); }
QStandardItem *item(GtkTreeIter *i) { return i ? static_cast<QStandardItem *>(i->item) : nullptr; }
QStandardItem *parentItem(InspectorModel *m, QStandardItem *i) { return i && i->parent() ? i->parent() : m->invisibleRootItem(); }
void margin(void *p, int side, int value) {
    auto *w = widget(p); auto margins = w->contentsMargins();
    if (side == 0) margins.setLeft(value); else if (side == 1) margins.setTop(value); else if (side == 2) margins.setRight(value); else margins.setBottom(value);
    w->setContentsMargins(margins);
}
}
extern "C" {
void qt_inspector_object_set(void *p, const char *property, ...) {
    auto *object = static_cast<QObject *>(p); va_list args; va_start(args, property);
    for (const char *name = property; name; name = va_arg(args, const char *)) {
        const QByteArray key(name);
        if (key == "foreground" || key == "family") object->setProperty(name, QString::fromUtf8(va_arg(args, const char *)));
        else if (key == "xalign" || key == "yalign") object->setProperty(name, va_arg(args, double));
        else object->setProperty(name, va_arg(args, int));
    }
    va_end(args);
}
void qt_inspector_unref(void *p) {
    if (qtObjects.contains(p)) return; // Qt models transfer to their view.
    if (p) g_object_unref(p); // Pango layouts remain real reference-counted objects.
}
gulong qt_inspector_connect(void *object, const char *signal, GCallback callback, void *data) {
    const QByteArray name(signal);
    if (name == "draw" || name == "motion-notify-event" || name == "leave-notify-event") {
        auto *chart = static_cast<ChartWidget *>(object);
        if (name == "draw") { static gulong serial = 0; const gulong id = ++serial; chart->draws.insert(id, {callback, data}); return id; }
        else if (name == "motion-notify-event") { chart->motion = callback; chart->motionData = data; }
        else { chart->leave = callback; chart->leaveData = data; }
        return 0;
    }
    if (name == "changed") {
        if (auto *combo = qobject_cast<QComboBox *>(static_cast<QObject *>(object))) {
            QObject::connect(combo, &QComboBox::currentIndexChanged, combo, [combo, callback, data] { reinterpret_cast<void (*)(GtkComboBox *, gpointer)>(callback)(combo, data); });
            return 0;
        }
        auto *selection = static_cast<QItemSelectionModel *>(object);
        QObject::connect(selection, &QItemSelectionModel::selectionChanged, selection, [selection, callback, data] {
            reinterpret_cast<void (*)(GtkTreeSelection *, gpointer)>(callback)(selection, data);
        });
        return 0;
    }
    if (name == "button-press-event") {
        auto *target = widget(object); auto *filter = new ButtonFilter(target, callback, data);
        if (auto *view = qobject_cast<QAbstractItemView *>(target)) view->viewport()->installEventFilter(filter);
        else target->installEventFilter(filter);
        return 0;
    }
    if (name == "activate" || name == "toggled") {
        auto *action = qobject_cast<QAction *>(static_cast<QObject *>(object));
        if (action) {
            auto invoke = [action, callback, data] { reinterpret_cast<void (*)(GtkMenuItem *, gpointer)>(callback)(action, data); };
            if (name == "activate") QObject::connect(action, &QAction::triggered, action, invoke);
            else QObject::connect(action, &QAction::toggled, action, invoke);
        }
        return 0;
    }
    auto *tree = static_cast<InspectorTree *>(object);
    if (name == "row-activated") {
        QObject::connect(tree, &QTreeView::activated, tree, [tree, callback, data](const QModelIndex &index) {
            auto *m = static_cast<InspectorModel *>(tree->model()); GtkTreePath path{m->itemFromIndex(index.siblingAtColumn(0))};
            reinterpret_cast<void (*)(GtkTreeView *, GtkTreePath *, GtkTreeViewColumn *, gpointer)>(callback)(tree, &path, nullptr, data);
        }); return 0;
    }
    if (name == "key-press-event") { tree->keypress = callback; tree->keyContext = data; return 0; }
    if (name == "query-tooltip") { tree->tooltip = callback; tree->tooltipContext = data; return 0; }
    auto invoke = [tree, callback, data](const QModelIndex &index) {
        auto *model = static_cast<InspectorModel *>(tree->model());
        GtkTreeIter iter{model->itemFromIndex(index)}; GtkTreePath path{iter.item};
        using Callback = void (*)(GtkTreeView *, GtkTreeIter *, GtkTreePath *, gpointer);
        reinterpret_cast<Callback>(callback)(tree, &iter, &path, data);
    };
    if (name == "row-expanded") QObject::connect(tree, &QTreeView::expanded, tree, invoke);
    else if (name == "row-collapsed") QObject::connect(tree, &QTreeView::collapsed, tree, invoke);
    return 0;
}
GtkWidget *gtk_box_new(int orientation, int spacing) {
    auto *w = new QWidget; auto *layout = new QBoxLayout(orientation ? QBoxLayout::TopToBottom : QBoxLayout::LeftToRight, w);
    layout->setContentsMargins(0, 0, 0, 0); layout->setSpacing(spacing);
    layout->addStretch(); w->setProperty("packingSpacer", true); return w;
}
void gtk_box_pack_start(void *box, GtkWidget *child, gboolean expand, gboolean fill, guint) {
    auto *w = widget(box); auto *layout = static_cast<QBoxLayout *>(w->layout());
    if (expand && w->property("packingSpacer").toBool()) {
        delete layout->takeAt(layout->count() - 1); w->setProperty("packingSpacer", false);
    }
    const int position = layout->count() - int(w->property("packingSpacer").toBool());
    const bool vertical = layout->direction() == QBoxLayout::TopToBottom;
    const Qt::Alignment alignment = !expand ? (vertical ? Qt::AlignTop : Qt::AlignLeft)
        : !fill ? (vertical ? Qt::AlignVCenter : Qt::AlignHCenter) : Qt::Alignment();
    layout->insertWidget(position, widget(child), expand ? 1 : 0, alignment);
}
GtkWidget *gtk_check_button_new_with_label(const char *text) { return new QCheckBox(QString::fromUtf8(text)); }
void gtk_toggle_button_set_active(GtkToggleButton *p, gboolean active) { static_cast<QCheckBox *>(p)->setChecked(active); }
gboolean gtk_toggle_button_get_active(GtkToggleButton *p) { return static_cast<QCheckBox *>(p)->isChecked(); }
GtkWidget *gtk_scrolled_window_new(void *, void *) { auto *s = new QScrollArea; s->setWidgetResizable(true); s->setFrameShape(QFrame::NoFrame); return s; }
void gtk_scrolled_window_set_policy(void *p, int h, int v) { auto *s = static_cast<QScrollArea *>(p); s->setHorizontalScrollBarPolicy(h ? Qt::ScrollBarAlwaysOff : Qt::ScrollBarAsNeeded); s->setVerticalScrollBarPolicy(v ? Qt::ScrollBarAlwaysOff : Qt::ScrollBarAsNeeded); }
void gtk_container_add(void *p, GtkWidget *child) {
    if (auto *scroll = qobject_cast<QScrollArea *>(widget(p))) scroll->setWidget(widget(child));
    else widget(p)->layout()->addWidget(widget(child));
}
GtkAdjustment *gtk_scrolled_window_get_vadjustment(void *p) { const auto list = widget(p)->findChildren<QTreeView *>(); return list.isEmpty() ? static_cast<QScrollArea *>(p)->verticalScrollBar() : list.first()->verticalScrollBar(); }
double gtk_adjustment_get_value(GtkAdjustment *p) { return p ? static_cast<QScrollBar *>(p)->value() : 0; }
void gtk_adjustment_set_value(GtkAdjustment *p, double v) { if (p) static_cast<QScrollBar *>(p)->setValue(int(v)); }
GtkWidget *gtk_grid_new() { auto *w = new QWidget; auto *l = new QGridLayout(w); l->setContentsMargins(0, 0, 0, 0); l->setAlignment(Qt::AlignTop | Qt::AlignLeft); return w; }
void gtk_grid_set_column_spacing(GtkGrid *p, int v) { static_cast<QGridLayout *>(widget(p)->layout())->setHorizontalSpacing(v); }
void gtk_grid_set_row_spacing(GtkGrid *p, int v) { static_cast<QGridLayout *>(widget(p)->layout())->setVerticalSpacing(v); }
void gtk_grid_attach(GtkGrid *p, GtkWidget *w, int col, int row, int cs, int rs) {
    auto *grid = static_cast<QGridLayout *>(widget(p)->layout());
    grid->addWidget(widget(w), row, col, rs, cs,
        Qt::Alignment(widget(w)->property("horizontalAlignment").toInt()));
    if (widget(w)->sizePolicy().horizontalPolicy() == QSizePolicy::Expanding) {
        grid->setAlignment(Qt::AlignTop);
        widget(p)->setProperty(("expandColumn" + QByteArray::number(col)).constData(), true);
    }
    // Keep key columns at their natural width and give the value column the
    // remaining space, while retaining explicitly expanding dashboard columns.
    for (int c = 0; c < grid->columnCount(); ++c)
        grid->setColumnStretch(c, c == grid->columnCount() - 1 ||
            widget(p)->property(("expandColumn" + QByteArray::number(c)).constData()).toBool());
}
GtkWidget *gtk_grid_get_child_at(GtkGrid *p, int col, int row) { auto *i = static_cast<QGridLayout *>(widget(p)->layout())->itemAtPosition(row, col); return i ? i->widget() : nullptr; }
GtkWidget *gtk_label_new(const char *s) { auto *label = new InspectorLabel(QString::fromUtf8(s ? s : "")); label->setTextFormat(Qt::PlainText); return label; }
void gtk_label_set_text(GtkLabel *p, const char *s) { static_cast<QLabel *>(p)->setText(QString::fromUtf8(s)); }
void gtk_label_set_xalign(GtkLabel *p, float x) { static_cast<QLabel *>(p)->setAlignment((x >= 0.75f ? Qt::AlignRight : x >= 0.25f ? Qt::AlignHCenter : Qt::AlignLeft) | Qt::AlignVCenter); }
void gtk_label_set_selectable(GtkLabel *p, gboolean v) { static_cast<QLabel *>(p)->setTextInteractionFlags(v ? Qt::TextSelectableByMouse : Qt::NoTextInteraction); }
GtkWidget *gtk_stack_new() { return new QStackedWidget; }
void gtk_stack_add_named(void *p, GtkWidget *w, const char *name) { widget(w)->setObjectName(name); static_cast<QStackedWidget *>(p)->addWidget(widget(w)); }
void gtk_stack_set_visible_child_name(void *p, const char *name) { auto *s = static_cast<QStackedWidget *>(p); for (int i = 0; i < s->count(); ++i) if (s->widget(i)->objectName() == QLatin1String(name)) s->setCurrentIndex(i); }
void gtk_widget_set_margin_start(GtkWidget *p, int v) { margin(p, 0, v); }
void gtk_widget_set_margin_end(GtkWidget *p, int v) { margin(p, 2, v); }
void gtk_widget_set_margin_top(GtkWidget *p, int v) { margin(p, 1, v); }
void gtk_widget_set_margin_bottom(GtkWidget *p, int v) { margin(p, 3, v); }
void gtk_widget_set_halign(GtkWidget *p, int align) {
    if (auto *label = qobject_cast<QLabel *>(widget(p))) {
        const auto horizontal = align == GTK_ALIGN_CENTER ? Qt::AlignHCenter : Qt::AlignLeft;
        label->setAlignment((label->alignment() & Qt::AlignVertical_Mask) | horizontal);
        label->setProperty("horizontalAlignment", int(horizontal));
        if (label->parentWidget() && label->parentWidget()->layout())
            label->parentWidget()->layout()->setAlignment(label, horizontal);
    }
}
void gtk_widget_set_valign(GtkWidget *p, int align) { if (auto *l = qobject_cast<QLabel *>(widget(p))) l->setAlignment((l->alignment() & Qt::AlignHorizontal_Mask) | (align == GTK_ALIGN_CENTER ? Qt::AlignVCenter : Qt::AlignTop)); }
void gtk_widget_set_vexpand(GtkWidget *p, gboolean expand) { auto policy = widget(p)->sizePolicy(); policy.setVerticalPolicy(expand ? QSizePolicy::Expanding : QSizePolicy::Preferred); widget(p)->setSizePolicy(policy); }
void gtk_widget_set_has_tooltip(GtkWidget *p, gboolean) { widget(p)->setAttribute(Qt::WA_AlwaysShowToolTips); }
void gtk_widget_show(GtkWidget *p) { widget(p)->show(); }
void gtk_widget_hide(GtkWidget *p) { widget(p)->hide(); }
void gtk_widget_show_all(GtkWidget *p) {
    if (qobject_cast<QAction *>(static_cast<QObject *>(p))) return;
    if (qobject_cast<QMenu *>(widget(p))) return;
    if (widget(p)->property("noShowAll").toBool()) return;
    if (widget(p)->parentWidget()) widget(p)->show();
}
gboolean gtk_widget_get_mapped(GtkWidget *p) { return widget(p)->isVisible(); }
gboolean gtk_widget_get_child_visible(GtkWidget *p) { return widget(p)->isVisible(); }
GtkTreeStore *gtk_tree_store_new(int count, ...) {
    auto *m = new InspectorModel; qtObjects.insert(m); QObject::connect(m, &QObject::destroyed, [m] { qtObjects.remove(m); }); va_list args; va_start(args, count); for (int i = 0; i < count; ++i) m->types.append(va_arg(args, GType)); va_end(args); m->setColumnCount(0); return m;
}
GtkListStore *gtk_list_store_new(int count, ...) {
    auto *m = new InspectorModel; m->listOnly = true;
    qtObjects.insert(m); QObject::connect(m, &QObject::destroyed, [m] { qtObjects.remove(m); });
    va_list args; va_start(args, count);
    for (int i = 0; i < count; ++i) m->types.append(va_arg(args, GType));
    va_end(args); m->setColumnCount(0); return m;
}
void gtk_tree_store_append(GtkTreeStore *p, GtkTreeIter *out, GtkTreeIter *parent) {
    auto *m = static_cast<InspectorModel *>(p); QList<QStandardItem *> cells;
    for (int c = 0; c < std::max(1, m->columnCount()); ++c) { auto *cell = new QStandardItem; cell->setEditable(false); cells.append(cell); }
    (parent ? item(parent) : m->invisibleRootItem())->appendRow(cells); out->item = cells.first();
}
void gtk_tree_store_set(GtkTreeStore *p, GtkTreeIter *iter, ...) {
    auto *m = static_cast<InspectorModel *>(p); auto *i = item(iter); va_list args; va_start(args, iter); int col;
    while ((col = va_arg(args, int)) != -1) {
        const auto type = m->types.value(col); QVariant value;
        if (type == G_TYPE_STRING) { const char *s = va_arg(args, const char *); value = QString::fromUtf8(s ? s : ""); }
        else if (type == G_TYPE_INT64) value = qlonglong(va_arg(args, gint64));
        else value = va_arg(args, int);
        if (i->data(ValueRole + col) != value) i->setData(value, ValueRole + col);
        if (col == 0) { i->setText(value.toString()); i->setToolTip(value.toString()); }
    }
    va_end(args);
    const auto idx = i->index(); emit m->dataChanged(idx, idx.siblingAtColumn(m->columnCount() - 1));
}
void gtk_tree_store_clear(GtkTreeStore *p) { auto *m = static_cast<InspectorModel *>(p); m->removeRows(0, m->rowCount()); }
gboolean gtk_tree_store_remove(GtkTreeStore *p, GtkTreeIter *iter) {
    auto *m = static_cast<InspectorModel *>(p); auto *i = item(iter); auto *parent = parentItem(m, i); const int row = i->row(); parent->removeRow(row); iter->item = parent->child(row); return iter->item != nullptr;
}
gboolean gtk_tree_model_iter_children(GtkTreeModel *p, GtkTreeIter *out, GtkTreeIter *parent) {
    auto *m = static_cast<InspectorModel *>(p); auto *i = parent ? item(parent) : m->invisibleRootItem(); out->item = i ? i->child(0) : nullptr; return out->item != nullptr;
}
gboolean gtk_tree_model_iter_next(GtkTreeModel *p, GtkTreeIter *iter) { auto *i = item(iter); iter->item = i ? parentItem(static_cast<InspectorModel *>(p), i)->child(i->row() + 1) : nullptr; return iter->item != nullptr; }
void gtk_tree_model_get(GtkTreeModel *p, GtkTreeIter *iter, ...) {
    auto *m = static_cast<InspectorModel *>(p); va_list args; va_start(args, iter); int col;
    while ((col = va_arg(args, int)) != -1) {
        void *out = va_arg(args, void *); const auto value = item(iter)->data(ValueRole + col);
        if (m->types.value(col) == G_TYPE_STRING) *static_cast<char **>(out) = g_strdup(value.toString().toUtf8().constData());
        else if (m->types.value(col) == G_TYPE_INT64) *static_cast<gint64 *>(out) = value.toLongLong();
        else *static_cast<int *>(out) = value.toInt();
    } va_end(args);
}
GtkTreePath *gtk_tree_model_get_path(GtkTreeModel *, GtkTreeIter *iter) { return new GtkTreePath{iter->item}; }
void gtk_tree_path_free(GtkTreePath *p) { delete p; }
GtkWidget *gtk_tree_view_new_with_model(GtkTreeModel *p) {
    auto *tree = new InspectorTree; auto *m = static_cast<InspectorModel *>(p); m->setParent(tree); tree->setModel(m);
    tree->setRootIsDecorated(!m->listOnly);
    QFont headerFont; headerFont.setBold(true); tree->header()->setFont(headerFont);
    tree->setItemDelegate(new InspectorDelegate(tree)); tree->setUniformRowHeights(true); tree->setIndentation(14); tree->setFrameShape(QFrame::NoFrame); tree->setEditTriggers(QAbstractItemView::NoEditTriggers); tree->header()->setStretchLastSection(false); return tree;
}
void gtk_tree_view_set_headers_visible(GtkTreeView *p, gboolean visible) { static_cast<QTreeView *>(p)->setHeaderHidden(!visible); }
void gtk_tree_view_set_enable_search(GtkTreeView *, gboolean) { /* No GTK incremental-search popup is created by the Qt view. */ }
GtkCellRenderer *gtk_cell_renderer_text_new() { return new QObject; }
GtkTreeViewColumn *gtk_tree_view_column_new_with_attributes(const char *title, GtkCellRenderer *cell, ...) {
    auto *column = static_cast<QObject *>(cell); column->setProperty("title", QString::fromUtf8(title));
    va_list args; va_start(args, cell); const char *attribute;
    while ((attribute = va_arg(args, const char *))) {
        const int col = va_arg(args, int);
        if (QByteArray(attribute).startsWith("level-")) column->setProperty(attribute, col);
        if (QByteArray(attribute) == "markup" || QByteArray(attribute) == "text") {
            column->setProperty("sourceColumn", col); column->setProperty("isMarkup", QByteArray(attribute) == "markup");
        }
    }
    va_end(args); return column;
}
void gtk_tree_view_append_column(GtkTreeView *p, GtkTreeViewColumn *column) {
    auto *c = static_cast<QObject *>(column); auto *tree = static_cast<QTreeView *>(p);
    auto *m = static_cast<InspectorModel *>(tree->model()); c->setParent(m); m->columns.append(c);
    const int n = m->columns.size() - 1; m->setColumnCount(n + 1); m->setHeaderData(n, Qt::Horizontal, c->property("title"));
    if (c->property("width").isValid()) tree->setColumnWidth(n, c->property("width").toInt());
    tree->header()->setSectionResizeMode(n, (c->property("expand").toBool() || !c->property("width").isValid()) ? QHeaderView::Stretch : c->property("fixed").toBool() ? QHeaderView::Fixed : QHeaderView::Interactive);
}
void gtk_tree_view_column_set_resizable(GtkTreeViewColumn *p, gboolean value) { static_cast<QObject *>(p)->setProperty("resizable", bool(value)); }
void gtk_tree_view_column_set_expand(GtkTreeViewColumn *p, gboolean value) { static_cast<QObject *>(p)->setProperty("expand", bool(value)); }
void gtk_tree_view_column_set_min_width(GtkTreeViewColumn *p, int width) { static_cast<QObject *>(p)->setProperty("width", width); }
GtkTreeSelection *gtk_tree_view_get_selection(GtkTreeView *p) { return static_cast<QTreeView *>(p)->selectionModel(); }
void gtk_tree_selection_set_mode(GtkTreeSelection *, int) {}
gboolean gtk_tree_selection_get_selected(GtkTreeSelection *p, GtkTreeModel **model, GtkTreeIter *iter) {
    auto *selection = static_cast<QItemSelectionModel *>(p); const auto rows = selection->selectedRows();
    if (rows.isEmpty()) return false;
    auto *m = static_cast<InspectorModel *>(selection->model()); if (model) *model = m; iter->item = m->itemFromIndex(rows.first()); return true;
}
gboolean gtk_tree_model_get_iter_first(GtkTreeModel *p, GtkTreeIter *iter) { return gtk_tree_model_iter_children(p, iter, nullptr); }
GtkWidget *gtk_search_entry_new() {
    auto *entry = new QLineEdit;
    entry->addAction(QIcon::fromTheme(QStringLiteral("edit-find")), QLineEdit::LeadingPosition);
    entry->setClearButtonEnabled(true);
    return entry;
}
void gtk_entry_set_placeholder_text(GtkEntry *p, const char *text) { static_cast<QLineEdit *>(p)->setPlaceholderText(QString::fromUtf8(text)); }
const char *gtk_entry_get_text(GtkEntry *p) {
    auto *entry = static_cast<QLineEdit *>(p);
    // The source reads this string immediately and does not retain it.
    thread_local QByteArray text; text = entry->text().toUtf8(); return text.constData();
}
GtkWidget *gtk_paned_new(int orientation) { auto *s = new InspectorSplitter(orientation ? Qt::Vertical : Qt::Horizontal); s->setChildrenCollapsible(false); return s; }
void gtk_paned_pack1(void *p, GtkWidget *child, gboolean resize, gboolean) {
    auto *splitter = static_cast<QSplitter *>(p);
    splitter->addWidget(widget(child));
    splitter->setStretchFactor(splitter->count() - 1, resize ? 1 : 0);
}
void gtk_paned_pack2(void *p, GtkWidget *child, gboolean a, gboolean b) { gtk_paned_pack1(p, child, a, b); }
void gtk_paned_set_position(void *p, int position) { static_cast<InspectorSplitter *>(p)->initialPosition = position; }
void gtk_widget_set_no_show_all(GtkWidget *p, gboolean value) {
    auto *w = widget(p);
    w->setProperty("noShowAll", bool(value));
    // Keep an initially hidden GTK child hidden when Qt shows its parent.
    // Changing this flag must never hide an already shown spectrum/media panel.
    if (value && w->isHidden()) w->hide();
}
void gtk_tree_view_expand_row(GtkTreeView *p, GtkTreePath *path, gboolean) { if (path->item) static_cast<QTreeView *>(p)->expand(static_cast<QStandardItem *>(path->item)->index()); }
void gtk_tree_view_collapse_row(GtkTreeView *p, GtkTreePath *path) { if (path->item) static_cast<QTreeView *>(p)->collapse(static_cast<QStandardItem *>(path->item)->index()); }
gboolean gtk_tree_view_get_tooltip_context(GtkTreeView *p, int *x, int *y, gboolean, GtkTreeModel **out, GtkTreePath **path, GtkTreeIter *iter) {
    auto *t = static_cast<QTreeView *>(p); const auto idx = t->indexAt(QPoint(*x, *y)); if (!idx.isValid()) return false;
    auto *m = static_cast<InspectorModel *>(t->model()); iter->item = m->itemFromIndex(idx.siblingAtColumn(0)); *out = m; *path = new GtkTreePath{iter->item}; return true;
}
void gtk_tree_view_set_tooltip_row(GtkTreeView *, GtkTooltip *, GtkTreePath *) {}
void gtk_tooltip_set_text(GtkTooltip *p, const char *text) { *static_cast<QString *>(p) = QString::fromUtf8(text); }
GtkWidget *gtk_drawing_area_new() { auto *w = new ChartWidget; w->setMouseTracking(true); return w; }
void gtk_widget_queue_draw(GtkWidget *p) { widget(p)->update(); }
int gtk_widget_get_allocated_width(GtkWidget *p) { return widget(p)->width(); }
int gtk_widget_get_allocated_height(GtkWidget *p) { return widget(p)->height(); }
void gtk_widget_add_events(GtkWidget *, int) {}
void gtk_widget_set_hexpand(GtkWidget *p, gboolean expand) { auto policy = widget(p)->sizePolicy(); policy.setHorizontalPolicy(expand ? QSizePolicy::Expanding : QSizePolicy::Preferred); widget(p)->setSizePolicy(policy); }
void gtk_widget_set_size_request(GtkWidget *p, int width, int height) { if (width >= 0) widget(p)->setMinimumWidth(width); if (height >= 0) widget(p)->setMinimumHeight(height); }
void gtk_widget_set_sensitive(GtkWidget *p, gboolean value) { if (auto *a = qobject_cast<QAction *>(static_cast<QObject *>(p))) a->setEnabled(value); else widget(p)->setEnabled(value); }
PangoContext *gtk_widget_get_pango_context(GtkWidget *p) {
    static PangoContext *context = pango_font_map_create_context(pango_cairo_font_map_get_default());
    auto *font = pango_font_description_new(); auto f = widget(p)->font();
    pango_font_description_set_family(font, f.family().toUtf8().constData());
    pango_font_description_set_absolute_size(font, QFontMetricsF(f).height() * PANGO_SCALE * 0.8);
    pango_context_set_font_description(context, font); pango_font_description_free(font); return context;
}
GdkScreen *gtk_widget_get_screen(GtkWidget *p) { return widget(p)->screen(); }
double gdk_screen_get_resolution(GdkScreen *p) { return static_cast<QScreen *>(p)->logicalDotsPerInch(); }
GtkWidget *gtk_combo_box_text_new() { return new QComboBox; }
void gtk_combo_box_text_append_text(void *p, const char *text) { static_cast<QComboBox *>(p)->addItem(QString::fromUtf8(text)); }
void gtk_combo_box_text_append(void *p, const char *id, const char *text) { static_cast<QComboBox *>(p)->addItem(QString::fromUtf8(text), QString::fromUtf8(id)); }
void gtk_combo_box_text_remove_all(void *p) { static_cast<QComboBox *>(p)->clear(); }
void gtk_combo_box_set_active(GtkComboBox *p, int index) { static_cast<QComboBox *>(p)->setCurrentIndex(index); }
int gtk_combo_box_get_active(GtkComboBox *p) { return static_cast<QComboBox *>(p)->currentIndex(); }
GtkWidget *gtk_event_box_new() { return gtk_box_new(1, 0); }
GtkWidget *gtk_frame_new(const char *title) {
    auto *frame = new QFrame; frame->setFrameShape(QFrame::StyledPanel);
    frame->setProperty("evemonFrame", true);
    auto *layout = new QVBoxLayout(frame); layout->setContentsMargins(0, 0, 0, 0); layout->setSpacing(4);
    if (title && *title) layout->addWidget(new QLabel(QString::fromUtf8(title)));
    return frame;
}
void gtk_container_set_border_width(void *p, int width) { widget(p)->setContentsMargins(width, width, width, width); }
GtkWidget *gtk_separator_new(int orientation) { auto *f = new QFrame; f->setFrameShape(orientation ? QFrame::VLine : QFrame::HLine); return f; }
GtkWidget *gtk_progress_bar_new() { auto *p = new QProgressBar; p->setRange(0, 1000); p->setValue(0); return p; }
void gtk_progress_bar_set_fraction(void *p, double value) { static_cast<QProgressBar *>(p)->setValue(qRound(std::clamp(value, 0.0, 1.0) * 1000)); }
void gtk_progress_bar_set_show_text(void *p, gboolean value) { auto *bar = static_cast<QProgressBar *>(p); bar->setTextVisible(value); if (!value) bar->setFixedHeight(4); }
void gtk_label_set_markup(GtkLabel *p, const char *text) { auto *label = static_cast<QLabel *>(p); label->setTextFormat(Qt::RichText); label->setText(pangoMarkup(QString::fromUtf8(text))); }
void gtk_label_set_ellipsize(GtkLabel *p, int) { widget(p)->setMinimumWidth(0); auto policy = widget(p)->sizePolicy(); policy.setHorizontalPolicy(QSizePolicy::Ignored); widget(p)->setSizePolicy(policy); }
void gtk_box_pack_end(void *p, GtkWidget *child, gboolean expand, gboolean fill, guint padding) {
    auto *layout = static_cast<QBoxLayout *>(widget(p)->layout());
    Q_UNUSED(padding);
    if (widget(p)->property("packingSpacer").toBool()) {
        widget(p)->setProperty("packingSpacer", false); widget(p)->setProperty("endPacking", true);
    }
    if (!widget(p)->property("endPacking").toBool()) { layout->addStretch(); widget(p)->setProperty("endPacking", true); }
    const bool vertical = layout->direction() == QBoxLayout::TopToBottom;
    const Qt::Alignment alignment = !expand ? (vertical ? Qt::AlignBottom : Qt::AlignRight)
        : !fill ? (vertical ? Qt::AlignVCenter : Qt::AlignHCenter) : Qt::Alignment();
    // GTK packs successive end children inward from the trailing edge.
    const int endCount = widget(p)->property("endPackingCount").toInt();
    layout->insertWidget(layout->count() - endCount, widget(child), expand ? 1 : 0, alignment);
    widget(p)->setProperty("endPackingCount", endCount + 1);
}
gboolean gtk_tree_view_row_expanded(GtkTreeView *p, GtkTreePath *path) { return static_cast<QTreeView *>(p)->isExpanded(static_cast<QStandardItem *>(path->item)->index()); }
void gtk_tree_view_set_enable_tree_lines(GtkTreeView *p, gboolean enabled) { static_cast<InspectorTree *>(p)->treeLines = enabled; }
GtkCssProvider *gtk_css_provider_new() { return g_object_new(G_TYPE_OBJECT, nullptr); }
void gtk_css_provider_load_from_data(GtkCssProvider *p, const char *data, gssize, void *) { g_object_set_data_full(G_OBJECT(p), "css", g_strdup(data), g_free); }
void *gtk_widget_get_style_context(GtkWidget *p) { return p; }
void gtk_style_context_add_provider(void *p, void *provider, int) {
    const QString css = QString::fromUtf8(static_cast<const char *>(g_object_get_data(G_OBJECT(provider), "css")));
    QFont font = widget(p)->font();
    const auto family = QRegularExpression("font-family:\\s*([^;}]+)", QRegularExpression::CaseInsensitiveOption).match(css);
    if (family.hasMatch()) font.setFamily(family.captured(1).trimmed().remove('"').remove('\''));
    const auto size = QRegularExpression("font-size:\\s*([0-9.]+)(pt|px)", QRegularExpression::CaseInsensitiveOption).match(css);
    if (size.hasMatch()) {
        if (size.captured(2) == "pt") font.setPointSizeF(size.captured(1).toDouble());
        else font.setPixelSize(qRound(size.captured(1).toDouble()));
    }
    widget(p)->setFont(font);
}
void gtk_style_context_add_class(void *p, const char *name) {
    if (QByteArray(name) == "dim-label") setDimmedText(widget(p));
}

void gtk_frame_set_shadow_type(void *p, int) { if (auto *frame = qobject_cast<QFrame *>(widget(p))) { frame->setFrameShape(QFrame::NoFrame); frame->setProperty("evemonFrame", false); } }
void gtk_label_set_line_wrap(GtkLabel *p, gboolean value) { static_cast<QLabel *>(p)->setWordWrap(value); }
void gtk_label_set_max_width_chars(GtkLabel *p, int chars) { auto *label = static_cast<InspectorLabel *>(p); label->maximumCharacters = chars; label->updateGeometry(); }
GtkWidget *gtk_flow_box_new() { auto *w = new QWidget; new FlagLayout(w); return w; }
void gtk_flow_box_set_selection_mode(void *, int) {}
void gtk_flow_box_set_min_children_per_line(void *p, int n) { static_cast<FlagLayout *>(widget(p)->layout())->minColumns = n; }
void gtk_flow_box_set_max_children_per_line(void *p, int n) { static_cast<FlagLayout *>(widget(p)->layout())->maxColumns = n; }
void gtk_flow_box_set_row_spacing(void *p, int n) { static_cast<FlagLayout *>(widget(p)->layout())->rowSpacing = n; }
void gtk_flow_box_set_column_spacing(void *p, int n) { static_cast<FlagLayout *>(widget(p)->layout())->columnSpacing = n; }
GList *gtk_container_get_children(void *p) { GList *list = nullptr; for (auto *w : widget(p)->findChildren<QWidget *>(QString(), Qt::FindDirectChildrenOnly)) list = g_list_append(list, w); return list; }
void gtk_widget_destroy(GtkWidget *p) { delete widget(p); }
void gtk_widget_set_tooltip_text(GtkWidget *p, const char *text) { widget(p)->setToolTip(QString::fromUtf8(text)); }
void gtk_scrolled_window_set_min_content_height(void *p, int height) { widget(p)->setMinimumHeight(height); }
GtkWidget *gtk_text_view_new() { auto *t = new QPlainTextEdit; t->setFrameShape(QFrame::NoFrame); return t; }
void gtk_text_view_set_editable(void *p, gboolean value) { static_cast<QPlainTextEdit *>(p)->setReadOnly(!value); }
void gtk_text_view_set_cursor_visible(void *p, gboolean value) { static_cast<QPlainTextEdit *>(p)->setCursorWidth(value ? 1 : 0); }
void gtk_text_view_set_wrap_mode(void *p, int) { static_cast<QPlainTextEdit *>(p)->setLineWrapMode(QPlainTextEdit::WidgetWidth); }
GtkTextBuffer *gtk_text_view_get_buffer(void *p) { return p; }
void gtk_text_buffer_set_text(GtkTextBuffer *p, const char *text, int length) { static_cast<QPlainTextEdit *>(p)->setPlainText(QString::fromUtf8(text, length)); }

gboolean qt_inspector_is_label(void *p) { return qobject_cast<QLabel *>(static_cast<QObject *>(p)) != nullptr; }
void *qt_inspector_ref_sink(void *p) { return p; }
void qt_inspector_disconnect(void *p, gulong id) { static_cast<ChartWidget *>(p)->draws.remove(id); }
void qt_inspector_block(void *p, gboolean value) { static_cast<QObject *>(p)->blockSignals(value); }
GtkWidget *gtk_image_new() { return new QLabel; }
void gtk_image_clear(void *p) { static_cast<QLabel *>(p)->clear(); }
void gtk_image_set_from_pixbuf(void *p, GdkPixbuf *pixbuf) {
    if (!pixbuf) { gtk_image_clear(p); return; }
    const QImage image(gdk_pixbuf_get_pixels(pixbuf), gdk_pixbuf_get_width(pixbuf), gdk_pixbuf_get_height(pixbuf), gdk_pixbuf_get_rowstride(pixbuf), gdk_pixbuf_get_has_alpha(pixbuf) ? QImage::Format_RGBA8888 : QImage::Format_RGB888);
    static_cast<QLabel *>(p)->setPixmap(QPixmap::fromImage(image.copy()));
}
void gtk_label_set_use_markup(GtkLabel *p, gboolean value) { static_cast<QLabel *>(p)->setTextFormat(value ? Qt::RichText : Qt::PlainText); }
void gtk_tree_view_column_set_fixed_width(GtkTreeViewColumn *p, int width) { static_cast<QObject *>(p)->setProperty("width", width); }
void gtk_tree_view_column_set_sizing(GtkTreeViewColumn *p, int) { static_cast<QObject *>(p)->setProperty("fixed", true); }
GtkTreeModel *gtk_tree_view_get_model(GtkTreeView *p) { return static_cast<QTreeView *>(p)->model(); }
gboolean gtk_tree_view_get_path_at_pos(GtkTreeView *p, int x, int y, GtkTreePath **path, void *, int *, int *) {
    auto *tree = static_cast<QTreeView *>(p); auto *m = static_cast<InspectorModel *>(tree->model()); auto index = tree->indexAt(QPoint(x, y)).siblingAtColumn(0);
    if (!index.isValid()) return false;
    *path = new GtkTreePath{m->itemFromIndex(index)}; return true;
}
gboolean gtk_tree_model_get_iter(GtkTreeModel *, GtkTreeIter *iter, GtkTreePath *path) { iter->item = path ? path->item : nullptr; return iter->item != nullptr; }
void gtk_tree_selection_select_iter(GtkTreeSelection *p, GtkTreeIter *iter) { if (iter->item) static_cast<QItemSelectionModel *>(p)->setCurrentIndex(item(iter)->index(), QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows); }
GtkWidget *gtk_menu_new() { return new QMenu; }
GtkWidget *gtk_menu_item_new_with_label(const char *text) { return new QAction(QString::fromUtf8(text)); }
GtkWidget *gtk_separator_menu_item_new() { auto *a = new QAction; a->setSeparator(true); return a; }
GtkWidget *gtk_radio_menu_item_new_with_label(GSList *group, const char *text) {
    if (!group) { auto *g = new QActionGroup(qApp); group = g_slist_append(nullptr, g); QObject::connect(g, &QObject::destroyed, [group] { g_slist_free(group); }); }
    auto *g = static_cast<QActionGroup *>(group->data); auto *a = new QAction(QString::fromUtf8(text), g); a->setCheckable(true); g->addAction(a); a->setProperty("radioGroup", qulonglong(reinterpret_cast<quintptr>(group))); return a;
}
GSList *gtk_radio_menu_item_get_group(void *p) { return reinterpret_cast<GSList *>(static_cast<QAction *>(p)->property("radioGroup").toULongLong()); }
void gtk_check_menu_item_set_active(GtkCheckMenuItem *p, gboolean value) { static_cast<QAction *>(p)->setChecked(value); }
gboolean gtk_check_menu_item_get_active(GtkCheckMenuItem *p) { return static_cast<QAction *>(p)->isChecked(); }
void gtk_menu_shell_append(void *p, GtkWidget *child) {
    auto *menu = static_cast<QMenu *>(p); auto *a = static_cast<QAction *>(child);
    if (a->actionGroup()) a->actionGroup()->setParent(menu); else a->setParent(menu);
    menu->addAction(a);
}
void gtk_menu_popup_at_pointer(void *p, GdkEvent *) { auto *menu = static_cast<QMenu *>(p); menu->exec(QCursor::pos()); menu->deleteLater(); }

}
