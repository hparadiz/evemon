#pragma once
#include <QObject>
#include <functional>
#include <QPalette>
#include <QSet>
#include <QWidget>

class QMenu;
class QPaintEvent;

class ThemeManager : public QObject {
public:
    struct Theme { QString id, name, path; };
    explicit ThemeManager(QObject *parent);
    QList<Theme> availableThemes() const;
    QString selected() const { return selectedTheme; }
    void apply(const QString &id);
    void populate(QMenu *menu, const std::function<void(const QString &)> &save);
    void prepareWindow(QWidget *window, bool translucent = true);
    void paintWindow(QWidget *window, QPaintEvent *event);
protected:
    bool eventFilter(QObject *object, QEvent *event) override;
private:
    QString selectedTheme = "System", systemStyle;
    QPalette systemPalette;
    QSet<QWidget *> windows;
    bool glass = false;
    QColor background;
    void updateBlur(QWidget *window);
};

ThemeManager *themeManager();
void setDimmedText(QWidget *widget);
void presentAuxiliaryWindow(QWidget *window, bool titleBar = true);

class ThemedWindow : public QWidget {
public:
    explicit ThemedWindow(QWidget *parent = nullptr, bool translucent = true);
protected:
    void paintEvent(QPaintEvent *event) override;
};
