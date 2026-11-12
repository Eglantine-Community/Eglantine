// SPDX-FileCopyrightText: Copyright 2026 DALIA Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cmath>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QVBoxLayout>

#include "core/core.h"
#include "core/perf_stats.h"
#include "qt_common/qt_common.h"
#include "video_core/shader_notify.h"
#include "yuzu/dalia/dalia_diagnostics.h"
#include "yuzu/dalia/dalia_gameplay_overlay.h"
#include "yuzu/dalia/dalia_settings.h"
#include "yuzu/main_window.h"

DaliaGameplayOverlay::DaliaGameplayOverlay(MainWindow* parent)
    : QWidget{parent}, m_main_window{parent} {
    setAttribute(Qt::WA_TranslucentBackground);
    setWindowFlags(Qt::FramelessWindowHint | Qt::Tool | Qt::WindowStaysOnTopHint);
    setFixedSize(360, 420);

    SetupUI();

    if (m_main_window) {
        connect(m_main_window, &MainWindow::statsUpdated, this, &DaliaGameplayOverlay::UpdateStats);
        connect(m_main_window, &MainWindow::positionChanged, this, &DaliaGameplayOverlay::Reposition);
        Reposition();
    }
}

DaliaGameplayOverlay::~DaliaGameplayOverlay() = default;

