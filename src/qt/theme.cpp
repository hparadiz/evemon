#include "theme.h"
#include "scrollbars.h"
#include <QApplication>
#include <QActionGroup>
#include <QDir>
#include <QEvent>
#include <QFileInfo>
#include <QFrame>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QPaintEvent>
#include <QPainter>
#include <QPointer>
#include <QSettings>
#include <QStandardPaths>
#include <QStyle>
#include <QStyleFactory>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <algorithm>
#ifdef HAVE_KWINDOWSYSTEM
#include <KWindowEffects>
#endif
#ifdef HAVE_KCOLORSCHEME
#include <KColorScheme>
#include <KSharedConfig>
#endif

namespace {
// Gamescope presents native top-level windows as separate surfaces. Keep
// auxiliary views in the existing Evemon surface in that session.
class EmbeddedWindow : public QWidget {
public:
    EmbeddedWindow(QWidget *content, bool titleBar)
        : QWidget(content->parentWidget()->window()), content(content), preferred(content->size())
    {
        setObjectName("evemonEmbeddedWindow");
        setAttribute(Qt::WA_DeleteOnClose);
        previousFocus = QApplication::focusWidget();
        frame = new QFrame(this);
        frame->setAutoFillBackground(titleBar);
        frame->setFrameShape(titleBar ? QFrame::StyledPanel : QFrame::NoFrame);
        auto *layout = new QVBoxLayout(frame);
        layout->setContentsMargins(0, 0, 0, 0); layout->setSpacing(0);
        if (titleBar) {
            auto *header = new QHBoxLayout;
            header->setContentsMargins(12, 6, 6, 6);
            header->addWidget(new QLabel(content->windowTitle()), 1);
            auto *close = new QToolButton(frame);
            close->setText(QStringLiteral("×")); close->setToolTip("Close");
            header->addWidget(close); layout->addLayout(header);
            connect(close, &QToolButton::clicked, content, &QWidget::close);
            preferred.rheight() += 36;
        }
        content->setParent(frame, Qt::Widget);
        content->setWindowModality(Qt::NonModal);
        content->setProperty("evemonEmbedded", true);
        content->setProperty("evemonOpaqueWindow", true);
        layout->addWidget(content);
        content->installEventFilter(this); parentWidget()->installEventFilter(this);
        fit(); show(); raise(); content->show(); content->setFocus();
    }
protected:
    bool eventFilter(QObject *object, QEvent *event) override {
        if (object == parentWidget() && event->type() == QEvent::Resize) fit();
        if (object == content && event->type() == QEvent::Hide) {
            hide();
            if (previousFocus) previousFocus->setFocus();
            deleteLater();
        }
        return false;
    }
    void paintEvent(QPaintEvent *) override {
        QPainter painter(this); painter.fillRect(rect(), QColor(0, 0, 0, 100));
    }
private:
    QWidget *content;
    QFrame *frame;
    QSize preferred;
    QPointer<QWidget> previousFocus;
    void fit() {
        setGeometry(parentWidget()->rect());
        const QSize size = preferred.boundedTo((this->size() - QSize(32, 32)).expandedTo(QSize(1, 1)));
        frame->setGeometry(QRect((width() - size.width()) / 2, (height() - size.height()) / 2, size.width(), size.height()));
    }
};

#ifndef HAVE_KCOLORSCHEME
QColor schemeColor(QSettings &file, const QString &key, const QColor &fallback)
{
    const QStringList parts = file.value(key).toStringList();
    if (parts.size() < 3) return fallback;
    return QColor(parts[0].toInt(), parts[1].toInt(), parts[2].toInt(),
                  parts.size() > 3 ? parts[3].toInt() : 255);
}

#endif

QPalette loadPalette(const QString &path, const QPalette &fallback)
{
#ifdef HAVE_KCOLORSCHEME
    Q_UNUSED(fallback);
    return KColorScheme::createApplicationPalette(KSharedConfig::openConfig(path, KConfig::SimpleConfig));
#else
    QSettings file(path, QSettings::IniFormat);
    QPalette palette = fallback;
    const struct { QPalette::ColorRole role; const char *key; } colors[] = {
        {QPalette::Window, "Colors:Window/BackgroundNormal"},
        {QPalette::WindowText, "Colors:Window/ForegroundNormal"},
        {QPalette::Base, "Colors:View/BackgroundNormal"},
        {QPalette::AlternateBase, "Colors:View/BackgroundAlternate"},
        {QPalette::Text, "Colors:View/ForegroundNormal"},
        {QPalette::Button, "Colors:Button/BackgroundNormal"},
        {QPalette::ButtonText, "Colors:Button/ForegroundNormal"},
        {QPalette::Highlight, "Colors:Selection/BackgroundNormal"},
        {QPalette::HighlightedText, "Colors:Selection/ForegroundNormal"},
        {QPalette::Link, "Colors:View/ForegroundLink"},
        {QPalette::LinkVisited, "Colors:View/ForegroundVisited"},
        {QPalette::ToolTipBase, "Colors:Tooltip/BackgroundNormal"},
        {QPalette::ToolTipText, "Colors:Tooltip/ForegroundNormal"}
    };
    for (const auto &color : colors)
        palette.setColor(color.role, schemeColor(file, color.key, palette.color(color.role)));
    for (auto role : {QPalette::Text, QPalette::WindowText, QPalette::ButtonText})
        palette.setColor(QPalette::Disabled, role,
                         schemeColor(file, "Colors:View/ForegroundInactive", QColor(128, 128, 128)));
    return palette;
#endif
}
}

