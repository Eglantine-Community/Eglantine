// SPDX-FileCopyrightText: Copyright 2026 DALIA Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <QHBoxLayout>
#include <QIcon>
#include <QKeyEvent>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QPushButton>
#include <QVBoxLayout>

#include "yuzu/dalia/dalia_sidebar.h"
#include "yuzu/dalia/dalia_theme.h"

namespace {

QIcon CreateNavIcon(const QString& kind, const QColor& color) {
    constexpr int size = 20;
    QPixmap pm(size, size);
    pm.fill(Qt::transparent);

    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    QPen pen(color, 1.7, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
    p.setPen(pen);
    p.setBrush(Qt::NoBrush);

    if (kind == QLatin1String("home")) {
        QPainterPath roof;
        roof.moveTo(3, 9.5);
        roof.lineTo(10, 3.5);
        roof.lineTo(17, 9.5);
        p.drawPath(roof);
        p.drawRoundedRect(QRectF(5, 9.5, 10, 7.5), 1.5, 1.5);
    } else if (kind == QLatin1String("games")) {
        // Sleek Switch/Shelf books-library icon
        p.drawRoundedRect(QRectF(3, 4, 3.5, 12), 1.0, 1.0);
        p.drawRoundedRect(QRectF(8.2, 4, 3.5, 12), 1.0, 1.0);
        QPainterPath tilt;
        tilt.moveTo(13.5, 4.5);
        tilt.lineTo(16.8, 15.5);
        p.drawPath(tilt);
    } else if (kind == QLatin1String("controllers")) {
        p.drawRoundedRect(QRectF(2.5, 6, 15, 8.5), 4.0, 4.0);
        p.drawPoint(QPointF(6.5, 10.2));
        p.drawLine(QPointF(5.2, 10.2), QPointF(7.8, 10.2));
        p.drawLine(QPointF(6.5, 8.9), QPointF(6.5, 11.5));
        p.setBrush(color);
        p.drawEllipse(QPointF(13.8, 9.2), 1.0, 1.0);
        p.drawEllipse(QPointF(12.3, 10.8), 1.0, 1.0);
    } else if (kind == QLatin1String("settings")) {
        p.drawEllipse(QPointF(10, 10), 2.8, 2.8);
        p.drawEllipse(QPointF(10, 10), 6.2, 6.2);
    } else if (kind == QLatin1String("system")) {
        QPainterPath wave;
        wave.moveTo(2.5, 13.5);
        wave.lineTo(6.5, 9.5);
        wave.lineTo(10.0, 12.0);
        wave.lineTo(14.0, 6.0);
        wave.lineTo(17.5, 8.5);
        p.drawPath(wave);
    } else if (kind == QLatin1String("menu")) {
        p.drawLine(QPointF(4, 6), QPointF(16, 6));
        p.drawLine(QPointF(4, 10), QPointF(16, 10));
        p.drawLine(QPointF(4, 14), QPointF(16, 14));
    }

    p.end();
    return QIcon(pm);
}

} // namespace

DaliaSidebar::DaliaSidebar(QWidget* parent) : QFrame{parent} {
    setObjectName(QStringLiteral("DaliaSidebar"));
    setFixedWidth(186);
    setStyleSheet(DaliaTheme::GetSidebarStyle());

    SetupUI();
}

DaliaSidebar::~DaliaSidebar() = default;

void DaliaSidebar::SetupUI() {
    auto* main_layout = new QVBoxLayout(this);
    main_layout->setContentsMargins(0, 16, 0, 14);
    main_layout->setSpacing(4);

    // 1. Brand Logo Header
    auto* logo_container = new QWidget(this);
    auto* logo_layout = new QHBoxLayout(logo_container);
    logo_layout->setContentsMargins(18, 4, 16, 18);
    logo_layout->setSpacing(10);

    auto* logo_badge = new QLabel(logo_container);
    logo_badge->setFixedSize(28, 28);
    logo_badge->setAlignment(Qt::AlignCenter);

    QPixmap dalia_icon(QStringLiteral(":/icons/default/256x256/dalia.png"));
    if (dalia_icon.isNull()) {
        dalia_icon = QPixmap(QStringLiteral(":/img/dalia.png"));
    }
    if (dalia_icon.isNull()) {
        dalia_icon = QPixmap(QStringLiteral(":/icons/default/256x256/lichii.png"));
    }
    if (dalia_icon.isNull()) {
        dalia_icon = QPixmap(QCoreApplication::applicationDirPath() + QStringLiteral("/dist/dalia.png"));
    }
    if (dalia_icon.isNull()) {
        dalia_icon = QPixmap(QStringLiteral("dist/dalia.png"));
    }
    if (!dalia_icon.isNull()) {
        QPixmap scaled(28, 28);
        scaled.fill(Qt::transparent);
        QPainter p(&scaled);
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setRenderHint(QPainter::SmoothPixmapTransform, true);
        QPainterPath clip;
        clip.addRoundedRect(0, 0, 28, 28, 7, 7);
        p.setClipPath(clip);
        p.drawPixmap(0, 0, 28, 28,
                     dalia_icon.scaled(28, 28, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation));
        p.end();
        logo_badge->setPixmap(scaled);
    } else {
        logo_badge->setText(QStringLiteral("D"));
        logo_badge->setStyleSheet(QStringLiteral(
            "background-color: %1; color: #FFFFFF; font-weight: 900; font-size: 14px; border-radius: 7px;")
            .arg(DaliaTheme::ColorAccentCoral));
    }
    logo_layout->addWidget(logo_badge);

    auto* brand_label = new QLabel(QStringLiteral("DALIA"), logo_container);
    QFont brand_font = brand_label->font();
    brand_font.setPointSize(14);
    brand_font.setBold(true);
    brand_label->setFont(brand_font);
    brand_label->setStyleSheet(QStringLiteral("color: #FFFFFF; letter-spacing: 1.5px;"));
    logo_layout->addWidget(brand_label);
    logo_layout->addStretch();

    main_layout->addWidget(logo_container);

    // 2. Navigation items
    struct NavItem {
        QString kind;
        QString text;
    };

    const std::vector<NavItem> items = {
        {QStringLiteral("home"), QStringLiteral("Inicio")},
        {QStringLiteral("games"), QStringLiteral("Juegos")},
        {QStringLiteral("controllers"), QStringLiteral("Mandos")},
        {QStringLiteral("settings"), QStringLiteral("Ajustes")},
        {QStringLiteral("system"), QStringLiteral("Sistema")},
    };

    m_nav_buttons.reserve(items.size());
    m_nav_kinds.reserve(items.size());

    for (size_t i = 0; i < items.size(); ++i) {
        const auto& item = items[i];
        auto* btn = new QPushButton(QStringLiteral("  %1").arg(item.text), this);
        btn->setCursor(Qt::PointingHandCursor);
        btn->setFixedHeight(42);
        btn->setIconSize(QSize(18, 18));
        btn->setFocusPolicy(Qt::StrongFocus);

        const int nav_index = static_cast<int>(i);
        connect(btn, &QPushButton::clicked, this, [this, nav_index] {
            SetCurrentIndex(nav_index);
            emit NavigationRequested(nav_index);
        });

        m_nav_buttons.push_back(btn);
        m_nav_kinds.push_back(item.kind);
        main_layout->addWidget(btn);
    }

    main_layout->addStretch();

    // 3. Footer Actions (Menu & User Profile)
    auto* menu_btn = new QPushButton(QStringLiteral("  Más opciones"), this);
    menu_btn->setCursor(Qt::PointingHandCursor);
    menu_btn->setFixedHeight(36);
    menu_btn->setIcon(CreateNavIcon(QStringLiteral("menu"), QColor(DaliaTheme::ColorTextSecondary)));
    menu_btn->setIconSize(QSize(16, 16));
    menu_btn->setStyleSheet(QStringLiteral(
        "QPushButton { background: transparent; color: %1; border: none; padding: 6px 18px; text-align: left; font-size: 12px; font-weight: 500; border-radius: 6px; margin: 0px 6px; }"
        "QPushButton:hover { background: %2; color: #FFFFFF; }")
        .arg(DaliaTheme::ColorTextSecondary, DaliaTheme::ColorSurfaceCard));
    connect(menu_btn, &QPushButton::clicked, this, [this, menu_btn] {
        emit MenuRequested(menu_btn->mapToGlobal(QPoint(menu_btn->width(), 0)));
    });
    main_layout->addWidget(menu_btn);

    auto* user_btn = new QPushButton(this);
    user_btn->setCursor(Qt::PointingHandCursor);
    user_btn->setFixedHeight(40);
    user_btn->setStyleSheet(QStringLiteral(
        "QPushButton { background: transparent; border: none; padding: 6px 16px; text-align: left; border-radius: 8px; margin: 2px 6px; }"
        "QPushButton:hover { background: %1; }")
        .arg(DaliaTheme::ColorSurfaceCard));

    auto* user_layout = new QHBoxLayout(user_btn);
    user_layout->setContentsMargins(6, 2, 6, 2);
    user_layout->setSpacing(8);

    auto* user_label = new QLabel(QStringLiteral("Perfil activo"), user_btn);
    user_label->setStyleSheet(QStringLiteral("color: #D2D7E0; font-size: 12px; font-weight: 600;"));
    user_layout->addWidget(user_label);
    user_layout->addStretch();

    auto* status_dot = new QLabel(QStringLiteral("●"), user_btn);
    status_dot->setStyleSheet(QStringLiteral("color: %1; font-size: 10px;").arg(DaliaTheme::ColorStatusGreen));
    user_layout->addWidget(status_dot);

    connect(user_btn, &QPushButton::clicked, this, &DaliaSidebar::UserProfileRequested);
    main_layout->addWidget(user_btn);

    UpdateSelectionStyles();
}

void DaliaSidebar::SetCurrentIndex(int index) {
    if (index >= 0 && index < static_cast<int>(m_nav_buttons.size())) {
        m_current_index = index;
        UpdateSelectionStyles();
    }
}

void DaliaSidebar::UpdateSelectionStyles() {
    for (size_t i = 0; i < m_nav_buttons.size(); ++i) {
        const bool active = (static_cast<int>(i) == m_current_index);
        m_nav_buttons[i]->setStyleSheet(DaliaTheme::GetSidebarButtonStyle(active));
        const QColor icon_col = active ? QColor(DaliaTheme::ColorTextPrimary)
                                       : QColor(DaliaTheme::ColorTextSecondary);
        m_nav_buttons[i]->setIcon(CreateNavIcon(m_nav_kinds[i], icon_col));
    }
}

void DaliaSidebar::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Up) {
        if (m_current_index > 0) {
            SetCurrentIndex(m_current_index - 1);
            emit NavigationRequested(m_current_index);
            m_nav_buttons[m_current_index]->setFocus();
        }
        event->accept();
        return;
    } else if (event->key() == Qt::Key_Down) {
        if (m_current_index + 1 < static_cast<int>(m_nav_buttons.size())) {
            SetCurrentIndex(m_current_index + 1);
            emit NavigationRequested(m_current_index);
            m_nav_buttons[m_current_index]->setFocus();
        }
        event->accept();
        return;
    } else if (event->key() == Qt::Key_Right || event->key() == Qt::Key_Return) {
        emit FocusContentRequested();
        event->accept();
        return;
    }

    QFrame::keyPressEvent(event);
}
