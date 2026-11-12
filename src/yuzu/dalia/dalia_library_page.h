// SPDX-FileCopyrightText: Copyright 2026 DALIA Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QWidget>

class MainWindow;
class GameList;
class QLineEdit;
class QPushButton;

class DaliaLibraryPage : public QWidget {
    Q_OBJECT

public:
    explicit DaliaLibraryPage(MainWindow* main_window, GameList* game_list, QWidget* parent = nullptr);
    ~DaliaLibraryPage() override;

    void SyncViewModeButtons();

signals:
    void FocusSidebarRequested();

protected:
    void keyPressEvent(QKeyEvent* event) override;

private:
    void SetupUI();

    MainWindow* m_main_window = nullptr;
    GameList* m_game_list = nullptr;
    QLineEdit* m_search_edit = nullptr;
    QPushButton* m_btn_view_carousel = nullptr;
    QPushButton* m_btn_view_grid = nullptr;
    QPushButton* m_btn_view_list = nullptr;
    QPushButton* m_btn_add_dir = nullptr;
    QPushButton* m_btn_refresh = nullptr;
};
