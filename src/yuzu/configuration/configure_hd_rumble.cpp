// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <limits>

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFont>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QSlider>
#include <QSpinBox>
#include <QToolButton>
#include <QVBoxLayout>

#include "common/settings.h"
#include "input_common/drivers/renhdrum_pad.h"
#include "yuzu/configuration/configure_hd_rumble.h"

namespace {

QLabel* MakeNote(const QString& text) {
    auto* label = new QLabel(text);
    label->setWordWrap(true);
    // Muted but legible on every theme: window-text blended 75/25 toward the background
    const QColor text_c = label->palette().color(QPalette::WindowText);
    const QColor back_c = label->palette().color(QPalette::Window);
    const QColor muted((text_c.red() * 3 + back_c.red()) / 4,
                       (text_c.green() * 3 + back_c.green()) / 4,
                       (text_c.blue() * 3 + back_c.blue()) / 4);
    label->setStyleSheet(QStringLiteral("color: %1;").arg(muted.name(QColor::HexArgb)));
    return label;
}

QFrame* MakeDivider() {
    auto* divider = new QFrame;
    divider->setFrameShape(QFrame::HLine);
    divider->setFrameShadow(QFrame::Sunken);
    return divider;
}

// A disclosure row like the upstream panel's collapsibles: plain text, arrow beside it
QWidget* MakeCollapsible(const QString& title, QWidget* content, bool open) {
    auto* holder = new QWidget;
    auto* col = new QVBoxLayout(holder);
    col->setContentsMargins(0, 0, 0, 0);
    col->setSpacing(6);
    auto* head = new QToolButton;
    head->setText(title);
    head->setCheckable(true);
    head->setChecked(open);
    head->setAutoRaise(true);
    head->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    head->setArrowType(open ? Qt::DownArrow : Qt::RightArrow);
    head->setCursor(Qt::PointingHandCursor);
    content->setVisible(open);
    QObject::connect(head, &QToolButton::toggled, head, [head, content](bool on) {
        head->setArrowType(on ? Qt::DownArrow : Qt::RightArrow);
        content->setVisible(on);
    });
    col->addWidget(head);
    col->addWidget(content);
    return holder;
}

QString DeviceTitle(unsigned device) {
    switch (device) {
    case 1u:
        return QObject::tr("Switch Controller");
    case 2u:
        return QObject::tr("DualSense Wireless Controller");
    default:
        return QObject::tr("Xbox Controller");
    }
}

} // namespace

