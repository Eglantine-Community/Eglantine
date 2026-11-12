// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QWidget>
#include <QPushButton>
#include <QLabel>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QTimer>

class MainWindow;
class GameList;

enum class SwitchButtonType {
    News,
    Shop,
    Album,
    Controllers,
    Settings,
    Diagnostics,
    Power,
};

class SwitchRoundButton : public QPushButton {
    Q_OBJECT
public:
    explicit SwitchRoundButton(SwitchButtonType type, QWidget* parent = nullptr);
    SwitchButtonType GetType() const { return m_type; }

protected:
    void paintEvent(QPaintEvent* event) override;
    QSize sizeHint() const override { return QSize(54, 54); }

private:
    SwitchButtonType m_type;
};

class SwitchTopBar : public QWidget {
    Q_OBJECT
public:
    explicit SwitchTopBar(QWidget* parent = nullptr);

public slots:
    void UpdateClock();

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    QLabel* m_time_label = nullptr;
    QTimer* m_clock_timer = nullptr;
};

class SwitchTitleBar : public QWidget {
    Q_OBJECT
public:
    explicit SwitchTitleBar(QWidget* parent = nullptr);
    void SetTitle(const QString& title);

private:
    QLabel* m_title_label = nullptr;
};

class SwitchActionBar : public QWidget {
    Q_OBJECT
public:
    explicit SwitchActionBar(MainWindow* main_window, GameList* game_list, QWidget* parent = nullptr);

private:
    MainWindow* m_main_window = nullptr;
    GameList* m_game_list = nullptr;
};

class SwitchFooterBar : public QWidget {
    Q_OBJECT
public:
    explicit SwitchFooterBar(MainWindow* main_window, GameList* game_list, QWidget* parent = nullptr);

signals:
    void StartRequested();
    void OptionsRequested();

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    MainWindow* m_main_window = nullptr;
    GameList* m_game_list = nullptr;
    QPushButton* m_start_button = nullptr;
    QPushButton* m_options_button = nullptr;
};

