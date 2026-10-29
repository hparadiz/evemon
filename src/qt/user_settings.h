#pragma once
#include <QSettings>
#include <QFileInfo>
#include <unistd.h>
extern "C" {
#include "../platform/desktop_session.h"
}

class UserSettings : public QSettings {
public:
    using QSettings::QSettings;
    void setValue(const QString &key, const QVariant &value) {
        QSettings::setValue(key, value);
        // QSettings atomically replaces its file. Repair ownership after each
        // elevated write, so a concurrent user-mode window can still save.
        if (geteuid() == 0 && desktop_session_uid() != 0) sync();
    }
    void sync() {
        QSettings::sync();
        desktop_settings_owner(fileName().toUtf8().constData());
        desktop_settings_owner(QFileInfo(fileName()).absolutePath().toUtf8().constData());
    }
};
