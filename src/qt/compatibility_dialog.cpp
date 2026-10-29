extern "C" {
#include "../ipc/query.h"
}
#include "compatibility_dialog.h"
#include "../platform/compatibility.h"
#include "theme.h"
#include <QFrame>
#include <QGridLayout>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QShortcut>
#include <QVBoxLayout>

void showCompatibilityDialog(QWidget *parent)
{
    auto *window = new ThemedWindow(parent, false);
    window->setObjectName("evemonCompatibility");
    window->setAttribute(Qt::WA_DeleteOnClose);
    window->setWindowTitle(query_is_foreign_host() ? QStringLiteral("evemon – Local GUI Compatibility") : QStringLiteral("evemon – System Compatibility"));
    window->setWindowModality(Qt::WindowModal);
    window->resize(720, 500);
    auto *escape = new QShortcut(QKeySequence(Qt::Key_Escape), window);
    QObject::connect(escape, &QShortcut::activated, window, &QWidget::close);
    auto *layout = new QVBoxLayout(window);
    layout->setContentsMargins(0, 0, 0, 0); layout->setSpacing(0);
    auto separator = [layout] {
        auto *line = new QFrame; line->setFrameShape(QFrame::HLine); layout->addWidget(line);
    };
    auto *header = new QHBoxLayout;
    header->setContentsMargins(16, 12, 16, 8);
    auto *title = new QLabel(query_is_foreign_host() ? "Local GUI Compatibility" : "System Compatibility");
    QFont heading = title->font(); heading.setPointSize(13); heading.setBold(true); title->setFont(heading);
    header->addWidget(title, 1);
    auto *close = new QPushButton("Close"); header->addWidget(close);
    QObject::connect(close, &QPushButton::clicked, window, &QWidget::close);
    layout->addLayout(header); separator();
    auto *scroll = new QScrollArea; scroll->setWidgetResizable(true); scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff); layout->addWidget(scroll, 1);
    auto *content = new QWidget;
    auto *grid = new QGridLayout(content);
    grid->setContentsMargins(16, 10, 16, 16); grid->setHorizontalSpacing(8); grid->setVerticalSpacing(2);
    grid->setColumnStretch(2, 1);
    const QStringList names{"Feature", "Status", "Details"};
    for (int c = 0; c < names.size(); ++c) {
        auto *label = new QLabel(names[c]); label->setFont(heading); grid->addWidget(label, 0, c);
    }
    auto *line = new QFrame; line->setFrameShape(QFrame::HLine); grid->addWidget(line, 1, 0, 1, 3);
    compat_result_t results[32];
    const int count = evemon_collect_compat_results(results, 32);
    const char *colors[] = {"#2ecc71", "#e67e22", "#e74c3c", "#95a5a6"};
    for (int i = 0; i < count; ++i) {
        const auto &result = results[i]; const QString color = colors[result.severity];
        if (!(i % 2)) {
            auto *background = new QWidget;
            background->setAttribute(Qt::WA_StyledBackground);
            background->setStyleSheet("background: rgba(128,128,128,10);");
            grid->addWidget(background, i + 2, 0, 1, 3);
        }
        auto *feature = new QLabel(QString("<span style='color:%1'>●</span> %2")
            .arg(color, QString::fromUtf8(result.feature).toHtmlEscaped()));
        auto *status = new QLabel(QString::fromUtf8(result.status));
        status->setStyleSheet(QString("color:%1; font-weight:bold;").arg(color));
        auto *detail = new QLabel(QString::fromUtf8(result.detail));
        detail->setWordWrap(true); detail->setMinimumWidth(120);
        detail->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        QFont font = detail->font(); font.setPointSize(9); detail->setFont(font);
        int column = 0;
        for (QLabel *label : {feature, status, detail}) {
            label->setContentsMargins(0, 4, 0, 4); label->setAlignment(Qt::AlignTop | Qt::AlignLeft);
            label->setTextInteractionFlags(Qt::TextSelectableByMouse);
            grid->addWidget(label, i + 2, column++);
        }
    }
    grid->setRowStretch(count + 2, 1); scroll->setWidget(content); separator();
    auto *footer = new QLabel(query_is_foreign_host() ?
        "These checks describe this GUI machine. Monitoring permissions belong to the remote evemon." :
        "Use File → Restart as Administrator to gain full access.  "
                             "Alternatively, grant individual capabilities with setcap(8).");
    footer->setWordWrap(true); footer->setContentsMargins(16, 8, 16, 10); layout->addWidget(footer);
    window->move(parent->frameGeometry().center() - window->rect().center()); presentAuxiliaryWindow(window);
}
