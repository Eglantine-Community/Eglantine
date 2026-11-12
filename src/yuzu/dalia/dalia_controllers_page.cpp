// SPDX-FileCopyrightText: Copyright 2026 DALIA Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <QCheckBox>
#include <QComboBox>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>

#include "common/settings.h"
#include "common/settings_input.h"
#include "qt_common/qt_common.h"
#include "yuzu/dalia/dalia_controllers_page.h"
#include "yuzu/dalia/dalia_theme.h"
#include "yuzu/debugger/controller.h"
#include "yuzu/main_window.h"

DaliaControllersPage::DaliaControllersPage(MainWindow* main_window, QWidget* parent)
    : QWidget{parent}, m_main_window{main_window} {
    SetupUI();
    LoadPlayerSettings(0);
}

DaliaControllersPage::~DaliaControllersPage() = default;

void DaliaControllersPage::SetupUI() {
    auto* main_layout = new QVBoxLayout(this);
    main_layout->setContentsMargins(28, 22, 28, 16);
    main_layout->setSpacing(18);

    // 1. Header
    auto* title_box = new QVBoxLayout();
    title_box->setSpacing(2);

    auto* title_label = new QLabel(QStringLiteral("Mandos y Controles"), this);
    QFont title_font = title_label->font();
    title_font.setPointSize(20);
    title_font.setBold(true);
    title_label->setFont(title_font);
    title_label->setStyleSheet(QStringLiteral("color: %1; letter-spacing: 0.3px;").arg(DaliaTheme::ColorTextPrimary));

    auto* subtitle_label = new QLabel(QStringLiteral("Gestión de mandos físicos, perfiles de jugador y calibración háptica"), this);
    subtitle_label->setStyleSheet(QStringLiteral("color: %1; font-size: 13px; font-weight: 500;").arg(DaliaTheme::ColorTextSecondary));

    title_box->addWidget(title_label);
    title_box->addWidget(subtitle_label);
    main_layout->addLayout(title_box);

    // 2. Player Slots Row (Jugador 1 a 4)
    auto* slots_layout = new QHBoxLayout();
    slots_layout->setSpacing(12);

    m_player_buttons.reserve(4);
    for (int i = 0; i < 4; ++i) {
        auto* btn = new QPushButton(this);
        btn->setCursor(Qt::PointingHandCursor);
        btn->setFixedHeight(54);

        auto* btn_layout = new QHBoxLayout(btn);
        btn_layout->setContentsMargins(14, 8, 14, 8);
        btn_layout->setSpacing(10);

        auto* dot = new QLabel(QStringLiteral("●"), btn);
        dot->setObjectName(QStringLiteral("status_dot"));
        dot->setStyleSheet(i == 0 ? QStringLiteral("color: %1; font-size: 12px;").arg(DaliaTheme::ColorStatusGreen)
                                  : QStringLiteral("color: %1; font-size: 12px;").arg(DaliaTheme::ColorTextTertiary));
        btn_layout->addWidget(dot);

        auto* name = new QLabel(QStringLiteral("Jugador %1").arg(i + 1), btn);
        name->setStyleSheet(QStringLiteral("color: #FFFFFF; font-weight: bold; font-size: 13px;"));
        btn_layout->addWidget(name);
        btn_layout->addStretch();

        connect(btn, &QPushButton::clicked, this, [this, i] {
            LoadPlayerSettings(i);
        });

        m_player_buttons.push_back(btn);
        slots_layout->addWidget(btn);
    }
    main_layout->addLayout(slots_layout);

    // 3. Active Player Configuration Card
    auto* card_frame = new QFrame(this);
    card_frame->setStyleSheet(QStringLiteral(
        "QFrame { background-color: %1; border: 1px solid %2; border-radius: 14px; }")
        .arg(DaliaTheme::ColorSurfaceCard, DaliaTheme::ColorBorderCard));

    auto* card_layout = new QVBoxLayout(card_frame);
    card_layout->setContentsMargins(24, 22, 24, 22);
    card_layout->setSpacing(18);

    // Card Header Strip
    auto* card_header = new QHBoxLayout();
    m_status_badge = new QLabel(QStringLiteral("● CONECTADO"), card_frame);
    m_status_badge->setStyleSheet(QStringLiteral(
        "background-color: rgba(53, 232, 125, 0.15); color: %1; border: 1px solid rgba(53, 232, 125, 0.4); "
        "border-radius: 8px; padding: 4px 12px; font-weight: bold; font-size: 11px; letter-spacing: 1px;")
        .arg(DaliaTheme::ColorStatusGreen));
    card_header->addWidget(m_status_badge);

    m_device_name_label = new QLabel(QStringLiteral("Dispositivo asignado: Teclado / Ratón"), card_frame);
    m_device_name_label->setStyleSheet(QStringLiteral("color: %1; font-size: 12px; font-weight: 500;").arg(DaliaTheme::ColorTextSecondary));
    card_header->addWidget(m_device_name_label);
    card_header->addStretch();

    m_check_connected = new QCheckBox(QStringLiteral("Conectar este mando"), card_frame);
    m_check_connected->setChecked(true);
    m_check_connected->setStyleSheet(QStringLiteral("color: #FFFFFF; font-size: 12px; font-weight: 600;"));
    connect(m_check_connected, &QCheckBox::toggled, this, [this] {
        SavePlayerSettings();
    });
    card_header->addWidget(m_check_connected);

    card_layout->addLayout(card_header);

    // Grid Options
    auto* grid = new QGridLayout();
    grid->setHorizontalSpacing(24);
    grid->setVerticalSpacing(16);

    auto* lbl_type = new QLabel(QStringLiteral("Tipo de Mando Emulado:"), card_frame);
    lbl_type->setStyleSheet(QStringLiteral("color: %1; font-size: 12px; font-weight: 500;").arg(DaliaTheme::ColorTextSecondary));
    grid->addWidget(lbl_type, 0, 0);

    m_combo_controller_type = new QComboBox(card_frame);
    m_combo_controller_type->addItem(QStringLiteral("Nintendo Switch Pro Controller"), static_cast<int>(Settings::ControllerType::ProController));
    m_combo_controller_type->addItem(QStringLiteral("Joy-Cons Acoplados (Dual)"), static_cast<int>(Settings::ControllerType::DualJoyconDetached));
    m_combo_controller_type->addItem(QStringLiteral("Joy-Con Izquierdo (Individual)"), static_cast<int>(Settings::ControllerType::LeftJoycon));
    m_combo_controller_type->addItem(QStringLiteral("Joy-Con Derecho (Individual)"), static_cast<int>(Settings::ControllerType::RightJoycon));
    m_combo_controller_type->addItem(QStringLiteral("Consola Portátil (Handheld)"), static_cast<int>(Settings::ControllerType::Handheld));
    m_combo_controller_type->addItem(QStringLiteral("Mando de GameCube"), static_cast<int>(Settings::ControllerType::GameCube));

    m_combo_controller_type->setStyleSheet(QStringLiteral(
        "QComboBox { background: %1; color: #FFFFFF; border: 1px solid %2; border-radius: 8px; padding: 6px 14px; font-weight: 600; min-width: 260px; font-size: 12px; }"
        "QComboBox:hover { border-color: %3; }")
        .arg(DaliaTheme::ColorSurfaceElevated, DaliaTheme::ColorBorderCard, DaliaTheme::ColorAccentIceBlue));

    connect(m_combo_controller_type, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this] {
        SavePlayerSettings();
    });
    grid->addWidget(m_combo_controller_type, 0, 1);

    m_check_vibration = new QCheckBox(QStringLiteral("Activar Vibración Háptica (HD Rumble)"), card_frame);
    m_check_vibration->setStyleSheet(QStringLiteral("color: #FFFFFF; font-size: 12px; font-weight: 500;"));
    connect(m_check_vibration, &QCheckBox::toggled, this, [this] {
        SavePlayerSettings();
    });
    grid->addWidget(m_check_vibration, 1, 0);

    m_check_motion = new QCheckBox(QStringLiteral("Activar Controles por Movimiento (Giroscopio)"), card_frame);
    m_check_motion->setStyleSheet(QStringLiteral("color: #FFFFFF; font-size: 12px; font-weight: 500;"));
    connect(m_check_motion, &QCheckBox::toggled, this, [this] {
        SavePlayerSettings();
    });
    grid->addWidget(m_check_motion, 1, 1);

    card_layout->addLayout(grid);
    main_layout->addWidget(card_frame);

    // 4. Action Buttons
    auto* actions_layout = new QHBoxLayout();
    actions_layout->setSpacing(12);

    m_btn_open_mapping = new QPushButton(QStringLiteral("🎮  Abrir Asignador Avanzado de Botones y Calibración"), this);
    m_btn_open_mapping->setCursor(Qt::PointingHandCursor);
    m_btn_open_mapping->setStyleSheet(QStringLiteral(
        "QPushButton { background: %1; color: #101318; font-weight: bold; border-radius: 8px; padding: 10px 22px; font-size: 12px; border: none; }"
        "QPushButton:hover { background: #5EC4F0; }")
        .arg(DaliaTheme::ColorAccentIceBlue));

    connect(m_btn_open_mapping, &QPushButton::clicked, this, [this] {
        if (m_main_window && m_main_window->controller_dialog) {
            m_main_window->controller_dialog->show();
            m_main_window->controller_dialog->raise();
        }
    });
    actions_layout->addWidget(m_btn_open_mapping);

    m_btn_refresh = new QPushButton(QStringLiteral("🔄 Detectar y Refrescar Mandos"), this);
    m_btn_refresh->setCursor(Qt::PointingHandCursor);
    m_btn_refresh->setStyleSheet(QStringLiteral(
        "QPushButton { background: %1; color: #FFFFFF; font-weight: 600; border-radius: 8px; padding: 10px 18px; font-size: 12px; border: 1px solid %2; }"
        "QPushButton:hover { background: %3; }")
        .arg(DaliaTheme::ColorSurfaceElevated, DaliaTheme::ColorBorderCard, DaliaTheme::ColorSurfaceHover));
    connect(m_btn_refresh, &QPushButton::clicked, this, &DaliaControllersPage::RefreshControllerDetection);
    actions_layout->addWidget(m_btn_refresh);

    m_feedback_label = new QLabel(this);
    m_feedback_label->setStyleSheet(QStringLiteral("color: %1; font-size: 12px; font-weight: bold;").arg(DaliaTheme::ColorStatusGreen));
    actions_layout->addWidget(m_feedback_label);
    actions_layout->addStretch();

    main_layout->addLayout(actions_layout);
    main_layout->addStretch();
}