void presentAuxiliaryWindow(QWidget *window, bool titleBar)
{
    if (window->property("evemonEmbedded").toBool()) {
        window->show(); window->parentWidget()->parentWidget()->raise(); return;
    }
    const auto desktops = qEnvironmentVariable("XDG_CURRENT_DESKTOP").split(':');
    if (window->parentWidget() && desktops.contains("gamescope", Qt::CaseInsensitive)) {
        new EmbeddedWindow(window, titleBar);
    } else {
        window->show(); window->raise(); window->activateWindow();
    }
}

ThemeManager::ThemeManager(QObject *parent) : QObject(parent),
    systemStyle(qApp->style()->objectName()), systemPalette(qApp->palette())
{
    qApp->installEventFilter(this);
}

ThemeManager *themeManager()
{
    static QPointer<ThemeManager> manager;
    if (!manager) manager = new ThemeManager(qApp);
    return manager;
}

void setDimmedText(QWidget *widget)
{
    widget->setProperty("evemonDimLabel", true);
    QPalette palette;
    QColor color = qApp->palette().color(QPalette::WindowText); color.setAlpha(150);
    palette.setColor(QPalette::WindowText, color); widget->setPalette(palette);
}

QList<ThemeManager::Theme> ThemeManager::availableThemes() const
{
    QList<Theme> result;
    QSet<QString> seen;
    // User schemes take precedence over system schemes with the same filename.
    for (const QString &base : QStandardPaths::standardLocations(QStandardPaths::GenericDataLocation)) {
        QDir directory(base + "/color-schemes");
        for (const QString &file : directory.entryList({"*.colors"}, QDir::Files)) {
            const QString id = "scheme:" + QFileInfo(file).completeBaseName();
            if (seen.contains(id)) continue;
            seen.insert(id);
            QSettings scheme(directory.filePath(file), QSettings::IniFormat);
            // QSettings maps the INI [General] group to its root keys.
            result.append({id, scheme.value("Name", QFileInfo(file).completeBaseName()).toString(), directory.filePath(file)});
        }
    }
    for (const QString &style : QStyleFactory::keys()) {
        const bool duplicate = std::any_of(result.cbegin(), result.cend(), [&style](const Theme &theme) {
            return theme.name.compare(style, Qt::CaseInsensitive) == 0;
        });
        result.append({"style:" + style, style + (duplicate ? " (style)" : ""), {}});
    }
    std::sort(result.begin(), result.end(), [](const Theme &a, const Theme &b) {
        return QString::localeAwareCompare(a.name.toCaseFolded(), b.name.toCaseFolded()) < 0;
    });
    result.prepend({"System", "System", {}});
    return result;
}

