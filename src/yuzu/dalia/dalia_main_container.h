// SPDX-FileCopyrightText: Copyright 2026 DALIA Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <vector>
#include <QWidget>
#include "qt_common/util/vk.h"

class MainWindow;
class GameList;
class DaliaSidebar;
class DaliaHomePage;
class DaliaLibraryPage;
class DaliaControllersPage;
class DaliaSettingsPage;
class DaliaSystemPage;
class QStackedWidget;

class DaliaMainContainer : public QWidget {
    Q_OBJECT

public:
    explicit DaliaMainContainer(MainWindow* main_window, GameList* game_list,
                                const std::vector<VkDeviceInfo::Record>& vk_records,
                                QWidget* parent = nullptr);
    ~DaliaMainContainer() override;

    void SetActiveTab(int index);
    [[nodiscard]] int GetActiveTab() const;
    void RefreshVkRecords(const std::vector<VkDeviceInfo::Record>& vk_records);

    DaliaLibraryPage* GetLibraryPage() const { return m_library_page; }
    DaliaHomePage* GetHomePage() const { return m_home_page; }

protected:
    void keyPressEvent(QKeyEvent* event) override;

private:
    void SetupUI();

    MainWindow* m_main_window = nullptr;
    GameList* m_game_list = nullptr;
    std::vector<VkDeviceInfo::Record> m_vk_records;

    DaliaSidebar* m_sidebar = nullptr;
    QStackedWidget* m_stack = nullptr;

    DaliaHomePage* m_home_page = nullptr;
    DaliaLibraryPage* m_library_page = nullptr;
    DaliaControllersPage* m_controllers_page = nullptr;
    DaliaSettingsPage* m_settings_page = nullptr;
    DaliaSystemPage* m_system_page = nullptr;
};

