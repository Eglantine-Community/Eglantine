// SPDX-FileCopyrightText: Copyright 2026 DALIA Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <QFrame>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>

#include "qt_common/config/uisettings.h"
#include "yuzu/dalia/dalia_library_page.h"
#include "yuzu/dalia/dalia_theme.h"
#include "yuzu/game/game_list.h"
#include "yuzu/main_window.h"

DaliaLibraryPage::DaliaLibraryPage(MainWindow* main_window, GameList* game_list, QWidget* parent)
    : QWidget{parent}, m_main_window{main_window}, m_game_list{game_list} {
    SetupUI();
    SyncViewModeButtons();
}

DaliaLibraryPage::~DaliaLibraryPage() = default;

void DaliaLibraryPage::SetupUI() {
    auto* main_layout = new QVBoxLayout(this);
    main_layout->setContentsMargins(28, 22, 28, 12);
    main_layout->setSpacing(14);

    // 1. Header Section
    auto* header_layout = new QHBoxLayout();
    header_layout->setSpacing(12);

    auto* title_box = new QVBoxLayout();
    title_box->setSpacing(2);

    auto* title_label = new QLabel(QStringLiteral("Tu biblioteca"), this);
    QFont title_font = title_label->font();
    title_font.setPointSize(20);
    title_font.setBold(true);
    title_label->setFont(title_font);
    title_label->setStyleSheet(QStringLiteral("color: %1; letter-spacing: 0.3px;").arg(DaliaTheme::ColorTextPrimary));

    auto* subtitle_label = new QLabel(QStringLiteral("Tus juegos, organizados en un solo lugar."), this);
    subtitle_label->setStyleSheet(QStringLiteral("color: %1; font-size: 13px; font-weight: 500;").arg(DaliaTheme::ColorTextSecondary));

    title_box->addWidget(title_label);
    title_box->addWidget(subtitle_label);
    header_layout->addLayout(title_box);
    header_layout->addStretch();

    // Compact View Mode Switcher
    auto* view_switch = new QFrame(this);
    view_switch->setStyleSheet(QStringLiteral(
        "QFrame { background-color: %1; border: 1px solid %2; border-radius: 8px; padding: 2px; }")
        .arg(DaliaTheme::ColorSurfaceCard, DaliaTheme::ColorBorderCard));
    auto* vs_layout = new QHBoxLayout(view_switch);
    vs_layout->setContentsMargins(2, 2, 2, 2);
    vs_layout->setSpacing(2);

    m_btn_view_carousel = new QPushButton(QStringLiteral("Carrusel"), view_switch);
    m_btn_view_grid = new QPushButton(QStringLiteral("Cuadrícula"), view_switch);
    m_btn_view_list = new QPushButton(QStringLiteral("Lista"), view_switch);

    for (auto* btn : {m_btn_view_carousel, m_btn_view_grid, m_btn_view_list}) {
        btn->setCursor(Qt::PointingHandCursor);
        btn->setFixedHeight(28);
        vs_layout->addWidget(btn);
    }

    connect(m_btn_view_carousel, &QPushButton::clicked, this, [this] {
        if (m_main_window) {
            m_main_window->SetCarouselView();
            SyncViewModeButtons();
        }
    });
    connect(m_btn_view_grid, &QPushButton::clicked, this, [this] {
        if (m_main_window) {
            m_main_window->SetGridView();
            SyncViewModeButtons();
        }
    });
    connect(m_btn_view_list, &QPushButton::clicked, this, [this] {
        if (m_main_window) {
            m_main_window->SetTreeView();
            SyncViewModeButtons();
        }
    });

    header_layout->addWidget(view_switch);

    // Search bar
    m_search_edit = new QLineEdit(this);
    m_search_edit->setPlaceholderText(QStringLiteral("Buscar juego..."));
    m_search_edit->setClearButtonEnabled(true);
    m_search_edit->setFixedWidth(210);
    m_search_edit->setFixedHeight(34);
    m_search_edit->setStyleSheet(QStringLiteral(
        "QLineEdit {"
        "  background-color: %1;"
        "  color: %2;"
        "  border: 1px solid %3;"
        "  border-radius: 8px;"
        "  padding: 4px 12px;"
        "  font-size: 12px;"
        "}"
        "QLineEdit:focus {"
        "  border: 1px solid %4;"
        "}")
        .arg(DaliaTheme::ColorSurfaceCard, DaliaTheme::ColorTextPrimary,
             DaliaTheme::ColorBorderCard, DaliaTheme::ColorAccentIceBlue));

    connect(m_search_edit, &QLineEdit::textChanged, this, [this](const QString& text) {
        if (m_game_list) {
            m_game_list->OnTextChanged(text);
        }
    });
    header_layout->addWidget(m_search_edit);

    // Add Directory Button
    m_btn_add_dir = new QPushButton(QStringLiteral("+ Carpeta"), this);
    m_btn_add_dir->setToolTip(QStringLiteral("Añadir carpeta de juegos"));
    m_btn_add_dir->setCursor(Qt::PointingHandCursor);
    m_btn_add_dir->setFixedHeight(34);
    const QString action_btn_style = QStringLiteral(
        "QPushButton {"
        "  background-color: %1;"
        "  color: %2;"
        "  border: 1px solid %3;"
        "  border-radius: 8px;"
        "  padding: 4px 12px;"
        "  font-weight: 600;"
        "  font-size: 12px;"
        "}"
        "QPushButton:hover {"
        "  background-color: %4;"
        "  color: #FFFFFF;"
        "}")
        .arg(DaliaTheme::ColorSurfaceCard, DaliaTheme::ColorTextSecondary,
             DaliaTheme::ColorBorderCard, DaliaTheme::ColorSurfaceElevated);
    m_btn_add_dir->setStyleSheet(action_btn_style);

    connect(m_btn_add_dir, &QPushButton::clicked, this, [this] {
        if (m_game_list) {
            emit m_game_list->AddDirectory();
        }
    });
    header_layout->addWidget(m_btn_add_dir);

    // Refresh Button
    m_btn_refresh = new QPushButton(QStringLiteral("↻"), this);
    m_btn_refresh->setToolTip(QStringLiteral("Actualizar biblioteca"));
    m_btn_refresh->setCursor(Qt::PointingHandCursor);
    m_btn_refresh->setFixedSize(34, 34);
    m_btn_refresh->setStyleSheet(action_btn_style);
    connect(m_btn_refresh, &QPushButton::clicked, this, [this] {
        if (m_game_list) {
            m_game_list->RefreshGameDirectory();
        }
    });
    header_layout->addWidget(m_btn_refresh);

    main_layout->addLayout(header_layout);

    // 2. Embed GameList seamlessly
    if (m_game_list) {
        main_layout->addWidget(m_game_list, 1);
    }
}