void ThemeManager::apply(const QString &requested)
{
    QString id = requested;
    // Preserve preferences from the original port and import matching GTK themes.
    if (id == "Dark") id = "scheme:BreezeDark";
    if (id == "Light") id = "scheme:BreezeLight";
    Theme chosen{"System", "System", {}};
    for (const auto &theme : availableThemes())
        if (theme.id == id || theme.name == id || theme.id == "scheme:" + id) { chosen = theme; break; }
    selectedTheme = chosen.id;
    qApp->setStyleSheet({});
    const QString style = chosen.id.startsWith("style:") ? chosen.id.mid(6) : systemStyle;
    if (qApp->style()->objectName().compare(style, Qt::CaseInsensitive) != 0)
        if (QStyle *replacement = QStyleFactory::create(style)) qApp->setStyle(replacement);
    QPalette palette = chosen.path.isEmpty()
        ? (chosen.id == "System" ? systemPalette : qApp->style()->standardPalette())
        : loadPalette(chosen.path, systemPalette);
    glass = palette.color(QPalette::Window).alpha() < 255 || palette.color(QPalette::Base).alpha() < 255;
    // Glass schemes describe the client tint. Sending their transparent WM
    // colors to KWin also makes dialog titlebars unreadable over the parent.
    // GTK leaves decorations on the desktop scheme; do the same for glass.
    qApp->setProperty("KDE_COLOR_SCHEME_PATH", glass ? QString() : chosen.path);
    background = palette.color(QPalette::Window);
    if (glass) {
        // NaviGlass uses clear child surfaces over one 60% window tint.
        if (background.alpha() == 0) background = QColor(24, 24, 24, 153);
        palette.setColor(QPalette::Window, Qt::transparent);
        palette.setColor(QPalette::Base, Qt::transparent);
        palette.setColor(QPalette::AlternateBase, QColor(255, 255, 255, 6));
    }
    if (chosen.id == QLatin1String("scheme:NaviGlass")) {
        // The GTK companion theme inherits WhiteSur's accent and text colors;
        // its KDE color file otherwise retains Breeze's different accent.
        palette.setColor(QPalette::Highlight, QColor("#0860f2"));
        palette.setColor(QPalette::Text, QColor("#e6e6e6"));
        palette.setColor(QPalette::WindowText, QColor("#e6e6e6"));
    }
    qApp->setPalette(palette);
    const QString frameStyle = QStringLiteral(
        "QFrame[evemonFrame=\"true\"] { border: 1px solid palette(mid); background: transparent; }"
        "QToolButton[evemonPanelClose=\"true\"] { border: none; padding: 0; min-width: 20px; max-width: 20px; min-height: 20px; max-height: 20px; background: transparent; border-radius: 4px; }"
        "QToolButton[evemonPanelClose=\"true\"]:hover { background: palette(mid); }");
    const QString menuBarStyle = QStringLiteral(
        "QMenuBar::item:selected { background: %1; color: %2; border-radius: 5px; }")
        .arg(palette.color(QPalette::Highlight).name(), palette.color(QPalette::HighlightedText).name());
    qApp->setStyleSheet(frameStyle + (glass ? menuBarStyle + QStringLiteral(
        "QMainWindow,QDockWidget,QTabWidget::pane,QScrollArea,QTreeView,QTableView,QListView,"
        "QTextEdit,QPlainTextEdit,QMenuBar,QStatusBar,QSplitter,QFrame { background: transparent; }"
        "QMessageBox,QInputDialog,QFileDialog,QFontDialog,QColorDialog { background: #303030; }"
        "QTabWidget::pane { border: 1px solid rgba(180,180,180,35); }"
        "QHeaderView::section { background: rgba(36,36,36,64); color: palette(text); }"
        "QTabBar::tab { background: transparent; color: palette(window-text); padding: 6px 10px; }"
        "QTabBar::tab:selected { background: rgba(92,92,92,110); }"
        "QMenu { background: rgba(48,48,48,230); color: palette(window-text); border: 1px solid rgba(180,180,180,55); border-radius: 10px; padding: 6px; }"
        "QMenu::item { padding: 3px 12px 3px 24px; border-radius: 5px; }"
        "QMenu::separator { height: 1px; background: rgba(180,180,180,45); margin: 4px; }"
        "QMenu::item:selected { background: palette(highlight); color: palette(highlighted-text); }"
        "QLineEdit,QComboBox,QPushButton { background: rgba(48,48,48,140); }"
        "QLineEdit { border: 1px solid rgba(180,180,180,35); border-radius: 5px; padding: 3px 6px; }"
        "QComboBox { border: 1px solid rgba(180,180,180,35); border-radius: 4px; padding: 3px 6px; min-height: 0; }"
        "QProgressBar { border: none; border-radius: 3px; background: rgba(180,180,180,30); }"
        "QProgressBar::chunk { border-radius: 3px; background: palette(highlight); }"
        "QComboBox QAbstractItemView { background: rgba(48,48,48,230); color: palette(text); border: 1px solid rgba(180,180,180,55); padding: 4px; }"
        "QTreeView::item:selected,QTableView::item:selected { background: rgba(92,92,92,166); }"
        "QFrame[evemonFrame=\"true\"] { border: 1px solid rgba(180,180,180,35); }"
        "QStatusBar::item { border: none; }") : QString()));
    // Removing a stylesheet can restore a widget's old inherited palette after
    // QApplication has delivered its palette change. Re-resolve those widgets
    // against their current parents; retain deliberately overridden palettes.
    const auto inheritPalette = [](QWidget *widget, const auto &self) -> void {
        // Breeze installs an explicit palette on menu bars during polish.
        if (!widget->palette().resolveMask() || qobject_cast<QMenuBar *>(widget))
            widget->setPalette(QPalette());
        if (widget->property("evemonDimLabel").toBool()) setDimmedText(widget);
        const auto children = widget->findChildren<QWidget *>(QString(), Qt::FindDirectChildrenOnly);
        for (QWidget *child : children) self(child, self);
    };
    for (QWidget *window : QApplication::topLevelWidgets()) inheritPalette(window, inheritPalette);
    for (QWidget *window : std::as_const(windows)) { updateBlur(window); window->update(); }
}

