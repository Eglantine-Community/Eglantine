// SPDX-FileCopyrightText: Copyright 2026 DALIA Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QDialog>
#include <QPixmap>
#include <QString>
#include "common/common_types.h"

class MainWindow;
class QLabel;
class QPushButton;

class DaliaGameDetailsDialog : public QDialog {
    Q_OBJECT

public:
    explicit DaliaGameDetailsDialog(MainWindow* parent, const QString& title, u64 program_id,
                                   const QString& file_path, const QPixmap& icon,
                                   const QString& developer, const QString& version,
                                   const QString& play_time, const QString& file_size);
    ~DaliaGameDetailsDialog() override;

signals:
    void LaunchRequested(const QString& path);

protected:
    void keyPressEvent(QKeyEvent* event) override;
    void paintEvent(QPaintEvent* event) override;

private:
    void SetupUI();
    void OnPlayClicked();
    void OnConfigureClicked();
    void OnSaveFolderClicked();
    void OnModFolderClicked();
    void OnClearShadersClicked();

    MainWindow* m_main_window = nullptr;
    QString m_title;
    u64 m_program_id = 0;
    QString m_file_path;
    QPixmap m_icon;
    QString m_developer;
    QString m_version;
    QString m_play_time;
    QString m_file_size;

    QPushButton* m_btn_play = nullptr;
    QPushButton* m_btn_configure = nullptr;
    QPushButton* m_btn_save = nullptr;
    QPushButton* m_btn_mods = nullptr;
    QPushButton* m_btn_clear_shaders = nullptr;
    QPushButton* m_btn_close = nullptr;
};

