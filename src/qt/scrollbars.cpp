#include "scrollbars.h"
#include <QAbstractScrollArea>
#include <QApplication>
#include <QEvent>
#include <QHeaderView>
#include <QPainter>
#include <QProxyStyle>
#include <QScrollBar>
#include <QStyleOptionSlider>
#include <QTimer>
#include <QVariantAnimation>
#include <algorithm>

namespace {
// Keep Qt's real scrollbars, range and input behavior, while letting the view
// use the full available size.
class OverlayStyle : public QProxyStyle {
public:
    OverlayStyle() : QProxyStyle(QStringLiteral("Fusion")) {}
    int styleHint(StyleHint hint, const QStyleOption *option, const QWidget *widget,
                  QStyleHintReturn *data) const override {
        if (hint == SH_ScrollBar_Transient) return true;
        return QProxyStyle::styleHint(hint, option, widget, data);
    }
    int pixelMetric(PixelMetric metric, const QStyleOption *option, const QWidget *widget) const override {
        if (metric == PM_ScrollBarExtent || metric == PM_ScrollView_ScrollBarOverlap) return 12;
        return QProxyStyle::pixelMetric(metric, option, widget);
    }
    QRect subControlRect(ComplexControl control, const QStyleOptionComplex *option,
                         SubControl part, const QWidget *widget) const override {
        const auto *slider = qstyleoption_cast<const QStyleOptionSlider *>(option);
        if (control != CC_ScrollBar || !slider)
            return QProxyStyle::subControlRect(control, option, part, widget);
        const bool horizontal = slider->orientation == Qt::Horizontal;
        const QRect rect = slider->rect;
        const int length = horizontal ? rect.width() : rect.height();
        const qint64 range = qint64(slider->maximum) - slider->minimum;
        const int thumb = std::min(length, std::max(28,
            int(qint64(length) * slider->pageStep / std::max(qint64(1), range + slider->pageStep))));
        const int offset = sliderPositionFromValue(slider->minimum, slider->maximum,
            slider->sliderPosition, length - thumb, slider->upsideDown);
        QRect result;
        if (part == SC_ScrollBarGroove) result = rect;
        else if (horizontal) {
            if (part == SC_ScrollBarSlider) result = QRect(rect.x() + offset, rect.y(), thumb, rect.height());
            else if (part == SC_ScrollBarSubPage) result = QRect(rect.x(), rect.y(), offset, rect.height());
            else if (part == SC_ScrollBarAddPage) result = QRect(rect.x() + offset + thumb, rect.y(), length - offset - thumb, rect.height());
        } else {
            if (part == SC_ScrollBarSlider) result = QRect(rect.x(), rect.y() + offset, rect.width(), thumb);
            else if (part == SC_ScrollBarSubPage) result = QRect(rect.x(), rect.y(), rect.width(), offset);
            else if (part == SC_ScrollBarAddPage) result = QRect(rect.x(), rect.y() + offset + thumb, rect.width(), length - offset - thumb);
        }
        return visualRect(slider->direction, rect, result);
    }
};

class OverlayScrollBar : public QScrollBar {
public:
    explicit OverlayScrollBar(Qt::Orientation orientation) : QScrollBar(orientation) {
        static OverlayStyle *overlayStyle = [] {
            auto *style = new OverlayStyle;
            style->setParent(qApp);
            return style;
        }();
        setStyle(overlayStyle);
        setAttribute(Qt::WA_OpaquePaintEvent, false);
        setAutoFillBackground(false);
        setAttribute(Qt::WA_Hover);
        idle.setSingleShot(true); idle.setInterval(1200);
        fade.setDuration(200); fade.setStartValue(1.0); fade.setEndValue(0.0);
        connect(&fade, &QVariantAnimation::valueChanged, this, [this](const QVariant &value) {
            opacity = value.toReal(); update();
        });
        connect(&idle, &QTimer::timeout, this, [this] {
            if (!underMouse() && !isSliderDown()) fade.start();
        });
        connect(this, &QScrollBar::valueChanged, this, [this] {
            if (!property("evemonUpdatingModel").toBool()) reveal();
        });
        connect(this, &QScrollBar::sliderReleased, this, [this] { idle.start(); });
    }
protected:
    bool event(QEvent *event) override {
        switch (event->type()) {
        case QEvent::Enter: case QEvent::MouseMove:
        case QEvent::MouseButtonPress: case QEvent::Wheel: reveal(); break;
        case QEvent::Show: if (underMouse()) reveal(); break;
        case QEvent::Leave: idle.start(); break;
        default: break;
        }
        const bool handled = QScrollBar::event(event);
        if (event->type() == QEvent::Polish || event->type() == QEvent::StyleChange ||
            event->type() == QEvent::Show) {
            // Style polishing can restore QScrollBar's default opaque flag.
            // Only the fading thumb is painted; the viewport must show through.
            setAttribute(Qt::WA_OpaquePaintEvent, false);
            setAutoFillBackground(false);
        }
        return handled;
    }
    void paintEvent(QPaintEvent *) override {
        if (opacity <= 0 || maximum() <= minimum()) return;
        QStyleOptionSlider option; initStyleOption(&option);
        QRectF thumb = style()->subControlRect(QStyle::CC_ScrollBar, &option,
                                               QStyle::SC_ScrollBarSlider, this);
        const bool active = underMouse() || isSliderDown();
        const int inset = active ? 2 : 4;
        if (orientation() == Qt::Vertical) thumb.adjust(inset, 2, -inset, -2);
        else thumb.adjust(2, inset, -2, -inset);
        QColor color = palette().color(QPalette::Text);
        color.setAlphaF(opacity * (active ? 0.65 : 0.4));
        QPainter painter(this); painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(Qt::NoPen); painter.setBrush(color);
        const qreal radius = std::min(thumb.width(), thumb.height()) / 2;
        painter.drawRoundedRect(thumb, radius, radius);
    }
private:
    QTimer idle;
    QVariantAnimation fade;
    qreal opacity = 0;
    void reveal() {
        fade.stop(); opacity = 1; update(); idle.start();
    }
};

class OverlayPlacement : public QObject {
public:
    explicit OverlayPlacement(QAbstractScrollArea *view) : QObject(view), area(view) {
        // The default scrollbar containers are siblings of the viewport. On a
        // translucent window they leave an unpainted strip beneath a fading
        // bar. Painting as viewport children keeps the underlying rows intact.
        for (auto *bar : {area->horizontalScrollBar(), area->verticalScrollBar()}) {
            bar->setParent(area->viewport());
            connect(bar, &QScrollBar::rangeChanged, this, [this] { schedule(); });
        }
        area->installEventFilter(this); area->viewport()->installEventFilter(this);
        schedule();
    }
protected:
    bool eventFilter(QObject *, QEvent *event) override {
        if (event->type() == QEvent::Resize || event->type() == QEvent::Show ||
            event->type() == QEvent::LayoutRequest || event->type() == QEvent::StyleChange)
            schedule();
        return false;
    }
private:
    QAbstractScrollArea *area;
    bool pending = false;
    void schedule() {
        if (pending) return;
        pending = true;
        QTimer::singleShot(0, this, [this] {
            pending = false;
            auto *h = area->horizontalScrollBar(), *v = area->verticalScrollBar();
            const bool horizontal = area->horizontalScrollBarPolicy() != Qt::ScrollBarAlwaysOff && h->maximum() > h->minimum();
            const bool vertical = area->verticalScrollBarPolicy() != Qt::ScrollBarAlwaysOff && v->maximum() > v->minimum();
            const int width = area->viewport()->width(), height = area->viewport()->height();
            const bool rtl = area->layoutDirection() == Qt::RightToLeft;
            h->setGeometry(rtl && vertical ? 12 : 0, std::max(0, height - 12), std::max(0, width - (vertical ? 12 : 0)), 12);
            v->setGeometry(rtl ? 0 : std::max(0, width - 12), 0, 12, std::max(0, height - (horizontal ? 12 : 0)));
            h->setVisible(horizontal); v->setVisible(vertical);
            h->raise(); v->raise();
        });
    }
};
}

void installOverlayScrollBars(QWidget *widget)
{
    auto *area = qobject_cast<QAbstractScrollArea *>(widget);
    if (!area || qobject_cast<QHeaderView *>(area) || area->property("evemonOverlayScrolling").toBool()) return;
    area->setProperty("evemonOverlayScrolling", true);
    area->setHorizontalScrollBar(new OverlayScrollBar(Qt::Horizontal));
    area->setVerticalScrollBar(new OverlayScrollBar(Qt::Vertical));
    new OverlayPlacement(area);
}
