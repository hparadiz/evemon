#include "milkdrop_engine.h"
#include "milkdrop_keys.h"
#include <cairo.h>
#include <QOpenGLWidget>
#include <QOpenGLContext>
#include <QSurfaceFormat>
#include <QTimer>
#include <QElapsedTimer>
#include <QPainter>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QPointer>
#include <QVBoxLayout>
#include <QScreen>
#include <functional>

extern "C" {
#include "../core/log.h"
}

class MilkDropSurface : public QOpenGLWidget {
public:
    MilkDropSurface(QWidget *parent, void *engine) : QOpenGLWidget(parent), engine(engine) {
        QSurfaceFormat format; format.setVersion(3, 3); format.setProfile(QSurfaceFormat::CoreProfile);
        format.setDepthBufferSize(0); format.setStencilBufferSize(0); format.setSwapInterval(1); setFormat(format);
        setMinimumSize(0, 0); setFocusPolicy(Qt::StrongFocus);
        timer.setInterval(16); connect(&timer, &QTimer::timeout, this, qOverload<>(&QWidget::update)); elapsed.start();
    }
    ~MilkDropSurface() override { cleanup(); }
    std::function<void()> fullscreenChanged;
protected:
    void initializeGL() override {
        if (!engine) return;
        evemon_md_initialize(engine);
        connect(context(), &QOpenGLContext::aboutToBeDestroyed, this, [this] { cleanup(); }, Qt::DirectConnection);
    }
    void paintGL() override {
        if (!engine) return;
        const qreal scale = devicePixelRatioF();
        const QSize pixels(qRound(width() * scale), qRound(height() * scale));
        evemon_md_render(engine, defaultFramebufferObject(), pixels.width(), pixels.height(), elapsed.restart() / 1000.0f);
        if (overlay.size() != pixels) overlay = QImage(pixels, QImage::Format_ARGB32_Premultiplied);
        overlay.fill(Qt::transparent);
        auto *surface = cairo_image_surface_create_for_data(overlay.bits(), CAIRO_FORMAT_ARGB32, overlay.width(), overlay.height(), overlay.bytesPerLine());
        auto *cr = cairo_create(surface); cairo_scale(cr, scale, scale);
        evemon_md_overlay(engine, cr, width(), height());
        cairo_destroy(cr); cairo_surface_destroy(surface);
        QPainter painter(this); painter.drawImage(rect(), overlay);
    }
    void showEvent(QShowEvent *e) override { QOpenGLWidget::showEvent(e); elapsed.restart(); timer.start(); }
    void hideEvent(QHideEvent *e) override { timer.stop(); QOpenGLWidget::hideEvent(e); }
    void keyPressEvent(QKeyEvent *e) override {
        unsigned key = 0;
        const bool shift = e->modifiers().testFlag(Qt::ShiftModifier);
        if (e->key() >= Qt::Key_A && e->key() <= Qt::Key_Z) key = e->key() + (shift ? 0 : 32);
        else if (e->key() >= Qt::Key_F1 && e->key() <= Qt::Key_F35) key = GDK_KEY_F1 + e->key() - Qt::Key_F1;
        else switch (e->key()) {
        case Qt::Key_Escape: key = GDK_KEY_Escape; break;
        case Qt::Key_Backspace: key = GDK_KEY_BackSpace; break;
        case Qt::Key_Return: case Qt::Key_Enter: key = GDK_KEY_Return; break;
        case Qt::Key_Delete: key = GDK_KEY_Delete; break;
        case Qt::Key_Up: key = GDK_KEY_Up; break;
        case Qt::Key_Down: key = GDK_KEY_Down; break;
        default: if (e->key() >= 32 && e->key() < 127) key = e->key(); break;
        }
        const unsigned modifiers = (shift ? GDK_SHIFT_MASK : 0) | (e->modifiers().testFlag(Qt::ControlModifier) ? GDK_CONTROL_MASK : 0);
        const int wasFullscreen = evemon_md_fullscreen(engine);
        if (evemon_md_key(engine, key, modifiers)) e->accept(); else QOpenGLWidget::keyPressEvent(e);
        if (wasFullscreen != evemon_md_fullscreen(engine) && fullscreenChanged) fullscreenChanged();
        update();
    }
    void wheelEvent(QWheelEvent *e) override { if (e->angleDelta().y()) evemon_md_next(engine, e->angleDelta().y() > 0 ? 1 : -1); e->accept(); }
    void mousePressEvent(QMouseEvent *e) override { setFocus(); e->accept(); }
    void mouseDoubleClickEvent(QMouseEvent *e) override { if (e->button() == Qt::LeftButton) evemon_md_random(engine); e->accept(); }
private:
    void *engine;
    QTimer timer;
    QElapsedTimer elapsed;
    QImage overlay;
    void cleanup() { if (engine && context() && context()->isValid()) { makeCurrent(); evemon_md_release_gl(engine); doneCurrent(); } }
};

class MilkDropView : public QWidget {
public:
    MilkDropView(QWidget *parent, const evemon_host_services_t *services, const char *directory)
        : QWidget(parent), engine(evemon_md_create(directory, services)) {
        auto *box = new QVBoxLayout(this); box->setContentsMargins(0, 0, 0, 0);
        surface = new MilkDropSurface(this, engine); box->addWidget(surface);
        surface->fullscreenChanged = [this] { toggleFullscreen(); };
    }
    ~MilkDropView() override { delete surface; delete fullscreen; evemon_md_destroy(engine); }
    void updateData(const evemon_proc_data_t *data) { evemon_md_update(engine, data); }
    void clear() { evemon_md_clear(engine); }
protected:
    void hideEvent(QHideEvent *e) override { if (!fullscreen) clear(); QWidget::hideEvent(e); }
    bool eventFilter(QObject *object, QEvent *event) override {
        if (object == fullscreen && event->type() == QEvent::Close) {
            // Close the visualization window by returning it to its original tab.
            evemon_md_key(engine, GDK_KEY_f, 0); toggleFullscreen(); event->ignore(); return true;
        }
        return QWidget::eventFilter(object, event);
    }
private:
    void *engine;
    MilkDropSurface *surface;
    QPointer<QWidget> fullscreen;
    void toggleFullscreen() {
        if (evemon_md_fullscreen(engine)) {
            fullscreen = new QWidget(nullptr, Qt::Window); fullscreen->setWindowTitle("MilkDrop");
            fullscreen->installEventFilter(this);
            auto *box = new QVBoxLayout(fullscreen); box->setContentsMargins(0, 0, 0, 0); box->addWidget(surface);
            fullscreen->setGeometry(surface->screen()->geometry());
            fullscreen->showFullScreen();
        } else if (fullscreen) {
            layout()->addWidget(surface); surface->show(); fullscreen->removeEventFilter(this);
            fullscreen->deleteLater(); fullscreen = nullptr;
        }
        surface->setFocus();
    }
};
extern "C" Q_DECL_EXPORT QWidget *evemon_qt_milkdrop_create(QWidget *parent, const evemon_host_services_t *services, const char *directory)
{ return new MilkDropView(parent, services, directory); }
extern "C" Q_DECL_EXPORT void evemon_qt_milkdrop_update(QWidget *view, const evemon_proc_data_t *data)
{ static_cast<MilkDropView *>(view)->updateData(data); }
extern "C" Q_DECL_EXPORT void evemon_qt_milkdrop_clear(QWidget *view)
{ static_cast<MilkDropView *>(view)->clear(); }