ConfigureHdRumble::ConfigureHdRumble(QWidget* parent) : QDialog(parent) {
    setWindowTitle(tr("HD Rumble"));
    setMinimumSize(560, 480);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(16, 12, 16, 12);
    root->setSpacing(10);

    // Header: the panel title with the master switch right-aligned, like the upstream panel
    auto* header = new QHBoxLayout;
    header->setSpacing(10);
    auto* title = new QLabel(tr("HD Rumble"));
    QFont title_font = title->font();
    title_font.setPointSizeF(title_font.pointSizeF() * 1.2);
    title_font.setWeight(QFont::DemiBold);
    title->setFont(title_font);
    hd_enabled = new QCheckBox;
    hd_enabled->setChecked(true);
    hd_enabled->setAccessibleName(tr("HD Rumble"));
    hd_enabled->setToolTip(tr("Off: the controllers use the emulator's standard rumble."));
    header->addWidget(title);
    header->addStretch(1);
    header->addWidget(hd_enabled, 0, Qt::AlignVCenter);
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
    root->addWidget(MakeDivider());

    // Body: the controller in use first, the others and the advanced rows behind disclosures
    const int connected = InputCommon::GetRenHDrumConnectedKind();
    // 0 Switch, 1 DualSense, 2 Xbox; none connected means no active card at all
    const unsigned active = (connected >= 0 && connected <= 2) ? (1u << connected) : 0u;
    body = new QWidget;
    auto* body_col = new QVBoxLayout(body);
    body_col->setContentsMargins(0, 0, 0, 0);
    body_col->setSpacing(10);
    body_col->addWidget(MakeNote(
        tr("While HD Rumble is on, the \"Enable Accurate Vibration\" filter in the vibration "
           "dialog is bypassed: this engine paces the 5 ms packets itself, and the 15 ms filter "
           "would drop the short ticks Switch 2 games send.")));
    const bool per_game = !Settings::values.hd_rumble_gain.UsingGlobal();
    body_col->addWidget(MakeNote(
        per_game ? tr("Intensity: this game has its own value, and changes here apply to it.")
                 : tr("Intensity multiplies the game's rumble.")));
    body_col->addWidget(MakeNote(
        tr("Eglantine bundles RenHDrum on its own. You can supply your own build: point the "
           "RENHDRUM_LIBRARY environment variable at it, or on Windows replace renhdrum.dll "
           "next to the emulator. Use a build from the same release, or the settings may not "
           "line up.")));
    if (active != 0u) {
        body_col->addWidget(CreateDeviceCard(active, true));
    } else {
        body_col->addWidget(MakeNote(tr("No compatible controller detected: these settings "
                                        "apply once one connects.")));
    }

    // Every other controller family stays one disclosure away, one card each
    auto* others_host = new QWidget;
    auto* others = new QVBoxLayout(others_host);
    others->setContentsMargins(0, 0, 0, 0);
    others->setSpacing(10);
    for (unsigned bit : {1u, 2u, 4u}) {
        if (bit != active) {
            others->addWidget(CreateDeviceCard(bit, false));
        }
    }
    body_col->addWidget(MakeCollapsible(tr("Other controllers"), others_host, false));
    body_col->addWidget(MakeCollapsible(tr("Advanced"), CreateAdvancedCard(), false));
    body_col->addWidget(MakeNote(
        tr("Changes reach connected controllers on their next vibration. To feel them now, "
           "open the Vibration settings and hold a button on the controller.")));
    body_col->addStretch(1);

    auto* scroll = new QScrollArea;
    scroll->setWidget(body);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    root->addWidget(scroll, 1);

    // Footer: credit left, dialog buttons right
    root->addWidget(MakeDivider());
    auto* footer = new QHBoxLayout;
    auto* credit =
        new QLabel(tr("<a href=\"https://github.com/trzz1214/RenHDrum\">Powered by RenHDrum</a>"));
    credit->setOpenExternalLinks(true);
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

void ConfigureHdRumble::AddRow(QVBoxLayout* col, bool& first,
                               const InputCommon::RenHDrumOption& opt, int table_index,
                               int def) {
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

    if (!first) {
        col->addWidget(MakeDivider());
    }
    first = false;

    auto* row_box = new QWidget;
    auto* row_layout = new QHBoxLayout(row_box);
    row_layout->setContentsMargins(0, 6, 0, 6);
    row_layout->setSpacing(12);
    auto* left = new QVBoxLayout;
    left->setSpacing(3);
    auto* title_label = new QLabel(QString::fromStdString(opt.label));
    title_label->setWordWrap(true);
    left->addWidget(title_label);
    if (!help.isEmpty()) {
        left->addWidget(MakeNote(help));
    }
    row_layout->addLayout(left, 1);

    if (opt.kind == 0) {
        // The label lives on the left stack; the checkbox is the control on the right
        row.check = new QCheckBox;
        row.check->setChecked(def != 0);
        row.check->setAccessibleName(QString::fromStdString(opt.label));
        if (!help.isEmpty()) {
            row.check->setToolTip(help);
        }
        row_layout->addWidget(row.check, 0, Qt::AlignVCenter);
    } else if (opt.kind == 2) {
        row.combo = new QComboBox;
        for (const std::string& choice : opt.choices) {
            row.combo->addItem(QString::fromStdString(choice));
        }
        row.combo->setCurrentIndex(std::clamp(def, 0, row.combo->count() - 1));
        if (!help.isEmpty()) {
            row.combo->setToolTip(help);
        }
        row_layout->addWidget(row.combo, 0, Qt::AlignVCenter);
    } else {
        auto* controls = new QHBoxLayout;
        controls->setContentsMargins(0, 0, 0, 0);
        controls->setSpacing(8);
        row.slider = new QSlider(Qt::Horizontal);
        row.slider->setMinimumWidth(120);
        row.slider->setRange(opt.min, opt.max);
        row.slider->setSingleStep(std::max(1, opt.step));
        row.slider->setPageStep(std::max(1, opt.step) * 4);
        row.slider->setValue(std::clamp(def, opt.min, opt.max));
        row.spin = new QSpinBox;
        row.spin->setRange(opt.min, opt.max);
        row.spin->setSingleStep(std::max(1, opt.step));
        row.spin->setSuffix(QStringLiteral(" %"));
        row.spin->setValue(std::clamp(def, opt.min, opt.max));
        if (!help.isEmpty()) {
            row.spin->setToolTip(help);
            row.slider->setToolTip(help);
        }
        connect(row.slider, &QSlider::valueChanged, row.spin, &QSpinBox::setValue);
        connect(row.spin, qOverload<int>(&QSpinBox::valueChanged), row.slider,
                &QSlider::setValue);
        controls->addWidget(row.slider);
        controls->addWidget(row.spin);
        auto* controls_host = new QWidget;
        controls_host->setLayout(controls);
        row_layout->addWidget(controls_host, 0, Qt::AlignVCenter);
    }
    row.container = row_box;
    col->addWidget(row_box);
    rows.push_back(row);
}

QWidget* ConfigureHdRumble::CreateDeviceCard(unsigned device, bool connected) {
    // A card like the upstream panel's: bold title and status pill on a header row, a
    // divider, then the tier-0 rows of this device
    auto* card = new QFrame;
    card->setFrameShape(QFrame::StyledPanel);
    auto* col = new QVBoxLayout(card);
    col->setContentsMargins(14, 10, 14, 12);
    col->setSpacing(6);
    auto* header = new QHBoxLayout;
    header->setSpacing(10);
    auto* title = new QLabel(DeviceTitle(device));
    QFont bold = title->font();
    bold.setWeight(QFont::DemiBold);
    title->setFont(bold);
    header->addWidget(title);
    header->addStretch(1);
    if (connected) {
        auto* pill = new QLabel(tr("connected"));
        pill->setStyleSheet(
            QStringLiteral("border: 1px solid palette(midlight); border-radius: 9px; "
                            "padding: 1px 8px;"));
        header->addWidget(pill, 0, Qt::AlignVCenter);
    }
    col->addLayout(header);
    col->addWidget(MakeDivider());

    bool first = false; // the header divider already separates the title from the rows
    const auto& options = Options();
    for (int i = 0; i < static_cast<int>(options.size()); ++i) {
        const auto& opt = options[i];
        // The master switch in the header is this table's first row; the cards don't repeat it
        if (opt.key == "enable_hd_rumble") {
            continue;
        }
        if ((opt.devices & device) == 0 || opt.tier != 0) {
            continue;
        }
        AddRow(col, first, opt, i, opt.def);
    }

    // A setting the engine's table does not carry, a long-standing Eglantine row
    if (device == 2u) {
        InputCommon::RenHDrumOption extra{};
        extra.key = "speaker_volume";
        extra.label = "Speaker volume";
        extra.help = "How loud the DualSense speaker taps while HD Haptics is on. 0 = off.";
        extra.kind = 1;
        extra.min = 0;
        extra.max = 100;
        extra.step = 5;
        AddRow(col, first, extra, -1, 60);
    }

    if (device == 4u) {
        // Whether the controller in use has trigger motors, known when the dialog opens
        xbox_triggers_state = InputCommon::GetRenHDrumXboxTriggers();
        xbox_no_triggers = MakeNote(tr("This controller has no trigger motors."));
        xbox_no_triggers->setVisible(xbox_triggers_state == 0);
        col->addWidget(xbox_no_triggers);
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
            col->addWidget(raw_box);
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
                    tr("Turned on. Restart the emulator to feel the triggers.\n\nWhile this is "
                       "on, games can't open their web pages (some manuals and logins). You can "
                       "turn it off in Controls > Advanced."));
            });
        }
        col->addWidget(MakeNote(tr("Trigger rumble works only with official Xbox "
                                   "controllers. Most wireless receivers drive just the "
                                   "grip motors.")));
    }
    return card;
}