void DaliaLibraryPage::SyncViewModeButtons() {
    const auto mode = UISettings::values.game_list_mode.GetValue();

    const QString active_style = QStringLiteral(
        "QPushButton { background-color: %1; color: %2; border: none; border-radius: 6px; padding: 4px 10px; font-weight: bold; font-size: 11px; }")
        .arg(DaliaTheme::ColorSurfaceElevated, DaliaTheme::ColorAccentIceBlue);
    const QString inactive_style = QStringLiteral(
        "QPushButton { background-color: transparent; color: %1; border: none; border-radius: 6px; padding: 4px 10px; font-weight: 500; font-size: 11px; }"
        "QPushButton:hover { color: #FFFFFF; }")
        .arg(DaliaTheme::ColorTextSecondary);

    m_btn_view_carousel->setStyleSheet(mode == Settings::GameListMode::CarouselView ? active_style : inactive_style);
    m_btn_view_grid->setStyleSheet(mode == Settings::GameListMode::GridView ? active_style : inactive_style);
    m_btn_view_list->setStyleSheet(mode == Settings::GameListMode::TreeView ? active_style : inactive_style);
}

void DaliaLibraryPage::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Left && (!m_search_edit->hasFocus())) {
        emit FocusSidebarRequested();
        event->accept();
        return;
    }

    QWidget::keyPressEvent(event);
}
