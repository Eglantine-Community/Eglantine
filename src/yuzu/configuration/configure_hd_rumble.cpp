// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <limits>

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QApplication>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QSlider>
#include <QSpinBox>
#include <QStackedWidget>
#include <QToolButton>
#include <QVBoxLayout>

#include "common/settings.h"
#include "input_common/drivers/renhdrum_pad.h"
#include "yuzu/configuration/configure_hd_rumble.h"

namespace {

QLabel* MakeNote(const QString& text) {
    auto* label = new QLabel(text);
    label->setWordWrap(true);
    label->setStyleSheet(QStringLiteral("color: palette(mid);"));
    return label;
}
} // namespace

ConfigureHdRumble::ConfigureHdRumble(QWidget* parent) : QDialog(parent) {
    setWindowTitle(tr("HD Rumble"));
    setMinimumSize(640, 460);

    auto* root = new QVBoxLayout(this);

    // Header: master switch
    auto* header = new QHBoxLayout;
    auto* title = new QLabel(tr("<b>HD Rumble</b>"));
    hd_enabled = new QCheckBox(tr("Enabled"));
    hd_enabled->setToolTip(tr("Off: the controllers use the emulator's standard rumble."));
    header->addWidget(title);
    header->addStretch();
    header->addWidget(hd_enabled);
    root->addLayout(header);
    if (!InputCommon::IsRenHDrumLoaded()) {
        auto* missing = new QLabel(
            tr("renhdrum.dll was not found next to the emulator, so the controllers use the "
               "standard rumble. Get it from <a href=\"https://github.com/trzz1214/RenHDrum/"
               "releases\">the RenHDrum releases</a> and restart the emulator."));
        missing->setWordWrap(true);
        missing->setOpenExternalLinks(true);
        root->addWidget(missing);
    }
    root->addWidget(MakeNote(
        tr("While HD Rumble is on, the \"Enable Accurate Vibration\" filter in the vibration "
           "dialog is bypassed: this engine paces the 5 ms packets itself, and the 15 ms filter "
           "would drop the short ticks Switch 2 games send.")));

    // Intensity and Lift weak rumble are rows of the settings table ("all controllers"), so
    // every device page below carries them; only the per-game note lives here.
    const bool per_game = !Settings::values.hd_rumble_gain.UsingGlobal();
    root->addWidget(MakeNote(
        per_game ? tr("Intensity: this game has its own value, and changes here apply to it.")
                 : tr("Intensity multiplies the game's rumble. Some Switch 2 games (Ocarina of "
                      "Time) send very weak values that the Joy-Con 2 still plays strongly: "
                      "try 400-600% for them.")));

    // Body: controller list on the left, its settings on the right
    auto* body = new QHBoxLayout;
    page_list = new QListWidget;
    page_list->addItem(tr("Switch Controller"));
    page_list->addItem(tr("DualSense"));
    page_list->addItem(tr("Xbox"));
    page_list->setFixedWidth(170);
    pages = new QStackedWidget;
    // Device bits as RenHDrum's options table numbers them: 1 Switch, 2 DualSense, 4 Xbox
    pages->addWidget(CreateOptionPage(
        1, tr("Pro Controller and Joy-Con: the game's HD Rumble sent with full detail.")));
    pages->addWidget(CreateOptionPage(
        2, tr("Detailed rumble on the DualSense and DualSense Edge haptics, wired or on "
              "Bluetooth.")));
    pages->addWidget(CreateOptionPage(
        4, tr("Xbox controllers and Xbox-style controllers from other brands.")));
    connect(page_list, &QListWidget::currentRowChanged, pages, &QStackedWidget::setCurrentIndex);
    // Open on the page of the controller in use, marked as connected
    const int connected = InputCommon::GetRenHDrumConnectedKind();
    if (connected >= 0 && connected < page_list->count()) {
        auto* item = page_list->item(connected);
        item->setText(tr("%1 (connected)").arg(item->text()));
    }
    page_list->setCurrentRow(connected >= 0 ? connected : 0);
    body->addWidget(page_list);
    body->addWidget(pages, 1);
    root->addLayout(body, 1);

    root->addWidget(MakeNote(
        tr("Changes reach connected controllers on their next vibration. To feel them now, open "
           "the Vibration settings and hold a button on the controller.")));

    auto* footer = new QHBoxLayout;
    auto* credit = new QLabel(tr("Powered by RenHDrum"));
    credit->setStyleSheet(QStringLiteral("color: palette(mid);"));
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel |
                                         QDialogButtonBox::RestoreDefaults);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(buttons->button(QDialogButtonBox::RestoreDefaults), &QPushButton::clicked, this,
            &ConfigureHdRumble::RestoreDefaults);
    footer->addWidget(credit);
    footer->addStretch();
    footer->addWidget(buttons);
    root->addLayout(footer);

    connect(hd_enabled, &QCheckBox::toggled, this, &ConfigureHdRumble::UpdateEnabledState);
    // Every row can gate another one (requires_row), so every control refreshes the state
    for (const auto& row : rows) {
        if (row.check != nullptr) {
            connect(row.check, &QCheckBox::toggled, this, &ConfigureHdRumble::UpdateEnabledState);
        } else if (row.combo != nullptr) {
            connect(row.combo, qOverload<int>(&QComboBox::currentIndexChanged), this,
                    &ConfigureHdRumble::UpdateEnabledState);
        } else if (row.spin != nullptr) {
            connect(row.spin, qOverload<int>(&QSpinBox::valueChanged), this,
                    &ConfigureHdRumble::UpdateEnabledState);
        }
    }

    LoadConfiguration();
}

