// SPDX-FileCopyrightText: Copyright 2026 DALIA Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <vector>
#include <QFrame>
#include <QPoint>

class QPushButton;

class DaliaSidebar : public QFrame {
    Q_OBJECT

public:
    explicit DaliaSidebar(QWidget* parent = nullptr);
    ~DaliaSidebar() override;

    void SetCurrentIndex(int index);
    [[nodiscard]] int GetCurrentIndex() const { return m_current_index; }

signals:
    void NavigationRequested(int index);
    void FocusContentRequested();
    void UserProfileRequested();
    void MenuRequested(const QPoint& global_pos);

protected:
    void keyPressEvent(QKeyEvent* event) override;

private:
    void SetupUI();
    void UpdateSelectionStyles();

    int m_current_index = 1; // Default to 'Juegos'
    std::vector<QPushButton*> m_nav_buttons;
    std::vector<QString> m_nav_kinds;
};
