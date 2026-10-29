#pragma once
#include "process_model.h"
#include <QScrollArea>
#include <QGridLayout>
#include <QLabel>
#include <QMap>
class DetailValue : public QLabel {
public:
    explicit DetailValue(QWidget *parent = nullptr);
    void setValue(const QString &value);
protected:
    void resizeEvent(QResizeEvent *event) override;
private:
    QString full;
    void elide();
};
class DetailPanel : public QWidget {
public:
    explicit DetailPanel(QWidget *parent = nullptr);
    void setProcess(const ProcessRow *row, quint64 send, quint64 receive, const QIcon &icon);
    void setExpanded(bool open);
    bool isExpanded() const;
    void setSoftware(const QMap<QString, QString> &values);
private:
    QScrollArea *body;
    class QPushButton *toggle;
    class VerticalSummary *summary;
    class ProcessIcon *processIcon;
    QMap<QString, DetailValue *> processValues, steamValues, softwareValues;
    QWidget *steamSection, *softwareSection;
    QWidget *makeSection(const QString &title, const QStringList &keys, QMap<QString, DetailValue *> &labels);
};