ConfigureHdRumble::~ConfigureHdRumble() = default;

namespace {

const std::vector<InputCommon::RenHDrumOption>& Options() {
    static const std::vector<InputCommon::RenHDrumOption> options =
        InputCommon::GetRenHDrumOptions();
    return options;
}

// Eglantine's storage, keyed the way the settings table names its rows. This is the whole
// binding layer: the dialog never talks to RenHDrum's own store, so every value behaves like
// any other input setting, per-game overrides included where the setting is switchable.
int OptionValue(const std::string& key) {
    auto& s = Settings::values;
    if (key == "switch_bt_mode") {
        return std::clamp(s.hd_rumble_bt_mode.GetValue(), 0, 1);
    }
    if (key == "dualsense_haptics") {
        return s.dualsense_haptics_enabled.GetValue() ? 1 : 0;
    }
    if (key == "dualsense_hd_haptics") {
        return s.dualsense_hd_haptics.GetValue() ? 1 : 0;
    }
    if (key == "dualsense_volume") {
        return std::clamp(s.dualsense_haptics_volume.GetValue(), 10, 100);
    }
    if (key == "xbox_enabled") {
        return s.xbox_rumble_enabled.GetValue() ? 1 : 0;
    }
    if (key == "gain") {
        return std::clamp(s.hd_rumble_gain.GetValue(), 25, 1000);
    }
    if (key == "low_boost") {
        return std::clamp(s.hd_rumble_low_boost.GetValue(), 0, 100);
    }
    if (key == "usb_no_queue") {
        return s.hd_rumble_usb_no_queue.GetValue() ? 1 : 0;
    }
    if (key == "envelope_5ms") {
        return s.hd_rumble_envelope_5ms.GetValue() ? 1 : 0;
    }
    if (key == "frequency_in_packet") {
        return s.hd_rumble_frequency_in_packet.GetValue() ? 1 : 0;
    }
    if (key == "frequency_both_bands") {
        return s.hd_rumble_frequency_both_bands.GetValue() ? 1 : 0;
    }
    if (key == "crisp_notes") {
        return s.hd_rumble_crisp_notes.GetValue() ? 1 : 0;
    }
    if (key == "usb_interval_ms") {
        // The setting keeps milliseconds, the table offers four of them
        static constexpr std::array<int, 4> ms{10, 15, 20, 30};
        const int stored = s.hd_rumble_usb_interval_ms.GetValue();
        int index = 1;
        int best = std::numeric_limits<int>::max();
        for (int i = 0; i < 4; ++i) {
            const int distance = std::abs(stored - ms[i]);
            if (distance < best) {
                best = distance;
                index = i;
            }
        }
        return index;
    }
    if (key == "usb_auto_slow") {
        return s.hd_rumble_usb_auto_slow.GetValue() ? 1 : 0;
    }
    if (key == "bt_after_input") {
        return s.hd_rumble_bt_after_input.GetValue() ? 1 : 0;
    }
    if (key == "bt_crisp_notes") {
        return s.hd_rumble_bt_crisp_notes.GetValue() ? 1 : 0;
    }
    if (key == "dualsense_crisp_notes") {
        return s.dualsense_crisp_notes.GetValue() ? 1 : 0;
    }
    if (key == "xbox_mode") {
        return std::clamp(s.xbox_rumble_mode.GetValue(), 0, 1);
    }
    if (key == "xbox_strength") {
        return std::clamp(s.xbox_motor_strength.GetValue(), 0, 1000);
    }
    if (key == "xbox_detail") {
        return std::clamp(s.xbox_detail_mix.GetValue(), 0, 100);
    }
    if (key == "xbox_accents") {
        return std::clamp(s.xbox_hit_accents.GetValue(), 0, 100);
    }
    if (key == "xbox_trigger_mix") {
        return std::clamp(s.xbox_trigger_mix.GetValue(), 0, 100);
    }
    if (key == "bluetooth_level") {
        return std::clamp(s.hd_rumble_bluetooth_level.GetValue(), 1, 3);
    }
    if (key == "speaker_volume") {
        return std::clamp(s.dualsense_speaker_volume.GetValue(), 0, 100);
    }
    return -1;
}

void SetOptionValue(const std::string& key, int value) {
    auto& s = Settings::values;
    if (key == "switch_bt_mode") {
        s.hd_rumble_bt_mode.SetValue(std::clamp(value, 0, 1));
    } else if (key == "dualsense_haptics") {
        s.dualsense_haptics_enabled.SetValue(value != 0);
    } else if (key == "dualsense_hd_haptics") {
        s.dualsense_hd_haptics.SetValue(value != 0);
    } else if (key == "dualsense_volume") {
        s.dualsense_haptics_volume.SetValue(std::clamp(value, 10, 100));
    } else if (key == "xbox_enabled") {
        s.xbox_rumble_enabled.SetValue(value != 0);
    } else if (key == "gain") {
        s.hd_rumble_gain.SetValue(std::clamp(value, 25, 1000));
    } else if (key == "low_boost") {
        s.hd_rumble_low_boost.SetValue(std::clamp(value, 0, 100));
    } else if (key == "usb_no_queue") {
        s.hd_rumble_usb_no_queue.SetValue(value != 0);
    } else if (key == "envelope_5ms") {
        s.hd_rumble_envelope_5ms.SetValue(value != 0);
    } else if (key == "frequency_in_packet") {
        s.hd_rumble_frequency_in_packet.SetValue(value != 0);
    } else if (key == "frequency_both_bands") {
        s.hd_rumble_frequency_both_bands.SetValue(value != 0);
    } else if (key == "crisp_notes") {
        s.hd_rumble_crisp_notes.SetValue(value != 0);
    } else if (key == "usb_interval_ms") {
        static constexpr std::array<int, 4> ms{10, 15, 20, 30};
        s.hd_rumble_usb_interval_ms.SetValue(
            ms[std::clamp(value, 0, 3)]); // a value outside the choices keeps the closest one
    } else if (key == "usb_auto_slow") {
        s.hd_rumble_usb_auto_slow.SetValue(value != 0);
    } else if (key == "bt_after_input") {
        s.hd_rumble_bt_after_input.SetValue(value != 0);
    } else if (key == "bt_crisp_notes") {
        s.hd_rumble_bt_crisp_notes.SetValue(value != 0);
    } else if (key == "dualsense_crisp_notes") {
        s.dualsense_crisp_notes.SetValue(value != 0);
    } else if (key == "xbox_mode") {
        s.xbox_rumble_mode.SetValue(std::clamp(value, 0, 1));
    } else if (key == "xbox_strength") {
        s.xbox_motor_strength.SetValue(std::clamp(value, 0, 1000));
    } else if (key == "xbox_detail") {
        s.xbox_detail_mix.SetValue(std::clamp(value, 0, 100));
    } else if (key == "xbox_accents") {
        s.xbox_hit_accents.SetValue(std::clamp(value, 0, 100));
    } else if (key == "xbox_trigger_mix") {
        s.xbox_trigger_mix.SetValue(std::clamp(value, 0, 100));
    } else if (key == "bluetooth_level") {
        s.hd_rumble_bluetooth_level.SetValue(std::clamp(value, 1, 3));
    } else if (key == "speaker_volume") {
        s.dualsense_speaker_volume.SetValue(std::clamp(value, 0, 100));
    }
}

} // namespace

