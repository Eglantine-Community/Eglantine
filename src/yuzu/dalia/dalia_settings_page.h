// SPDX-FileCopyrightText: Copyright 2026 DALIA Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <vector>
#include <QWidget>
#include "qt_common/util/vk.h"

class MainWindow;
class QComboBox;
class QCheckBox;
class QSlider;
class QLabel;
class QPushButton;
class QStackedWidget;

class DaliaSettingsPage : public QWidget {
    Q_OBJECT

public:
    explicit DaliaSettingsPage(MainWindow* main_window, const std::vector<VkDeviceInfo::Record>& vk_records, QWidget* parent = nullptr);
    ~DaliaSettingsPage() override;

    void LoadSettings();
    void RefreshVkRecords(const std::vector<VkDeviceInfo::Record>& vk_records);

signals:
    void FocusSidebarRequested();

protected:
    void keyPressEvent(QKeyEvent* event) override;

private:
    void SetupUI();
    QWidget* CreateBasicView();
    QWidget* CreateAdvancedView();

    void ApplyProfile(int profile_index);
    void SaveSettings();
    void RestoreDefaults();
    void ClearShaderCache();

    MainWindow* m_main_window = nullptr;
    std::vector<VkDeviceInfo::Record> m_vk_records;

    QPushButton* m_btn_mode_basic = nullptr;
    QPushButton* m_btn_mode_advanced = nullptr;
    QStackedWidget* m_mode_stack = nullptr;

    // Basic controls
    QComboBox* m_combo_preset = nullptr;
    QComboBox* m_combo_resolution = nullptr;
    QComboBox* m_combo_aspect = nullptr;
    QComboBox* m_combo_filter = nullptr;
    QSlider* m_slider_fsr_sharpness = nullptr;
    QLabel* m_label_fsr_sharpness = nullptr;
    QComboBox* m_combo_aa = nullptr;
    QCheckBox* m_check_docked = nullptr;
    QSlider* m_slider_volume = nullptr;
    QLabel* m_label_volume = nullptr;

    // Advanced controls
    QComboBox* m_combo_vk_device = nullptr;
    QComboBox* m_combo_astc_recompression = nullptr;
    QComboBox* m_combo_vram_mode = nullptr;
    QComboBox* m_combo_extended_dynamic_state = nullptr;
    QCheckBox* m_check_async_gpu = nullptr;
    QCheckBox* m_check_disk_shaders = nullptr;
    QCheckBox* m_check_driver_cache = nullptr;
    QCheckBox* m_check_reactive_flushing = nullptr;
    QCheckBox* m_check_gpl = nullptr;
    QCheckBox* m_check_precompile_shaders = nullptr;
    QComboBox* m_combo_cpu_accuracy = nullptr;
    QCheckBox* m_check_multi_core = nullptr;
    QComboBox* m_combo_memory_layout = nullptr;
    QCheckBox* m_check_speed_limit = nullptr;

    QPushButton* m_btn_clear_shaders = nullptr;
    QPushButton* m_btn_restore = nullptr;
    QPushButton* m_btn_apply = nullptr;
    QLabel* m_status_feedback = nullptr;
};
