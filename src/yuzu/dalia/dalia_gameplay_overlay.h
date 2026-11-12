// SPDX-FileCopyrightText: Copyright 2026 DALIA Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QWidget>
#include <QPoint>

namespace Core {
struct PerfStatsResults;
}

namespace VideoCore {
class ShaderNotify;
}

class MainWindow;
class QLabel;
class QPushButton;

class DaliaGameplayOverlay : public QWidget {
    Q_OBJECT

public:
    explicit DaliaGameplayOverlay(MainWindow* parent);
    ~DaliaGameplayOverlay() override;

    void UpdateStats(const Core::PerfStatsResults& results, const VideoCore::ShaderNotify& shaders);
    void SetGameTitle(const QString& title, u64 program_id);

public slots:
    void ToggleVisibility();
    void Reposition();

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;

private:
    void SetupUI();
    void UpdateControlsState();

    MainWindow* m_main_window = nullptr;
    QPoint m_drag_position;
    bool m_is_dragging = false;

    QLabel* m_title_label = nullptr;
    QLabel* m_status_indicator = nullptr;
    QLabel* m_fps_value = nullptr;
    QLabel* m_frametime_value = nullptr;
    QLabel* m_speed_value = nullptr;
    QLabel* m_shaders_value = nullptr;
    QLabel* m_gpu_value = nullptr;

    QPushButton* m_btn_pause = nullptr;
    QPushButton* m_btn_screenshot = nullptr;
    QPushButton* m_btn_speed = nullptr;
    QPushButton* m_btn_settings = nullptr;
    QPushButton* m_btn_diagnostics = nullptr;
    QPushButton* m_btn_stop = nullptr;
    QPushButton* m_btn_close = nullptr;
};

