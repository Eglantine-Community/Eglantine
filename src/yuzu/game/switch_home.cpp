// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cmath>
#include <QDateTime>
#include <QDesktopServices>
#include <QPainter>
#include <QPainterPath>
#include <QUrl>

#include "common/fs/fs_paths.h"
#include "common/fs/path_util.h"
#include "qt_common/config/uisettings.h"
#include "yuzu/dalia/dalia_diagnostics.h"
#include "yuzu/dalia/dalia_settings.h"
#include "yuzu/debugger/controller.h"
#include "yuzu/game/game_list.h"
#include "yuzu/game/switch_home.h"
#include "yuzu/main_window.h"

SwitchRoundButton::SwitchRoundButton(SwitchButtonType type, QWidget* parent)
    : QPushButton{parent}, m_type{type} {
    setFixedSize(54, 54);
    setCursor(Qt::PointingHandCursor);
    setFocusPolicy(Qt::StrongFocus);

    switch (type) {
    case SwitchButtonType::News:
        setToolTip(tr("News / Updates"));
        break;
    case SwitchButtonType::Shop:
        setToolTip(tr("Nintendo eShop / Add Game Directory"));
        break;
    case SwitchButtonType::Album:
        setToolTip(tr("Album / Screenshots"));
        break;
    case SwitchButtonType::Controllers:
        setToolTip(tr("Controllers"));
        break;
    case SwitchButtonType::Settings:
        setToolTip(tr("System Settings"));
        break;
    case SwitchButtonType::Diagnostics:
        setToolTip(tr("Diagnostics & Hardware Telemetry"));
        break;
    case SwitchButtonType::Power:
        setToolTip(tr("Sleep Mode / Exit"));
        break;
    }
}