void DaliaGameplayOverlay::SetupUI() {
    auto* main_layout = new QVBoxLayout(this);
    main_layout->setContentsMargins(18, 16, 18, 16);
    main_layout->setSpacing(12);

    // Header: Title, Status Dot, Close
    auto* header_layout = new QHBoxLayout();
    header_layout->setSpacing(8);

    m_status_indicator = new QLabel(QStringLiteral("●"), this);
    m_status_indicator->setStyleSheet(QStringLiteral("color: #35E87D; font-size: 14px;"));
    header_layout->addWidget(m_status_indicator);

    m_title_label = new QLabel(QStringLiteral("DALIA OVERLAY"), this);
    QFont title_font = m_title_label->font();
    title_font.setBold(true);
    title_font.setPointSize(11);
    m_title_label->setFont(title_font);
    m_title_label->setStyleSheet(QStringLiteral("color: #FFFFFF; letter-spacing: 1px;"));
    header_layout->addWidget(m_title_label, 1);

    m_btn_close = new QPushButton(QStringLiteral("✕"), this);
    m_btn_close->setFixedSize(24, 24);
    m_btn_close->setCursor(Qt::PointingHandCursor);
    m_btn_close->setStyleSheet(QStringLiteral(
        "QPushButton { background: transparent; color: #8E9297; font-weight: bold; border: none; font-size: 13px; }"
        "QPushButton:hover { color: #FFFFFF; }"));
    connect(m_btn_close, &QPushButton::clicked, this, &QWidget::hide);
    header_layout->addWidget(m_btn_close);
    main_layout->addLayout(header_layout);

    // Divider
    auto* divider = new QFrame(this);
    divider->setFrameShape(QFrame::HLine);
    divider->setStyleSheet(QStringLiteral("color: #2D313A; max-height: 1px;"));
    main_layout->addWidget(divider);

    // Performance Metrics Card
    auto* metrics_card = new QFrame(this);
    metrics_card->setStyleSheet(QStringLiteral(
        "QFrame { background-color: #161820; border-radius: 10px; border: 1px solid #282C37; padding: 6px; }"));
    auto* metrics_grid = new QGridLayout(metrics_card);
    metrics_grid->setContentsMargins(12, 10, 12, 10);
    metrics_grid->setHorizontalSpacing(14);
    metrics_grid->setVerticalSpacing(8);

    // FPS (Hero metric)
    auto* fps_title = new QLabel(QStringLiteral("GAME FRAMERATE"), metrics_card);
    fps_title->setStyleSheet(QStringLiteral("color: #8E9297; font-size: 10px; font-weight: bold; letter-spacing: 1px;"));
    metrics_grid->addWidget(fps_title, 0, 0, 1, 2);

    m_fps_value = new QLabel(QStringLiteral("60.0 FPS"), metrics_card);
    QFont fps_font = m_fps_value->font();
    fps_font.setPointSize(22);
    fps_font.setBold(true);
    m_fps_value->setFont(fps_font);
    m_fps_value->setStyleSheet(QStringLiteral("color: #00C3E3;"));
    metrics_grid->addWidget(m_fps_value, 1, 0, 1, 2);

    // Frametime
    auto* ft_title = new QLabel(QStringLiteral("Frametime:"), metrics_card);
    ft_title->setStyleSheet(QStringLiteral("color: #8E9297; font-size: 11px;"));
    metrics_grid->addWidget(ft_title, 2, 0);

    m_frametime_value = new QLabel(QStringLiteral("16.6 ms"), metrics_card);
    m_frametime_value->setStyleSheet(QStringLiteral("color: #FFFFFF; font-weight: bold; font-size: 12px;"));
    metrics_grid->addWidget(m_frametime_value, 2, 1);

    // Emulation Speed
    auto* speed_title = new QLabel(QStringLiteral("Emulation Speed:"), metrics_card);
    speed_title->setStyleSheet(QStringLiteral("color: #8E9297; font-size: 11px;"));
    metrics_grid->addWidget(speed_title, 3, 0);

    m_speed_value = new QLabel(QStringLiteral("100%"), metrics_card);
    m_speed_value->setStyleSheet(QStringLiteral("color: #FFFFFF; font-weight: bold; font-size: 12px;"));
    metrics_grid->addWidget(m_speed_value, 3, 1);

    // Shader Compiler Status
    auto* shader_title = new QLabel(QStringLiteral("Shader Pipeline:"), metrics_card);
    shader_title->setStyleSheet(QStringLiteral("color: #8E9297; font-size: 11px;"));
    metrics_grid->addWidget(shader_title, 4, 0);

    m_shaders_value = new QLabel(QStringLiteral("Ready (0 building)"), metrics_card);
    m_shaders_value->setStyleSheet(QStringLiteral("color: #35E87D; font-weight: bold; font-size: 11px;"));
    metrics_grid->addWidget(m_shaders_value, 4, 1);

    // GPU Engine
    auto* gpu_title = new QLabel(QStringLiteral("GPU Acceleration:"), metrics_card);
    gpu_title->setStyleSheet(QStringLiteral("color: #8E9297; font-size: 11px;"));
    metrics_grid->addWidget(gpu_title, 5, 0);

    m_gpu_value = new QLabel(QStringLiteral("Vulkan 1.3 (RDNA 3)"), metrics_card);
    m_gpu_value->setStyleSheet(QStringLiteral("color: #00C3E3; font-weight: bold; font-size: 11px;"));
    metrics_grid->addWidget(m_gpu_value, 5, 1);

    main_layout->addWidget(metrics_card);

    // Runtime Control Action Buttons
    auto* actions_layout = new QVBoxLayout();
    actions_layout->setSpacing(8);

    const QString btn_style = QStringLiteral(
        "QPushButton { background: #21242E; color: #FFFFFF; font-weight: 600; border-radius: 8px; padding: 7px 12px; border: 1px solid #363B4A; font-size: 11px; text-align: left; }"
        "QPushButton:hover { background: #2A2F3D; border-color: #00C3E3; color: #00C3E3; }");

    m_btn_pause = new QPushButton(QStringLiteral("⏸   Pause Game"), this);
    m_btn_pause->setCursor(Qt::PointingHandCursor);
    m_btn_pause->setStyleSheet(btn_style);
    connect(m_btn_pause, &QPushButton::clicked, this, [this] {
        if (!m_main_window) return;
        m_main_window->OnPauseContinueGame();
        UpdateControlsState();
    });
    actions_layout->addWidget(m_btn_pause);

    m_btn_screenshot = new QPushButton(QStringLiteral("📸  Capture Screenshot"), this);
    m_btn_screenshot->setCursor(Qt::PointingHandCursor);
    m_btn_screenshot->setStyleSheet(btn_style);
    connect(m_btn_screenshot, &QPushButton::clicked, this, [this] {
        if (m_main_window) m_main_window->OnCaptureScreenshot();
    });
    actions_layout->addWidget(m_btn_screenshot);

    m_btn_speed = new QPushButton(QStringLiteral("⚡  Speed Limit: 100% (Normal)"), this);
    m_btn_speed->setCursor(Qt::PointingHandCursor);
    m_btn_speed->setStyleSheet(btn_style);
    connect(m_btn_speed, &QPushButton::clicked, this, [this] {
        const bool current = Settings::values.use_speed_limit.GetValue();
        Settings::values.use_speed_limit.SetValue(!current);
        m_btn_speed->setText(!current ? QStringLiteral("⚡  Speed Limit: 100% (Normal)")
                                      : QStringLiteral("⚡  Speed Limit: Uncapped"));
    });
    actions_layout->addWidget(m_btn_speed);

    auto* sub_actions_layout = new QHBoxLayout();
    sub_actions_layout->setSpacing(8);

    m_btn_settings = new QPushButton(QStringLiteral("⚙️ Settings"), this);
    m_btn_settings->setCursor(Qt::PointingHandCursor);
    m_btn_settings->setStyleSheet(btn_style);
    connect(m_btn_settings, &QPushButton::clicked, this, [this] {
        if (m_main_window) {
            DaliaSettingsDialog dlg(m_main_window, m_main_window->vk_device_records);
            dlg.exec();
        }
    });
    sub_actions_layout->addWidget(m_btn_settings);

    m_btn_diagnostics = new QPushButton(QStringLiteral("🔍 Diagnostics"), this);
    m_btn_diagnostics->setCursor(Qt::PointingHandCursor);
    m_btn_diagnostics->setStyleSheet(btn_style);
    connect(m_btn_diagnostics, &QPushButton::clicked, this, [this] {
        if (m_main_window) {
            DaliaDiagnosticsDialog dlg(m_main_window, m_main_window->vk_device_records);
            dlg.exec();
        }
    });
    sub_actions_layout->addWidget(m_btn_diagnostics);
    actions_layout->addLayout(sub_actions_layout);

    m_btn_stop = new QPushButton(QStringLiteral("⏹   Return to Library"), this);
    m_btn_stop->setCursor(Qt::PointingHandCursor);
    m_btn_stop->setStyleSheet(QStringLiteral(
        "QPushButton { background: #2A171F; color: #FF4D6D; font-weight: bold; border-radius: 8px; padding: 7px 12px; border: 1px solid #5C2237; font-size: 11px; text-align: left; }"
        "QPushButton:hover { background: #472132; color: #FFA8B7; }"));
    connect(m_btn_stop, &QPushButton::clicked, this, [this] {
        if (m_main_window) {
            hide();
            m_main_window->OnStopGame();
        }
    });
    actions_layout->addWidget(m_btn_stop);

    main_layout->addLayout(actions_layout);
}