QWidget* ConfigureHdRumble::CreateAdvancedCard() {
    // The tier-1 rows of every family behind one disclosure, each group under its device
    // title: a Gamesir in Direct mode is configured the same way with or without it plugged in
    auto* card = new QFrame;
    card->setFrameShape(QFrame::StyledPanel);
    auto* col = new QVBoxLayout(card);
    col->setContentsMargins(14, 10, 14, 12);
    col->setSpacing(6);
    const auto& options = Options();
    bool first_group = true;
    for (unsigned bit : {1u, 2u, 4u}) {
        if (!first_group) {
            col->addWidget(MakeDivider());
        }
        first_group = false;
        auto* group_title = new QLabel(DeviceTitle(bit));
        QFont bold = group_title->font();
        bold.setWeight(QFont::DemiBold);
        group_title->setFont(bold);
        col->addWidget(group_title);
        bool first = true;
        for (int i = 0; i < static_cast<int>(options.size()); ++i) {
            const auto& opt = options[i];
            if (opt.key == "enable_hd_rumble" || opt.tier != 1 || (opt.devices & bit) == 0) {
                continue;
            }
            AddRow(col, first, opt, i, opt.def);
        }
        // A setting the engine's table does not carry, a long-standing Eglantine row
        if (bit == 1u) {
            InputCommon::RenHDrumOption extra{};
            extra.key = "bluetooth_level";
            extra.label = "Bluetooth pacing";
            extra.help = "Lower levels send less often, which keeps input lag down on a busy "
                         "Bluetooth link. (Bluetooth only)";
            extra.choices = {"1 - Stable (60 ms)", "2 - Balanced (45 ms)", "3 - Quality (35 ms)"};
            extra.kind = 2;
            AddRow(col, first, extra, -1, 3);
        }
    }
    return card;
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
    body->setEnabled(on);
    // The gate reads the row widgets, not the settings store: the store only catches up on OK,
    // and the rows below a mode switch must react while the dialog is open
    auto row_value = [this](const std::string& key) -> int {
        if (key == "enable_hd_rumble") {
            return hd_enabled->isChecked() ? 1 : 0;
        }
        for (const auto& row : rows) {
            if (row.key != key) {
                continue;
            }
            if (row.check != nullptr) {
                return row.check->isChecked() ? 1 : 0;
            }
            if (row.combo != nullptr) {
                return row.combo->currentIndex();
            }
            if (row.spin != nullptr) {
                return row.spin->value();
            }
        }
        return OptionValue(key); // no widget for it: the stored value stands in
    };
    for (auto& row : rows) {
        bool parent_on = true;
        if (row.table_index >= 0) {
            // requires_row points at a table index; that row must be non-zero for this one
            const auto& options = Options();
            const int parent_index = options[row.table_index].requires_row;
            if (parent_index >= 0 && parent_index < static_cast<int>(options.size())) {
                const std::string& parent_key = options[parent_index].key;
                parent_on = row_value(parent_key) != 0;
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
