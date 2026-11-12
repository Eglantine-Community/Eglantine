// SPDX-FileCopyrightText: Copyright 2026 DALIA Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <vector>
#include <QWidget>

class MainWindow;
class QComboBox;
class QCheckBox;
class QPushButton;
class QLabel;

class DaliaControllersPage : public QWidget {
    Q_OBJECT

public:
    explicit DaliaControllersPage(MainWindow* main_window, QWidget* parent = nullptr);
    ~DaliaControllersPage() override;

    void LoadPlayerSettings(int player_index);
    void SavePlayerSettings();

signals:
    void FocusSidebarRequested();

protected:
    void keyPressEvent(QKeyEvent* event) override;

private:
    void SetupUI();
    void RefreshControllerDetection();

    MainWindow* m_main_window = nullptr;
    int m_selected_player = 0;

    std::vector<QPushButton*> m_player_buttons;
    QLabel* m_status_badge = nullptr;
    QLabel* m_device_name_label = nullptr;

    QCheckBox* m_check_connected = nullptr;
    QComboBox* m_combo_controller_type = nullptr;
    QCheckBox* m_check_vibration = nullptr;
    QCheckBox* m_check_motion = nullptr;

    QPushButton* m_btn_open_mapping = nullptr;
    QPushButton* m_btn_refresh = nullptr;
    QLabel* m_feedback_label = nullptr;
};
