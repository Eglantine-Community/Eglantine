// SPDX-FileCopyrightText: Copyright 2026 DALIA Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <vector>
#include <QDialog>
#include "qt_common/util/vk.h"

class MainWindow;
class QTextEdit;
class QLineEdit;
class QLabel;
class QPushButton;
class QTabWidget;

class DaliaDiagnosticsDialog : public QDialog {
    Q_OBJECT

public:
    explicit DaliaDiagnosticsDialog(MainWindow* parent, const std::vector<VkDeviceInfo::Record>& vk_records);
    ~DaliaDiagnosticsDialog() override;

protected:
    void paintEvent(QPaintEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;

private:
    void SetupUI();
    QWidget* CreateHardwareOverviewTab();
    QWidget* CreateVulkanHealthTab();
    QWidget* CreateLogViewerTab();

    void RefreshLogs();
    void FilterLogs(const QString& filter_text);
    void ExportDiagnosticReport();
    void CopySummaryToClipboard();

    MainWindow* m_main_window = nullptr;
    std::vector<VkDeviceInfo::Record> m_vk_records;

    QTabWidget* m_tab_widget = nullptr;
    QTextEdit* m_log_edit = nullptr;
    QLineEdit* m_filter_edit = nullptr;
    QString m_raw_log_content;

    QPushButton* m_btn_export = nullptr;
    QPushButton* m_btn_copy = nullptr;
    QPushButton* m_btn_close = nullptr;
};