QWidget* ConfigureHdRumble::CreateOptionPage(unsigned device, const QString& note) {
    auto* page = new QWidget;
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(MakeNote(note));

    auto* basic = new QFormLayout;
    QFormLayout* advanced = nullptr;
    QWidget* advanced_box = nullptr;
    QToolButton* advanced_button = nullptr;

    const auto& options = Options();
    auto add_row = [&](const InputCommon::RenHDrumOption& opt, int table_index, int def) {
        OptionRow row;
        row.key = opt.key;
        row.table_index = table_index;
        row.def = def;

        QString help = QString::fromStdString(opt.help);
        if (opt.link == 1) {
            help += help.isEmpty() ? QStringLiteral("(wired only)")
                                   : QStringLiteral(" (wired only)");
        } else if (opt.link == 2) {
            help += help.isEmpty() ? QStringLiteral("(Bluetooth only)")
                                   : QStringLiteral(" (Bluetooth only)");
        }

        QFormLayout* form = basic;
        if (opt.tier == 1) {
            if (!advanced) {
                // Collapsible Advanced section, the way the upstream dialog groups its
                // second-tier rows; native Qt widgets only.
                advanced_button = new QToolButton;
                advanced_button->setText(tr("Advanced"));
                advanced_button->setCheckable(true);
                advanced_button->setToolButtonStyle(Qt::ToolButtonTextOnly);
                advanced_button->setArrowType(Qt::DownArrow);
                advanced_box = new QWidget;
                advanced = new QFormLayout(advanced_box);
                advanced->setContentsMargins(0, 0, 0, 0);
                layout->addWidget(advanced_button);
                layout->addWidget(advanced_box);
                advanced_box->setVisible(false);
                connect(advanced_button, &QToolButton::toggled, advanced_box, &QWidget::setVisible);
                connect(advanced_button, &QToolButton::toggled, this, [advanced_button](bool on) {
                    advanced_button->setArrowType(on ? Qt::UpArrow : Qt::DownArrow);
                });
            }
            form = advanced;
        }

        if (opt.kind == 0) {
            row.check = new QCheckBox(QString::fromStdString(opt.label));
            if (!help.isEmpty()) {
                row.check->setToolTip(help);
            }
            form->addRow(row.check);
        } else if (opt.kind == 2) {
            row.combo = new QComboBox;
            for (const std::string& choice : opt.choices) {
                row.combo->addItem(QString::fromStdString(choice));
            }
            if (!help.isEmpty()) {
                row.combo->setToolTip(help);
            }
            form->addRow(QString::fromStdString(opt.label), row.combo);
        } else {
            row.slider = new QSlider(Qt::Horizontal);
            row.slider->setRange(opt.min, opt.max);
            row.slider->setSingleStep(opt.step);
            row.spin = new QSpinBox;
            row.spin->setRange(opt.min, opt.max);
            row.spin->setSingleStep(opt.step);
            if (!help.isEmpty()) {
                row.spin->setToolTip(help);
                row.slider->setToolTip(help);
            }
            connect(row.slider, &QSlider::valueChanged, row.spin, &QSpinBox::setValue);
            connect(row.spin, qOverload<int>(&QSpinBox::valueChanged), row.slider,
                    &QSlider::setValue);
            auto* spin_row = new QHBoxLayout;
            spin_row->addWidget(row.slider, 1);
            spin_row->addWidget(row.spin);
            form->addRow(QString::fromStdString(opt.label), spin_row);
        }
        if (!help.isEmpty() && opt.kind != 0) {
            form->addRow(QString{}, MakeNote(help));
        }
        rows.push_back(row);
    };

    for (int i = 0; i < static_cast<int>(options.size()); ++i) {
        const auto& opt = options[i];
        // The master switch in the header is this table's first row; the pages don't repeat it
        if (opt.key == "enable_hd_rumble") {
            continue;
        }
        if ((opt.devices & device) == 0) {
            continue;
        }
        add_row(opt, i, opt.def);
    }

    // Two settings the engine's table does not carry, both long-standing Eglantine rows
    if (device == 1) {
        InputCommon::RenHDrumOption extra;
        extra.key = "bluetooth_level";
        extra.label = "Bluetooth pacing";
        extra.help = "Lower levels send less often, which keeps input lag down on a busy "
                     "Bluetooth link.";
        extra.choices = {"1 - Stable (60 ms)", "2 - Balanced (45 ms)", "3 - Quality (35 ms)"};
        extra.kind = 2;
        extra.def = 3;
        add_row(extra, -1, 3);
    }
    if (device == 2) {
        InputCommon::RenHDrumOption extra;
        extra.key = "speaker_volume";
        extra.label = "Speaker volume";
        extra.help = "How loud the DualSense speaker taps while HD Haptics is on. 0 = off.";
        extra.kind = 1;
        extra.min = 0;
        extra.max = 100;
        extra.step = 5;
        extra.def = 60;
        add_row(extra, -1, 60);
    }

    layout->addLayout(basic);

    if (device == 4) {
        // Whether the controller in use has trigger motors, known when the dialog opens
        xbox_triggers_state = InputCommon::GetRenHDrumXboxTriggers();
        xbox_no_triggers = new QLabel(tr("This controller has no trigger motors."));
        xbox_no_triggers->setStyleSheet(QStringLiteral("color: palette(mid);"));
        xbox_no_triggers->setVisible(xbox_triggers_state == 0);
        layout->addWidget(xbox_no_triggers);
        // An official Xbox controller whose trigger motors are out of reach: offer the one
        // setting that reaches them, where the user is setting up the vibration
        if (InputCommon::GetRenHDrumXboxNeedsRawInput() &&
            !Settings::values.enable_raw_input.GetValue()) {
            auto* raw_box = new QWidget;
            auto* raw_row = new QHBoxLayout(raw_box);
            raw_row->setContentsMargins(0, 0, 0, 0);
            auto* raw_note = new QLabel(tr("Trigger rumble needs \"XInput 8 player support\"."));
            raw_note->setWordWrap(true);
            auto* raw_button = new QPushButton(tr("Turn it on"));
            raw_row->addWidget(raw_note, 1);
            raw_row->addWidget(raw_button);
            layout->addWidget(raw_box);
            connect(raw_button, &QPushButton::clicked, this, [this, raw_box] {
                Settings::values.enable_raw_input.SetValue(true);
                // Keep the Controls > Advanced checkbox in step, or its OK would undo this
                for (QWidget* top : QApplication::topLevelWidgets()) {
                    if (auto* box = top->findChild<QCheckBox*>(QStringLiteral("enable_raw_input"))) {
                        box->setChecked(true);
                    }
                }
                raw_box->setVisible(false);
                QMessageBox::information(
                    this, tr("HD Rumble"),
                    tr("Turned on. Restart the emulator to feel the triggers.\n\nWhile this is on, "
                       "games can't open their web pages (some manuals and logins). You can turn "
                       "it off in Controls > Advanced."));
            });
        }
        layout->addWidget(MakeNote(tr("Trigger rumble works only with official Xbox "
                                       "controllers. Most wireless receivers drive just the "
                                       "grip motors.")));
    }

    layout->addStretch();
    return page;
}

