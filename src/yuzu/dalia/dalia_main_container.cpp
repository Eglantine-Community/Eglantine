// SPDX-FileCopyrightText: Copyright 2026 DALIA Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <QHBoxLayout>
#include <QKeyEvent>
#include <QStackedWidget>

#include "yuzu/dalia/dalia_controllers_page.h"
#include "yuzu/dalia/dalia_home_page.h"
#include "yuzu/dalia/dalia_library_page.h"
#include "yuzu/dalia/dalia_main_container.h"
#include "yuzu/dalia/dalia_settings_page.h"
#include "yuzu/dalia/dalia_sidebar.h"
#include "yuzu/dalia/dalia_system_page.h"
#include "yuzu/dalia/dalia_theme.h"
#include "yuzu/game/game_list.h"
#include "yuzu/main_window.h"

DaliaMainContainer::DaliaMainContainer(MainWindow* main_window, GameList* game_list,
                                       const std::vector<VkDeviceInfo::Record>& vk_records,
                                       QWidget* parent)
    : QWidget{parent}, m_main_window{main_window}, m_game_list{game_list}, m_vk_records{vk_records} {
    setStyleSheet(QStringLiteral("background-color: %1;").arg(DaliaTheme::ColorBackgroundMain));
    SetupUI();
}

DaliaMainContainer::~DaliaMainContainer() = default;

void DaliaMainContainer::SetupUI() {
    auto* main_layout = new QHBoxLayout(this);
    main_layout->setContentsMargins(0, 0, 0, 0);
    main_layout->setSpacing(0);

    // 1. Sidebar on the left
    m_sidebar = new DaliaSidebar(this);
    main_layout->addWidget(m_sidebar);

    // 2. Stacked Content Area on the right
    m_stack = new QStackedWidget(this);
    m_stack->setStyleSheet(QStringLiteral("background-color: %1;").arg(DaliaTheme::ColorBackgroundMain));

    m_home_page = new DaliaHomePage(m_main_window, m_game_list, this);
    m_library_page = new DaliaLibraryPage(m_main_window, m_game_list, this);
    m_controllers_page = new DaliaControllersPage(m_main_window, this);
    m_settings_page = new DaliaSettingsPage(m_main_window, m_vk_records, this);
    m_system_page = new DaliaSystemPage(m_main_window, m_vk_records, this);

    m_stack->addWidget(m_home_page);        // 0: Inicio
    m_stack->addWidget(m_library_page);     // 1: Juegos (Tu biblioteca)
    m_stack->addWidget(m_controllers_page); // 2: Mandos
    m_stack->addWidget(m_settings_page);    // 3: Ajustes
    m_stack->addWidget(m_system_page);      // 4: Sistema

    main_layout->addWidget(m_stack, 1);

    // 3. Connect navigation signals
    connect(m_sidebar, &DaliaSidebar::NavigationRequested, this, &DaliaMainContainer::SetActiveTab);

    connect(m_sidebar, &DaliaSidebar::FocusContentRequested, this, [this] {
        if (m_stack->currentWidget()) {
            m_stack->currentWidget()->setFocus();
        }
    });

    connect(m_sidebar, &DaliaSidebar::UserProfileRequested, this, [this] {
        if (m_main_window) {
            m_main_window->OnConfigure();
        }
    });

    connect(m_sidebar, &DaliaSidebar::MenuRequested, this, [this](const QPoint& global_pos) {
        if (m_main_window) {
            m_main_window->ShowApplicationMenu(global_pos);
        }
    });

    if (m_game_list) {
        connect(m_game_list, &GameList::PopulatingCompleted, this, [this] {
            if (m_home_page) {
                m_home_page->UpdateHeroGame();
            }
            if (m_library_page) {
                m_library_page->SyncViewModeButtons();
            }
        });
    }

    connect(m_home_page, &DaliaHomePage::NavigateToTab, this, &DaliaMainContainer::SetActiveTab);
    connect(m_home_page, &DaliaHomePage::FocusSidebarRequested, this, [this] { m_sidebar->setFocus(); });
    connect(m_library_page, &DaliaLibraryPage::FocusSidebarRequested, this, [this] { m_sidebar->setFocus(); });
    connect(m_controllers_page, &DaliaControllersPage::FocusSidebarRequested, this, [this] { m_sidebar->setFocus(); });
    connect(m_settings_page, &DaliaSettingsPage::FocusSidebarRequested, this, [this] { m_sidebar->setFocus(); });
    connect(m_system_page, &DaliaSystemPage::FocusSidebarRequested, this, [this] { m_sidebar->setFocus(); });

    // Default to 'Juegos' (1)
    SetActiveTab(1);
}

void DaliaMainContainer::SetActiveTab(int index) {
    if (m_stack && index >= 0 && index < m_stack->count()) {
        m_stack->setCurrentIndex(index);
        m_sidebar->SetCurrentIndex(index);

        switch (index) {
        case 0:
            if (m_home_page) m_home_page->UpdateHeroGame();
            break;
        case 1:
            if (m_library_page) m_library_page->SyncViewModeButtons();
            break;
        case 2:
            if (m_controllers_page) m_controllers_page->LoadPlayerSettings(0);
            break;
        case 3:
            if (m_settings_page) m_settings_page->LoadSettings();
            break;
        case 4:
            if (m_system_page) m_system_page->RefreshTelemetry();
            break;
        }
    }
}

int DaliaMainContainer::GetActiveTab() const {
    return m_stack ? m_stack->currentIndex() : 0;
}

void DaliaMainContainer::RefreshVkRecords(const std::vector<VkDeviceInfo::Record>& vk_records) {
    m_vk_records.clear();
    m_vk_records.reserve(vk_records.size());
    for (const auto& rec : vk_records) {
        m_vk_records.emplace_back(rec);
    }
    if (m_settings_page) {
        m_settings_page->RefreshVkRecords(vk_records);
    }
    if (m_system_page) {
        m_system_page->RefreshVkRecords(vk_records);
    }
}

void DaliaMainContainer::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Backtab || event->key() == Qt::Key_Left) {
        m_sidebar->setFocus();
        event->accept();
        return;
    }
    QWidget::keyPressEvent(event);
}
