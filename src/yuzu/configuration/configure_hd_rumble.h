// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QDialog>

#include <string>
#include <vector>

class QCheckBox;
class QComboBox;
class QLabel;
class QListWidget;
class QWidget;
class QSlider;
class QSpinBox;
class QStackedWidget;

/// Settings for RenHDrum: HD Rumble on Switch Pro Controller / Joy-Con, DualSense haptics and
/// Xbox rumble. The rows come from the engine's own settings table (see
/// InputCommon::GetRenHDrumOptions), translated to native Qt widgets and bound to the same
/// Settings::values entries every other input setting uses; the SDL driver re-reads them on
/// every vibration, so connected controllers pick up a change once the dialog is accepted.
class ConfigureHdRumble : public QDialog {
    Q_OBJECT

public:
    explicit ConfigureHdRumble(QWidget* parent);
    ~ConfigureHdRumble() override;

    void ApplyConfiguration();

private:
    /// One row of the options table, bound to an Eglantine setting. The table index is what
    /// other rows' requires_row points at.
    struct OptionRow {
        std::string key;
        int table_index{-1};
        int def{};
        QCheckBox* check{};
        QSlider* slider{};
        QSpinBox* spin{};
        QComboBox* combo{};
        QWidget* container{};
    };

    /// One page per controller family: `device` is the bit RenHDrum's table marks rows with
    /// (1 Switch, 2 DualSense, 4 Xbox).
    QWidget* CreateOptionPage(unsigned device, const QString& note);
    void LoadConfiguration();
    void RestoreDefaults();
    void UpdateEnabledState();

    QCheckBox* hd_enabled{};
    QListWidget* page_list{};
    QStackedWidget* pages{};

    /// Every option row of every page, in build order; the table index carries requires_row.
    std::vector<OptionRow> rows;

    QLabel* xbox_no_triggers{};
    int xbox_triggers_state{-1};
};