void DaliaGameplayOverlay::SetGameTitle(const QString& title, u64 program_id) {
    if (m_title_label) {
        m_title_label->setText(title.isEmpty() ? QStringLiteral("DALIA OVERLAY") : title);
    }
}

void DaliaGameplayOverlay::UpdateControlsState() {
    if (QtCommon::system && QtCommon::system->IsPoweredOn()) {
        const bool paused = QtCommon::system->IsPaused();
        if (m_btn_pause) {
            m_btn_pause->setText(paused ? QStringLiteral("▶   Resume Game") : QStringLiteral("⏸   Pause Game"));
        }
        if (m_status_indicator) {
            m_status_indicator->setText(QStringLiteral("●"));
            m_status_indicator->setStyleSheet(paused ? QStringLiteral("color: #FFB703; font-size: 14px;")
                                                     : QStringLiteral("color: #35E87D; font-size: 14px;"));
        }
    }
}

void DaliaGameplayOverlay::UpdateStats(const Core::PerfStatsResults& results,
                                      const VideoCore::ShaderNotify& shaders) {
    const double fps = results.average_game_fps;
    if (!std::isnan(fps) && fps >= 0.0) {
        m_fps_value->setText(QStringLiteral("%1 FPS").arg(QString::number(fps, 'f', 1)));
        if (fps >= 50.0) {
            m_fps_value->setStyleSheet(QStringLiteral("color: #35E87D;"));
        } else if (fps >= 28.0) {
            m_fps_value->setStyleSheet(QStringLiteral("color: #00C3E3;"));
        } else {
            m_fps_value->setStyleSheet(QStringLiteral("color: #FF4D6D;"));
        }
    }

    const double ft = results.frametime * 1000.0;
    if (!std::isnan(ft) && ft >= 0.0) {
        m_frametime_value->setText(QStringLiteral("%1 ms").arg(QString::number(ft, 'f', 1)));
    }

    const double speed = results.emulation_speed * 100.0;
    if (!std::isnan(speed) && speed >= 0.0) {
        m_speed_value->setText(QStringLiteral("%1%").arg(QString::number(speed, 'f', 0)));
    }

    const int building = const_cast<VideoCore::ShaderNotify&>(shaders).ShadersBuilding();
    if (building > 0) {
        m_shaders_value->setText(QStringLiteral("Compiling (%1)").arg(building));
        m_shaders_value->setStyleSheet(QStringLiteral("color: #FFB703; font-weight: bold; font-size: 11px;"));
    } else {
        m_shaders_value->setText(QStringLiteral("Ready (0 building)"));
        m_shaders_value->setStyleSheet(QStringLiteral("color: #35E87D; font-weight: bold; font-size: 11px;"));
    }

    UpdateControlsState();
}