void SwitchRoundButton::paintEvent(QPaintEvent* event) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);

    const bool isDark = UISettings::IsDarkTheme();
    const bool isHovered = underMouse() || hasFocus();

    const QRectF r = rect();
    const qreal sz = std::min(r.width(), r.height()) - 6.0;
    const QRectF circleRect((r.width() - sz) / 2.0, (r.height() - sz) / 2.0, sz, sz);

    // 1. Draw outer focus glow if hovered
    if (isHovered) {
        painter.setPen(QPen(QColor(0, 195, 227, 90), 5.5));
        painter.setBrush(Qt::NoBrush);
        painter.drawEllipse(circleRect.adjusted(-2.0, -2.0, 2.0, 2.0));
    }

    // 2. Circular button body
    QColor bgColor = isDark ? QColor(48, 51, 56) : QColor(255, 255, 255);
    QColor borderColor = isHovered ? (isDark ? QColor(0, 210, 255) : QColor(0, 180, 220))
                                   : (isDark ? QColor(70, 75, 82) : QColor(215, 215, 215));

    painter.setPen(QPen(borderColor, isHovered ? 3.0 : 1.5));
    painter.setBrush(bgColor);
    painter.drawEllipse(circleRect);

    // 3. Draw Icon inside
    const qreal cx = circleRect.center().x();
    const qreal cy = circleRect.center().y();

    switch (m_type) {
    case SwitchButtonType::News: {
        const QColor redCol(230, 0, 18);
        painter.setPen(Qt::NoPen);
        painter.setBrush(redCol);
        QRectF bubble(cx - 11.0, cy - 9.5, 22.0, 15.0);
        painter.drawRoundedRect(bubble, 3.5, 3.5);

        QPolygonF tail;
        tail << QPointF(cx - 6.0, cy + 5.0) << QPointF(cx - 11.0, cy + 9.5) << QPointF(cx - 2.0, cy + 5.0);
        painter.drawPolygon(tail);

        painter.setPen(QPen(Qt::white, 2.0, Qt::SolidLine, Qt::RoundCap));
        painter.drawLine(QPointF(cx - 6.0, cy - 4.5), QPointF(cx + 6.0, cy - 4.5));
        painter.drawLine(QPointF(cx - 6.0, cy - 1.0), QPointF(cx + 6.0, cy - 1.0));
        painter.drawLine(QPointF(cx - 6.0, cy + 2.5), QPointF(cx + 2.0, cy + 2.5));
        break;
    }
    case SwitchButtonType::Shop: {
        const QColor orangeCol(255, 149, 0);
        painter.setPen(QPen(orangeCol, 2.2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        painter.setBrush(Qt::NoBrush);
        painter.drawArc(QRectF(cx - 5.5, cy - 11.0, 11.0, 9.0), 0, 180 * 16);

        painter.setBrush(orangeCol);
        QPolygonF bag;
        bag << QPointF(cx - 8.5, cy - 4.0) << QPointF(cx + 8.5, cy - 4.0)
            << QPointF(cx + 10.0, cy + 9.0) << QPointF(cx - 10.0, cy + 9.0);
        painter.drawPolygon(bag);

        painter.setPen(QPen(Qt::white, 1.8));
        painter.drawLine(QPointF(cx - 3.0, cy + 2.0), QPointF(cx + 3.0, cy + 2.0));
        break;
    }
    case SwitchButtonType::Album: {
        const QColor blueCol(0, 122, 255);
        painter.setPen(QPen(blueCol, 2.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        painter.setBrush(Qt::NoBrush);
        QRectF frame(cx - 9.5, cy - 7.5, 19.0, 15.0);
        painter.drawRoundedRect(frame, 2.5, 2.5);

        QPainterPath mountains;
        mountains.moveTo(cx - 8.0, cy + 5.5);
        mountains.lineTo(cx - 3.0, cy - 1.5);
        mountains.lineTo(cx + 1.0, cy + 2.5);
        mountains.lineTo(cx + 5.0, cy - 3.5);
        mountains.lineTo(cx + 8.0, cy + 5.5);
        painter.drawPath(mountains);

        painter.setBrush(blueCol);
        painter.drawEllipse(QRectF(cx + 2.5, cy - 6.0, 3.0, 3.0));
        break;
    }
    case SwitchButtonType::Controllers: {
        const QColor grayCol = isDark ? QColor(200, 205, 210) : QColor(85, 85, 85);
        painter.setPen(QPen(grayCol, 1.8, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        painter.setBrush(Qt::NoBrush);

        painter.drawRoundedRect(QRectF(cx - 9.5, cy - 9.5, 8.0, 19.0), 3.5, 3.5);
        painter.drawEllipse(QRectF(cx - 7.0, cy - 5.5, 3.0, 3.0));

        painter.drawRoundedRect(QRectF(cx + 1.5, cy - 9.5, 8.0, 19.0), 3.5, 3.5);
        painter.drawEllipse(QRectF(cx + 4.0, cy + 1.0, 3.0, 3.0));
        break;
    }
    case SwitchButtonType::Settings: {
        const QColor grayCol = isDark ? QColor(200, 205, 210) : QColor(85, 85, 85);
        painter.setPen(QPen(grayCol, 2.0));
        painter.setBrush(Qt::NoBrush);
        painter.drawEllipse(QRectF(cx - 5.5, cy - 5.5, 11.0, 11.0));
        painter.drawEllipse(QRectF(cx - 2.5, cy - 2.5, 5.0, 5.0));

        for (int i = 0; i < 8; ++i) {
            const qreal angle = i * (3.14159265 / 4.0);
            const qreal r1 = 5.5;
            const qreal r2 = 9.0;
            painter.drawLine(QPointF(cx + r1 * std::cos(angle), cy + r1 * std::sin(angle)),
                             QPointF(cx + r2 * std::cos(angle), cy + r2 * std::sin(angle)));
        }
        break;
    }
    case SwitchButtonType::Diagnostics: {
        const QColor cyanCol(0, 195, 227);
        painter.setPen(QPen(cyanCol, 2.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        painter.setBrush(Qt::NoBrush);
        QPainterPath path;
        path.moveTo(cx - 10.0, cy);
        path.lineTo(cx - 5.0, cy);
        path.lineTo(cx - 2.0, cy - 7.0);
        path.lineTo(cx + 2.0, cy + 7.0);
        path.lineTo(cx + 5.0, cy);
        path.lineTo(cx + 10.0, cy);
        painter.drawPath(path);
        break;
    }
    case SwitchButtonType::Power: {
        const QColor grayCol = isDark ? QColor(200, 205, 210) : QColor(85, 85, 85);
        painter.setPen(QPen(grayCol, 2.2, Qt::SolidLine, Qt::RoundCap));
        painter.setBrush(Qt::NoBrush);
        painter.drawArc(QRectF(cx - 7.5, cy - 5.5, 15.0, 15.0), 45 * 16, 270 * 16);
        painter.drawLine(QPointF(cx, cy - 8.5), QPointF(cx, cy - 1.0));
        break;
    }
    }
}

SwitchTopBar::SwitchTopBar(QWidget* parent) : QWidget{parent} {
    setFixedHeight(50);

    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(28, 6, 28, 6);
    layout->setSpacing(14);

    layout->addStretch();

    m_time_label = new QLabel(this);
    QFont font = m_time_label->font();
    font.setBold(true);
    font.setPointSize(13);
    m_time_label->setFont(font);

    layout->addWidget(m_time_label);

    m_clock_timer = new QTimer(this);
    connect(m_clock_timer, &QTimer::timeout, this, &SwitchTopBar::UpdateClock);
    m_clock_timer->start(1000);
    UpdateClock();
}

void SwitchTopBar::UpdateClock() {
    m_time_label->setText(QDateTime::currentDateTime().toString(QStringLiteral("h:mm AP")));
    const bool isDark = UISettings::IsDarkTheme();
    m_time_label->setStyleSheet(isDark ? QStringLiteral("color: #FFFFFF;")
                                       : QStringLiteral("color: #2D2D2D;"));
    update();
}

void SwitchTopBar::paintEvent(QPaintEvent* event) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);

    const bool isDark = UISettings::IsDarkTheme();

    // 1. Draw 3 Circular User Avatars on the left
    const qreal avY = 7.0;
    const qreal avSz = 36.0;

    // Avatar 1: Dalia Flower Logo
    painter.setPen(QPen(isDark ? QColor(70, 75, 80) : QColor(220, 220, 220), 1.5));
    static const QPixmap dalia_pixmap = QPixmap(QStringLiteral(":/icons/default/256x256/eden.png"));
    if (!dalia_pixmap.isNull()) {
        QPainterPath clipPath;
        clipPath.addEllipse(QRectF(28.0, avY, avSz, avSz));
        painter.save();
        painter.setClipPath(clipPath);
        painter.drawPixmap(QRectF(28.0, avY, avSz, avSz), dalia_pixmap, dalia_pixmap.rect());
        painter.restore();
        painter.setBrush(Qt::NoBrush);
        painter.drawEllipse(QRectF(28.0, avY, avSz, avSz));
    } else {
        painter.setBrush(QColor(0, 166, 81));
        painter.drawEllipse(QRectF(28.0, avY, avSz, avSz));
        painter.setPen(Qt::white);
        QFont f = painter.font();
        f.setBold(true);
        f.setPointSize(11);
        painter.setFont(f);
        painter.drawText(QRectF(28.0, avY, avSz, avSz), Qt::AlignCenter, QStringLiteral("D"));
    }

    // Avatar 2: Red (Mario)
    painter.setBrush(QColor(230, 0, 18));
    painter.drawEllipse(QRectF(72.0, avY, avSz, avSz));
    painter.drawText(QRectF(72.0, avY, avSz, avSz), Qt::AlignCenter, QStringLiteral("M"));

    // Avatar 3: Purple (Joy-Con)
    painter.setBrush(QColor(102, 45, 145));
    painter.drawEllipse(QRectF(116.0, avY, avSz, avSz));
    painter.drawText(QRectF(116.0, avY, avSz, avSz), Qt::AlignCenter, QStringLiteral("★"));

    // 2. Draw Wi-Fi icon next to clock on the right
    const qreal rx = width() - 58.0;
    const qreal ry = height() / 2.0;
    const QColor iconCol = isDark ? QColor(240, 240, 240) : QColor(45, 45, 45);

    painter.setPen(QPen(iconCol, 1.8, Qt::SolidLine, Qt::RoundCap));
    painter.setBrush(Qt::NoBrush);
    painter.drawArc(QRectF(rx - 7.0, ry - 7.0, 14.0, 14.0), 45 * 16, 90 * 16);
    painter.drawArc(QRectF(rx - 4.5, ry - 4.5, 9.0, 9.0), 45 * 16, 90 * 16);
    painter.setBrush(iconCol);
    painter.drawEllipse(QRectF(rx - 1.2, ry + 1.0, 2.4, 2.4));

    // 3. Draw Battery icon
    const qreal bx = width() - 32.0;
    const qreal by = ry - 5.0;
    painter.setPen(QPen(iconCol, 1.5));
    painter.setBrush(Qt::NoBrush);
    painter.drawRoundedRect(QRectF(bx, by, 18.0, 10.0), 2.0, 2.0);
    painter.fillRect(QRectF(bx + 18.0, by + 3.0, 2.0, 4.0), iconCol);
    // Green battery fill
    painter.fillRect(QRectF(bx + 2.0, by + 2.0, 14.0, 6.0), QColor(53, 232, 125));
}

SwitchTitleBar::SwitchTitleBar(QWidget* parent) : QWidget{parent} {
    setFixedHeight(36);

    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(32, 2, 32, 2);
    layout->setSpacing(8);

    m_title_label = new QLabel(QStringLiteral("⬚ Dalia"), this);
    QFont font = m_title_label->font();
    font.setBold(true);
    font.setPointSize(14);
    m_title_label->setFont(font);

    layout->addWidget(m_title_label);
    layout->addStretch();
}

void SwitchTitleBar::SetTitle(const QString& title) {
    const QString display = title.trimmed().isEmpty() ? QStringLiteral("Dalia") : title.trimmed();
    m_title_label->setText(QStringLiteral("⬚ %1").arg(display));

    const bool isDark = UISettings::IsDarkTheme();
    m_title_label->setStyleSheet(
        isDark ? QStringLiteral("color: #00D2FF; font-weight: bold;")
               : QStringLiteral("color: #009AC7; font-weight: bold;"));
}

SwitchActionBar::SwitchActionBar(MainWindow* main_window, GameList* game_list, QWidget* parent)
    : QWidget{parent}, m_main_window{main_window}, m_game_list{game_list} {
    setFixedHeight(72);

    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(10, 8, 10, 8);
    layout->setSpacing(22);
    layout->setAlignment(Qt::AlignCenter);

    auto* btnNews = new SwitchRoundButton(SwitchButtonType::News, this);
    auto* btnShop = new SwitchRoundButton(SwitchButtonType::Shop, this);
    auto* btnAlbum = new SwitchRoundButton(SwitchButtonType::Album, this);
    auto* btnControllers = new SwitchRoundButton(SwitchButtonType::Controllers, this);
    auto* btnSettings = new SwitchRoundButton(SwitchButtonType::Settings, this);
    auto* btnDiagnostics = new SwitchRoundButton(SwitchButtonType::Diagnostics, this);
    auto* btnPower = new SwitchRoundButton(SwitchButtonType::Power, this);

    connect(btnNews, &QPushButton::clicked, this, [this] {
        if (m_game_list)
            m_game_list->RefreshGameDirectory();
    });

    connect(btnShop, &QPushButton::clicked, this, [this] {
        if (m_game_list)
            emit m_game_list->AddDirectory();
    });

    connect(btnAlbum, &QPushButton::clicked, this, [] {
        const auto captures_dir = Common::FS::GetEdenPath(Common::FS::EdenPath::ScreenshotsDir);
        QDesktopServices::openUrl(QUrl::fromLocalFile(QString::fromStdString(Common::FS::PathToUTF8String(captures_dir))));
    });

    connect(btnControllers, &QPushButton::clicked, this, [this] {
        if (m_main_window && m_main_window->controller_dialog)
            m_main_window->controller_dialog->show();
    });

    connect(btnSettings, &QPushButton::clicked, this, [this] {
        if (m_main_window) {
            DaliaSettingsDialog dlg(m_main_window, m_main_window->vk_device_records);
            dlg.exec();
        }
    });

    connect(btnDiagnostics, &QPushButton::clicked, this, [this] {
        if (m_main_window) {
            DaliaDiagnosticsDialog dlg(m_main_window, m_main_window->vk_device_records);
            dlg.exec();
        }
    });

    connect(btnPower, &QPushButton::clicked, this, [this] {
        if (m_main_window)
            m_main_window->close();
    });

    layout->addWidget(btnNews);
    layout->addWidget(btnShop);
    layout->addWidget(btnAlbum);
    layout->addWidget(btnControllers);
    layout->addWidget(btnSettings);
    layout->addWidget(btnDiagnostics);
    layout->addWidget(btnPower);
}

SwitchFooterBar::SwitchFooterBar(MainWindow* main_window, GameList* game_list, QWidget* parent)
    : QWidget{parent}, m_main_window{main_window}, m_game_list{game_list} {
    setFixedHeight(44);

    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(32, 6, 32, 6);
    layout->setSpacing(16);

    layout->addStretch();

    m_options_button = new QPushButton(QStringLiteral("<b>—/+</b> Options"), this);
    m_options_button->setCursor(Qt::PointingHandCursor);
    m_options_button->setStyleSheet(QStringLiteral(
        "QPushButton { background: transparent; color: #8E9297; font-size: 13px; border: none; padding: 4px 10px; font-weight: 500; }"
        "QPushButton:hover { color: #00C3E3; }"));
    connect(m_options_button, &QPushButton::clicked, this, &SwitchFooterBar::OptionsRequested);
    layout->addWidget(m_options_button);

    m_start_button = new QPushButton(QStringLiteral(" Ⓐ Start "), this);
    m_start_button->setCursor(Qt::PointingHandCursor);
    m_start_button->setStyleSheet(QStringLiteral(
        "QPushButton { background: #383838; color: #FFFFFF; font-weight: bold; border-radius: 12px; padding: 4px 14px; font-size: 12px; border: none; }"
        "QPushButton:hover { background: #00C3E3; color: #000000; }"
    ));
    connect(m_start_button, &QPushButton::clicked, this, &SwitchFooterBar::StartRequested);
    layout->addWidget(m_start_button);
}

void SwitchFooterBar::paintEvent(QPaintEvent* event) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);

    const bool isDark = UISettings::IsDarkTheme();

    // Thin divider line at the top
    painter.setPen(QPen(isDark ? QColor(56, 58, 62) : QColor(210, 210, 210), 1.0));
    painter.drawLine(0, 0, width(), 0);

    // Switch Handheld console icon on the left
    const qreal cx = 32.0;
    const qreal cy = 12.0;
    const QColor consoleCol = isDark ? QColor(220, 220, 220) : QColor(45, 45, 45);

    // Screen
    painter.setPen(QPen(consoleCol, 1.6));
    painter.setBrush(Qt::NoBrush);
    painter.drawRoundedRect(QRectF(cx + 6.0, cy, 14.0, 18.0), 2.0, 2.0);

    // Left Joy-Con (Neon Blue)
    painter.setBrush(QColor(10, 185, 230));
    painter.drawRoundedRect(QRectF(cx, cy, 5.0, 18.0), 2.0, 2.0);

    // Right Joy-Con (Neon Red)
    painter.setBrush(QColor(255, 60, 40));
    painter.drawRoundedRect(QRectF(cx + 21.0, cy, 5.0, 18.0), 2.0, 2.0);
}