void DaliaControllersPage::LoadPlayerSettings(int player_index) {
    if (player_index < 0 || player_index >= 4) return;
    m_selected_player = player_index;

    // Update Player buttons styling
    for (size_t i = 0; i < m_player_buttons.size(); ++i) {
        const bool active = (static_cast<int>(i) == m_selected_player);
        if (active) {
            m_player_buttons[i]->setStyleSheet(QStringLiteral(
                "QPushButton { background-color: %1; border: 1px solid %2; border-radius: 10px; }")
                .arg(DaliaTheme::ColorSurfaceElevated, DaliaTheme::ColorAccentIceBlue));
        } else {
            m_player_buttons[i]->setStyleSheet(QStringLiteral(
                "QPushButton { background-color: %1; border: 1px solid %2; border-radius: 10px; }"
                "QPushButton:hover { background-color: %3; }")
                .arg(DaliaTheme::ColorSurfaceCard, DaliaTheme::ColorBorderCard, DaliaTheme::ColorSurfaceElevated));
        }
    }

    const auto& players = Settings::values.players.GetValue();
    if (m_selected_player < static_cast<int>(players.size())) {
        const auto& p = players[m_selected_player];

        m_check_connected->blockSignals(true);
        m_check_connected->setChecked(p.connected);
        m_check_connected->blockSignals(false);

        m_combo_controller_type->blockSignals(true);
        const int type_val = static_cast<int>(p.controller_type);
        for (int i = 0; i < m_combo_controller_type->count(); ++i) {
            if (m_combo_controller_type->itemData(i).toInt() == type_val) {
                m_combo_controller_type->setCurrentIndex(i);
                break;
            }
        }
        m_combo_controller_type->blockSignals(false);

        m_check_vibration->blockSignals(true);
        m_check_vibration->setChecked(p.vibration_enabled);
        m_check_vibration->blockSignals(false);

        m_check_motion->blockSignals(true);
        m_check_motion->setChecked(Settings::values.motion_enabled.GetValue());
        m_check_motion->blockSignals(false);

        if (p.connected) {
            m_status_badge->setText(QStringLiteral("● CONECTADO"));
            m_status_badge->setStyleSheet(QStringLiteral(
                "background-color: rgba(53, 232, 125, 0.15); color: %1; border: 1px solid rgba(53, 232, 125, 0.4); "
                "border-radius: 8px; padding: 4px 12px; font-weight: bold; font-size: 11px; letter-spacing: 1px;")
                .arg(DaliaTheme::ColorStatusGreen));
        } else {
            m_status_badge->setText(QStringLiteral("○ DESCONECTADO"));
            m_status_badge->setStyleSheet(QStringLiteral(
                "background-color: rgba(142, 146, 151, 0.15); color: %1; border: 1px solid rgba(142, 146, 151, 0.4); "
                "border-radius: 8px; padding: 4px 12px; font-weight: bold; font-size: 11px; letter-spacing: 1px;")
                .arg(DaliaTheme::ColorTextSecondary));
        }

        const QString profile_name = QString::fromStdString(p.profile_name);
        m_device_name_label->setText(profile_name.isEmpty()
            ? QStringLiteral("Dispositivo: Automático / Predeterminado")
            : QStringLiteral("Perfil: %1").arg(profile_name));
    }
}