void ConfigureHdRumble::LoadConfiguration() {
    hd_enabled->setChecked(Settings::values.hd_rumble_enabled.GetValue());
    for (const auto& row : rows) {
        const int value = OptionValue(row.key);
        if (row.check != nullptr) {
            row.check->setChecked(value != 0);
        } else if (row.combo != nullptr) {
            row.combo->setCurrentIndex(std::clamp(value, 0, row.combo->count() - 1));
        } else if (row.spin != nullptr) {
            row.spin->setValue(std::clamp(value, row.spin->minimum(), row.spin->maximum()));
        }
    }
    UpdateEnabledState();
}

void ConfigureHdRumble::RestoreDefaults() {
    hd_enabled->setChecked(true);
    for (const auto& row : rows) {
        if (row.check != nullptr) {
            row.check->setChecked(row.def != 0);
        } else if (row.combo != nullptr) {
            row.combo->setCurrentIndex(std::clamp(row.def, 0, row.combo->count() - 1));
        } else if (row.spin != nullptr) {
            row.spin->setValue(std::clamp(row.def, row.spin->minimum(), row.spin->maximum()));
        }
    }
    UpdateEnabledState();
}

void ConfigureHdRumble::UpdateEnabledState() {
    const bool on = hd_enabled->isChecked();
    pages->setEnabled(on);
    page_list->setEnabled(on);
    for (auto& row : rows) {
        bool parent_on = true;
        if (row.table_index >= 0) {
            // requires_row points at a table index; that row must be non-zero for this one
            const auto& options = Options();
            const int parent_index = options[row.table_index].requires_row;
            if (parent_index >= 0 && parent_index < static_cast<int>(options.size())) {
                const auto& parent_key = options[parent_index].key;
                parent_on = OptionValue(parent_key) != 0;
            }
        }
        const bool triggers_ok = row.key != "xbox_trigger_mix" || xbox_triggers_state != 0;
        const bool enabled = on && parent_on && triggers_ok;
        if (row.check != nullptr) {
            row.check->setEnabled(enabled);
        }
        if (row.combo != nullptr) {
            row.combo->setEnabled(enabled);
        }
        if (row.slider != nullptr && row.spin != nullptr) {
            row.slider->setEnabled(enabled);
            row.spin->setEnabled(enabled);
        }
    }
}

void ConfigureHdRumble::ApplyConfiguration() {
    auto& s = Settings::values;
    s.hd_rumble_enabled.SetValue(hd_enabled->isChecked());
    for (const auto& row : rows) {
        if (row.check != nullptr) {
            SetOptionValue(row.key, row.check->isChecked() ? 1 : 0);
        } else if (row.combo != nullptr) {
            SetOptionValue(row.key, row.combo->currentIndex());
        } else if (row.spin != nullptr) {
            SetOptionValue(row.key, row.spin->value());
        }
    }
}