void ThemeManager::populate(QMenu *menu, const std::function<void(const QString &)> &save)
{
    menu->clear();
    auto *group = new QActionGroup(menu);
    // The group dies with its actions on the next rebuild.
    const auto themes = availableThemes();
    for (const auto &theme : themes) {
        auto *action = menu->addAction(theme.name);
        action->setCheckable(true); action->setChecked(theme.id == selectedTheme); group->addAction(action);
        connect(action, &QAction::triggered, menu, [this, id = theme.id, save] { apply(id); save(selectedTheme); });
    }
    connect(menu->actions().front(), &QObject::destroyed, group, &QObject::deleteLater);
}

void ThemeManager::prepareWindow(QWidget *window, bool translucent)
{
    window->setAttribute(Qt::WA_TranslucentBackground, translucent);
    window->setProperty("evemonOpaqueWindow", !translucent);
    windows.insert(window);
    if (!translucent) updateBlur(window);
    connect(window, &QObject::destroyed, this, [this, window] { windows.remove(window); });
}

void ThemeManager::paintWindow(QWidget *window, QPaintEvent *event)
{
    QPainter painter(window);
    painter.setCompositionMode(QPainter::CompositionMode_Source);
    QColor color = background;
    if (window->property("evemonOpaqueWindow").toBool()) color.setAlpha(255);
    else if (!window->isActiveWindow() && glass) color = QColor(36, 36, 36, background.alpha());
    painter.fillRect(event->rect(), color);
}

void ThemeManager::updateBlur(QWidget *window)
{
    const bool opaque = window->property("evemonOpaqueWindow").toBool();
    if (opaque) {
        QColor color = background; color.setAlpha(255);
        QPalette palette; palette.setColor(QPalette::Window, color);
        window->setPalette(palette);
    }
#ifdef HAVE_KWINDOWSYSTEM
    if (window->windowHandle()) KWindowEffects::enableBlurBehind(window->windowHandle(), glass && !opaque);
#else
    Q_UNUSED(window);
#endif
}

bool ThemeManager::eventFilter(QObject *object, QEvent *event)
{
    if (event->type() == QEvent::Polish)
        if (auto *widget = qobject_cast<QWidget *>(object)) installOverlayScrollBars(widget);
    if (auto *widget = qobject_cast<QWidget *>(object);
        widget && (event->type() == QEvent::Polish || event->type() == QEvent::Show) &&
        (qobject_cast<QMenu *>(widget) || widget->windowType() == Qt::Popup) &&
        !windows.contains(widget))
        prepareWindow(widget);
    if (auto *window = qobject_cast<QWidget *>(object); window && windows.contains(window)) {
        if (event->type() == QEvent::Show || event->type() == QEvent::WinIdChange)
            QTimer::singleShot(0, window, [this, window] { updateBlur(window); });
        if (event->type() == QEvent::WindowActivate || event->type() == QEvent::WindowDeactivate) window->update();
    }
    return false;
}

ThemedWindow::ThemedWindow(QWidget *parent, bool translucent) : QWidget(parent, Qt::Window) { themeManager()->prepareWindow(this, translucent); }
void ThemedWindow::paintEvent(QPaintEvent *event) { themeManager()->paintWindow(this, event); }