void DaliaGameplayOverlay::Reposition() {
    if (!m_main_window) return;
    const QPoint main_pos = m_main_window->pos();
    const int target_x = main_pos.x() + m_main_window->width() - width() - 28;
    const int target_y = main_pos.y() + 68;
    move(std::max(main_pos.x() + 10, target_x), std::max(main_pos.y() + 10, target_y));
}

void DaliaGameplayOverlay::ToggleVisibility() {
    if (isVisible()) {
        hide();
    } else {
        Reposition();
        show();
        raise();
    }
}

void DaliaGameplayOverlay::paintEvent(QPaintEvent* event) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);

    // Translucent dark graphite background
    painter.fillRect(rect(), QColor(18, 20, 26, 235));

    // Outer subtle cyan glow border
    painter.setPen(QPen(QColor(0, 195, 227, 120), 1.5));
    painter.setBrush(Qt::NoBrush);
    painter.drawRoundedRect(rect().adjusted(1, 1, -1, -1), 14, 14);
}

void DaliaGameplayOverlay::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        m_drag_position = event->globalPosition().toPoint() - frameGeometry().topLeft();
        m_is_dragging = true;
        event->accept();
    }
}

void DaliaGameplayOverlay::mouseMoveEvent(QMouseEvent* event) {
    if (m_is_dragging && (event->buttons() & Qt::LeftButton)) {
        move(event->globalPosition().toPoint() - m_drag_position);
        event->accept();
    }
}

void DaliaGameplayOverlay::mouseReleaseEvent(QMouseEvent* event) {
    m_is_dragging = false;
    event->accept();
}

void DaliaGameplayOverlay::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Escape || event->key() == Qt::Key_F10) {
        hide();
        return;
    }
    QWidget::keyPressEvent(event);
}

