#pragma once
#include <QPlainTextEdit>
#include <QComboBox>
#include <QCheckBox>
#include <QHash>
#include <QLabel>
#include <functional>
#include <memory>
extern "C" {
#include "../plugins/evemon_plugin.h"
}
class WriteHighlighting;
class WriteText : public QPlainTextEdit {
public:
    explicit WriteText(QWidget *parent = nullptr);
    ~WriteText() override;
    void showGutter(bool visible);
    void appendEvent(const QString &text, int pid, const QString &name, bool scroll);
    void clear();
    void scrollToBottom();
    QStringList syntaxNames() const;
    void setSyntax(const QString &name);
    std::function<void()> scrolledUp;
protected:
    void resizeEvent(QResizeEvent *) override;
    void wheelEvent(QWheelEvent *) override;
    void keyPressEvent(QKeyEvent *) override;
    void changeEvent(QEvent *) override;
private:
    struct Entry { int pid; QString name, time; };
    QHash<int, Entry> entries;
    QWidget *gutter;
    std::unique_ptr<WriteHighlighting> highlighting;
    bool gutterVisible = false;
    void updateGutterGeometry();
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
    void setCaptureAvailable(bool available);
    std::function<void()> changed;
private:
    QComboBox *destination;
    QCheckBox *descendants, *gutter, *autoScroll, *excludeKernel;
    WriteText *text;
    QLabel *captureStatus;
};