void DaliaControllersPage::SavePlayerSettings() {
    auto& players = Settings::values.players.GetValue();
    if (m_selected_player < static_cast<int>(players.size())) {
        players[m_selected_player].connected = m_check_connected->isChecked();
        players[m_selected_player].controller_type =
            static_cast<Settings::ControllerType>(m_combo_controller_type->currentData().toInt());
        players[m_selected_player].vibration_enabled = m_check_vibration->isChecked();

        Settings::values.motion_enabled.SetValue(m_check_motion->isChecked());

        if (QtCommon::system) {
            QtCommon::system->ApplySettings();
        }
        if (m_main_window) {
            m_main_window->OnSaveConfig();
        }

        m_feedback_label->setText(QStringLiteral("✓ Guardado"));
        QTimer::singleShot(2000, m_feedback_label, [this] { m_feedback_label->clear(); });

        LoadPlayerSettings(m_selected_player);
    }
}

void DaliaControllersPage::RefreshControllerDetection() {
    QtCommon::SetupHID();
    if (m_main_window && m_main_window->controller_dialog) {
        m_main_window->controller_dialog->refreshConfiguration();
    }
    LoadPlayerSettings(m_selected_player);
    m_feedback_label->setText(QStringLiteral("✓ Mandos detectados"));
    QTimer::singleShot(2500, m_feedback_label, [this] { m_feedback_label->clear(); });
}

void DaliaControllersPage::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Left) {
        emit FocusSidebarRequested();
        event->accept();
        return;
    }
    QWidget::keyPressEvent(event);
}
