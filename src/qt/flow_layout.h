#pragma once
#include <QLayout>
#include <algorithm>

// Toolbar controls keep their normal size and wrap as the inspector narrows.
class FlowLayout : public QLayout {
public:
    explicit FlowLayout(int spacing = 6) { setContentsMargins(0, 0, 0, 0); setSpacing(spacing); }
    ~FlowLayout() override { while (auto *item = takeAt(0)) delete item; }
    void addItem(QLayoutItem *item) override { items.append(item); }
    int count() const override { return items.size(); }
    QLayoutItem *itemAt(int i) const override { return items.value(i); }
    QLayoutItem *takeAt(int i) override { return i >= 0 && i < items.size() ? items.takeAt(i) : nullptr; }
    bool hasHeightForWidth() const override { return true; }
    int heightForWidth(int width) const override { return arrange(QRect(0, 0, width, 0), false); }
    QSize minimumSize() const override {
        QSize size;
        for (auto *item : items) size = size.expandedTo(item->minimumSize());
        const auto margins = contentsMargins();
        return size + QSize(margins.left() + margins.right(), margins.top() + margins.bottom());
    }
    QSize sizeHint() const override { return minimumSize(); }
    void setGeometry(const QRect &rect) override { QLayout::setGeometry(rect); arrange(rect, true); }
private:
    QList<QLayoutItem *> items;
    int arrange(const QRect &rect, bool apply) const {
        const auto margins = contentsMargins();
        const QRect area = rect.marginsRemoved(margins);
        int x = area.left(), y = area.top(), rowHeight = 0;
        for (auto *item : items) {
            if (item->isEmpty()) continue;
            const QSize size = item->sizeHint();
            if (x > area.left() && x + size.width() > area.right() + 1) {
                x = area.left(); y += rowHeight + spacing(); rowHeight = 0;
            }
            if (apply) item->setGeometry(QRect(QPoint(x, y), size));
            x += size.width() + spacing(); rowHeight = std::max(rowHeight, size.height());
        }
        return y + rowHeight - rect.top() + margins.bottom();
    }
};
