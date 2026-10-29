#pragma once
#include <QPlainTextEdit>
#include <QComboBox>
#include <QCheckBox>
#include <functional>
extern "C" {
#include "../plugins/evemon_plugin.h"
}
class WriteText : public QPlainTextEdit {
public:
    explicit WriteText(QWidget *parent = nullptr);
    void showGutter(bool visible);
    void appendEvent(const QString &text, int pid, const QString &name, bool scroll);
    std::function<void()> scrolledUp;
protected:
    void resizeEvent(QResizeEvent *) override;
    void wheelEvent(QWheelEvent *) override;
private:
    QWidget *gutter;
    bool gutterVisible = false;
    void paintGutter(QPaintEvent *);
    friend class WriteGutter;
};
class WriteView : public QWidget {
public:
    explicit WriteView(QWidget *parent = nullptr);
    int mask() const { return destination->currentIndex(); }
    bool includesDescendants() const { return descendants->isChecked(); }
    void append(const evemon_fd_write_payload_t &write);
    void clear();
    std::function<void()> changed;
private:
    QComboBox *destination;
    QCheckBox *descendants, *gutter, *autoScroll, *excludeKernel;
    WriteText *text;
};
