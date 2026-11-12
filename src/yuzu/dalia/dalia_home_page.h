// SPDX-FileCopyrightText: Copyright 2026 DALIA Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <vector>
#include <QWidget>

class MainWindow;
class GameList;
class QLabel;
class QPushButton;
class QHBoxLayout;

class DaliaHomePage : public QWidget {
    Q_OBJECT

public:
    explicit DaliaHomePage(MainWindow* main_window, GameList* game_list, QWidget* parent = nullptr);
    ~DaliaHomePage() override;

    void UpdateHeroGame();
    void RefreshRecentGames();

signals:
    void NavigateToTab(int tab_index);
    void FocusSidebarRequested();

protected:
    void keyPressEvent(QKeyEvent* event) override;

private:
    void SetupUI();

    MainWindow* m_main_window = nullptr;
    GameList* m_game_list = nullptr;

    QLabel* m_hero_icon = nullptr;
    QLabel* m_hero_title = nullptr;
    QLabel* m_hero_info = nullptr;
    QPushButton* m_btn_hero_play = nullptr;
    QPushButton* m_btn_hero_details = nullptr;
    QString m_hero_file_path;

    QWidget* m_recent_container = nullptr;
    QHBoxLayout* m_recent_layout = nullptr;

    QLabel* m_status_label = nullptr;
};
