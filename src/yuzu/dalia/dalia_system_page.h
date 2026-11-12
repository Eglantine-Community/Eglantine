// SPDX-FileCopyrightText: Copyright 2026 DALIA Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <vector>
#include <QWidget>
#include "qt_common/util/vk.h"

class MainWindow;
class QTextEdit;
class QLineEdit;
class QTabWidget;
class QLabel;

class DaliaSystemPage : public QWidget {
    Q_OBJECT

public:
    explicit DaliaSystemPage(MainWindow* main_window, const std::vector<VkDeviceInfo::Record>& vk_records, QWidget* parent = nullptr);
    ~DaliaSystemPage() override;

    void RefreshTelemetry();
    void RefreshVkRecords(const std::vector<VkDeviceInfo::Record>& vk_records);

signals:
    void FocusSidebarRequested();

protected:
    void keyPressEvent(QKeyEvent* event) override;

private:
    void SetupUI();
    QWidget* CreateHardwareOverviewTab();
    QWidget* CreateLogViewerTab();

    void RefreshLogs();
    void FilterLogs(const QString& filter_text);
    void ExportDiagnosticReport();
    void CopySummaryToClipboard();

    MainWindow* m_main_window = nullptr;
    std::vector<VkDeviceInfo::Record> m_vk_records;

    QTabWidget* m_tab_widget = nullptr;
    QLabel* m_val_cpu = nullptr;
    QLabel* m_val_ram = nullptr;
    QLabel* m_val_gpu = nullptr;
    QLabel* m_val_vram_mode = nullptr;
    QLabel* m_val_astc = nullptr;
    QLabel* m_val_mem_layout = nullptr;
    QLabel* m_val_shader_cache = nullptr;

    QTextEdit* m_log_edit = nullptr;
    QLineEdit* m_filter_edit = nullptr;
    QString m_raw_log_content;
};
