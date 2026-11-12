// SPDX-FileCopyrightText: Copyright 2026 DALIA Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <QCheckBox>
#include <QComboBox>
#include <QFrame>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMessageBox>
#include <QPainter>
#include <QPushButton>
#include <QSlider>
#include <QStackedWidget>
#include <QVBoxLayout>

#include "common/fs/fs.h"
#include "common/fs/fs_paths.h"
#include "common/fs/fs_util.h"
#include "common/fs/path_util.h"
#include "common/settings.h"
#include "qt_common/config/uisettings.h"
#include "qt_common/qt_common.h"
#include "yuzu/dalia/dalia_settings.h"
#include "yuzu/main_window.h"

DaliaSettingsDialog::DaliaSettingsDialog(MainWindow* parent, const std::vector<VkDeviceInfo::Record>& vk_records)
    : QDialog{parent}, m_main_window{parent}, m_vk_records{vk_records} {
    setWindowTitle(QStringLiteral("DALIA — System Settings"));
    setFixedSize(860, 620);
    setModal(true);
    setWindowFlags(windowFlags() & ~Qt::WindowContextHelpButtonHint);

    SetupUI();
    LoadSettings();
}

DaliaSettingsDialog::~DaliaSettingsDialog() = default;

void DaliaSettingsDialog::SetupUI() {
    auto* main_layout = new QVBoxLayout(this);
    main_layout->setContentsMargins(28, 24, 28, 20);
    main_layout->setSpacing(16);

    // Top Header: Title, Subtitle, and Mode Switcher
    auto* header_layout = new QHBoxLayout();
    header_layout->setSpacing(16);

    auto* title_box = new QVBoxLayout();
    title_box->setSpacing(2);
    auto* title_label = new QLabel(QStringLiteral("SYSTEM SETTINGS"), this);
    QFont title_font = title_label->font();
    title_font.setPointSize(15);
    title_font.setBold(true);
    title_label->setFont(title_font);
    title_label->setStyleSheet(QStringLiteral("color: #FFFFFF; letter-spacing: 1px;"));

    auto* subtitle_label = new QLabel(QStringLiteral("Nintendo Switch 2 & High Performance Vulkan Engine"), this);
    subtitle_label->setStyleSheet(QStringLiteral("color: #8E9297; font-size: 11px;"));
    title_box->addWidget(title_label);
    title_box->addWidget(subtitle_label);
    header_layout->addLayout(title_box);
    header_layout->addStretch();

    // Mode Toggle Pill
    auto* toggle_container = new QFrame(this);
    toggle_container->setStyleSheet(QStringLiteral(
        "QFrame { background-color: #1A1D24; border-radius: 16px; padding: 2px; }"));
    auto* toggle_layout = new QHBoxLayout(toggle_container);
    toggle_layout->setContentsMargins(2, 2, 2, 2);
    toggle_layout->setSpacing(4);

    m_btn_mode_basic = new QPushButton(QStringLiteral("Basic Mode"), this);
    m_btn_mode_basic->setCheckable(true);
    m_btn_mode_basic->setChecked(true);
    m_btn_mode_basic->setCursor(Qt::PointingHandCursor);
    m_btn_mode_basic->setFixedHeight(30);

    m_btn_mode_advanced = new QPushButton(QStringLiteral("Advanced Mode"), this);
    m_btn_mode_advanced->setCheckable(true);
    m_btn_mode_advanced->setChecked(false);
    m_btn_mode_advanced->setCursor(Qt::PointingHandCursor);
    m_btn_mode_advanced->setFixedHeight(30);

    const QString active_style = QStringLiteral(
        "QPushButton { background-color: #00C3E3; color: #12141A; font-weight: bold; border-radius: 14px; padding: 4px 16px; border: none; font-size: 11px; }");
    const QString inactive_style = QStringLiteral(
        "QPushButton { background-color: transparent; color: #8E9297; font-weight: 500; border-radius: 14px; padding: 4px 16px; border: none; font-size: 11px; }"
        "QPushButton:hover { color: #FFFFFF; }");

    m_btn_mode_basic->setStyleSheet(active_style);
    m_btn_mode_advanced->setStyleSheet(inactive_style);

    toggle_layout->addWidget(m_btn_mode_basic);
    toggle_layout->addWidget(m_btn_mode_advanced);
    header_layout->addWidget(toggle_container);
    main_layout->addLayout(header_layout);

    // Divider
    auto* divider = new QFrame(this);
    divider->setFrameShape(QFrame::HLine);
    divider->setStyleSheet(QStringLiteral("color: #2D313A; max-height: 1px;"));
    main_layout->addWidget(divider);

    // Stack for Basic vs Advanced
    m_mode_stack = new QStackedWidget(this);
    m_mode_stack->addWidget(CreateBasicPage());
    m_mode_stack->addWidget(CreateAdvancedPage());
    main_layout->addWidget(m_mode_stack, 1);

    connect(m_btn_mode_basic, &QPushButton::clicked, this, [this, active_style, inactive_style] {
        m_btn_mode_basic->setChecked(true);
        m_btn_mode_advanced->setChecked(false);
        m_btn_mode_basic->setStyleSheet(active_style);
        m_btn_mode_advanced->setStyleSheet(inactive_style);
        m_mode_stack->setCurrentIndex(0);
    });

    connect(m_btn_mode_advanced, &QPushButton::clicked, this, [this, active_style, inactive_style] {
        m_btn_mode_advanced->setChecked(true);
        m_btn_mode_basic->setChecked(false);
        m_btn_mode_advanced->setStyleSheet(active_style);
        m_btn_mode_basic->setStyleSheet(inactive_style);
        m_mode_stack->setCurrentIndex(1);
    });

    // Bottom Action Bar
    auto* footer_layout = new QHBoxLayout();
    footer_layout->setSpacing(12);

    m_btn_restore = new QPushButton(QStringLiteral("Restore Defaults"), this);
    m_btn_restore->setCursor(Qt::PointingHandCursor);
    m_btn_restore->setStyleSheet(QStringLiteral(
        "QPushButton { background: #232731; color: #C4C7CE; border-radius: 8px; padding: 8px 16px; font-weight: 600; border: 1px solid #363B47; }"
        "QPushButton:hover { background: #2C313D; color: #FFFFFF; }"));
    connect(m_btn_restore, &QPushButton::clicked, this, &DaliaSettingsDialog::RestoreDefaults);
    footer_layout->addWidget(m_btn_restore);

    footer_layout->addStretch();

    m_btn_cancel = new QPushButton(QStringLiteral("Cancel"), this);
    m_btn_cancel->setCursor(Qt::PointingHandCursor);
    m_btn_cancel->setStyleSheet(QStringLiteral(
        "QPushButton { background: #232731; color: #C4C7CE; border-radius: 8px; padding: 8px 20px; font-weight: 600; border: 1px solid #363B47; }"
        "QPushButton:hover { background: #2C313D; color: #FFFFFF; }"));
    connect(m_btn_cancel, &QPushButton::clicked, this, &QDialog::reject);
    footer_layout->addWidget(m_btn_cancel);

    m_btn_apply = new QPushButton(QStringLiteral("Apply & Save"), this);
    m_btn_apply->setCursor(Qt::PointingHandCursor);
    m_btn_apply->setStyleSheet(QStringLiteral(
        "QPushButton { background: #00C3E3; color: #12141A; border-radius: 8px; padding: 8px 24px; font-weight: bold; border: none; font-size: 13px; }"
        "QPushButton:hover { background: #33D4ED; }"));
    connect(m_btn_apply, &QPushButton::clicked, this, &DaliaSettingsDialog::SaveSettings);
    footer_layout->addWidget(m_btn_apply);

    main_layout->addLayout(footer_layout);
}

QWidget* DaliaSettingsDialog::CreateBasicPage() {
    auto* widget = new QWidget(this);
    auto* layout = new QVBoxLayout(widget);
    layout->setContentsMargins(0, 8, 0, 0);
    layout->setSpacing(16);

    const QString group_box_style = QStringLiteral(
        "QGroupBox { font-weight: bold; font-size: 12px; color: #00C3E3; border: 1px solid #282C37; border-radius: 10px; margin-top: 14px; padding-top: 14px; background: #161820; }"
        "QGroupBox::title { subcontrol-origin: margin; left: 14px; padding: 0 6px; }");

    // Profile Presets Group
    auto* profile_group = new QGroupBox(QStringLiteral("PERFORMANCE PROFILES"), widget);
    profile_group->setStyleSheet(group_box_style);
    auto* profile_layout = new QHBoxLayout(profile_group);
    profile_layout->setContentsMargins(16, 16, 16, 16);
    profile_layout->setSpacing(12);

    auto* profile_label = new QLabel(QStringLiteral("Quick Configuration Profile:"), profile_group);
    profile_label->setStyleSheet(QStringLiteral("color: #FFFFFF; font-weight: 500; font-size: 12px;"));
    profile_layout->addWidget(profile_label);

    m_combo_preset = new QComboBox(profile_group);
    m_combo_preset->addItem(QStringLiteral("⚖️ Balanced Console (Native 1080p, BC3, 60 FPS Recommended)"));
    m_combo_preset->addItem(QStringLiteral("🎮 Handheld / Battery Saver (720p Native, BC3, 60 FPS)"));
    m_combo_preset->addItem(QStringLiteral("🚀 High Performance (AMD RX 7900 XT & Core i5-13400F — 1440p/4K, 12GB Layout)"));
    m_combo_preset->addItem(QStringLiteral("💎 Ultra 4K Fidelity (4K UHD 2160p, SMAA, High Accuracy)"));
    m_combo_preset->setStyleSheet(QStringLiteral(
        "QComboBox { background: #21242E; color: #FFFFFF; border: 1px solid #363B4A; border-radius: 6px; padding: 6px 12px; min-width: 320px; }"
        "QComboBox QAbstractItemView { background: #1A1D26; color: #FFFFFF; selection-background-color: #00C3E3; selection-color: #12141A; }"));
    profile_layout->addWidget(m_combo_preset, 1);
    layout->addWidget(profile_group);

    connect(m_combo_preset, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &DaliaSettingsDialog::ApplyProfile);

    // Graphics Settings Group
    auto* graphics_group = new QGroupBox(QStringLiteral("GRAPHICS & DISPLAY"), widget);
    graphics_group->setStyleSheet(group_box_style);
    auto* g_grid = new QGridLayout(graphics_group);
    g_grid->setContentsMargins(16, 16, 16, 16);
    g_grid->setHorizontalSpacing(18);
    g_grid->setVerticalSpacing(12);

    // Resolution
    g_grid->addWidget(new QLabel(QStringLiteral("Resolution Scaling:"), graphics_group), 0, 0);
    m_combo_resolution = new QComboBox(graphics_group);
    m_combo_resolution->addItem(QStringLiteral("0.5X (360p / 540p)"), static_cast<u32>(Settings::ResolutionSetup::Res1_2X));
    m_combo_resolution->addItem(QStringLiteral("0.75X (540p / 810p)"), static_cast<u32>(Settings::ResolutionSetup::Res3_4X));
    m_combo_resolution->addItem(QStringLiteral("1X Native (720p / 1080p)"), static_cast<u32>(Settings::ResolutionSetup::Res1X));
    m_combo_resolution->addItem(QStringLiteral("1.5X (1080p / 1620p)"), static_cast<u32>(Settings::ResolutionSetup::Res3_2X));
    m_combo_resolution->addItem(QStringLiteral("2X 1440p / 4K (Recommended for RX 7900 XT)"), static_cast<u32>(Settings::ResolutionSetup::Res2X));
    m_combo_resolution->addItem(QStringLiteral("3X (2160p / 3240p)"), static_cast<u32>(Settings::ResolutionSetup::Res3X));
    m_combo_resolution->addItem(QStringLiteral("4X 4K / 8K UHD"), static_cast<u32>(Settings::ResolutionSetup::Res4X));
    g_grid->addWidget(m_combo_resolution, 0, 1);

    // Aspect Ratio
    g_grid->addWidget(new QLabel(QStringLiteral("Aspect Ratio:"), graphics_group), 0, 2);
    m_combo_aspect = new QComboBox(graphics_group);
    m_combo_aspect->addItem(QStringLiteral("16:9 (Standard)"), static_cast<u32>(Settings::AspectRatio::R16_9));
    m_combo_aspect->addItem(QStringLiteral("4:3"), static_cast<u32>(Settings::AspectRatio::R4_3));
    m_combo_aspect->addItem(QStringLiteral("21:9 (Ultrawide)"), static_cast<u32>(Settings::AspectRatio::R21_9));
    m_combo_aspect->addItem(QStringLiteral("16:10"), static_cast<u32>(Settings::AspectRatio::R16_10));
    m_combo_aspect->addItem(QStringLiteral("Stretch to Window"), static_cast<u32>(Settings::AspectRatio::Stretch));
    g_grid->addWidget(m_combo_aspect, 0, 3);

    // Scaling Filter
    g_grid->addWidget(new QLabel(QStringLiteral("Window Adapting Filter:"), graphics_group), 1, 0);
    m_combo_filter = new QComboBox(graphics_group);
    m_combo_filter->addItem(QStringLiteral("Bilinear"), static_cast<u32>(Settings::ScalingFilter::Bilinear));
    m_combo_filter->addItem(QStringLiteral("Bicubic"), static_cast<u32>(Settings::ScalingFilter::Bicubic));
    m_combo_filter->addItem(QStringLiteral("ScaleForce"), static_cast<u32>(Settings::ScalingFilter::ScaleForce));
    m_combo_filter->addItem(QStringLiteral("AMD FidelityFX Super Resolution (FSR)"), static_cast<u32>(Settings::ScalingFilter::Fsr));
    m_combo_filter->addItem(QStringLiteral("AMD FSR 3"), static_cast<u32>(Settings::ScalingFilter::Fsr3));
    g_grid->addWidget(m_combo_filter, 1, 1);

    // FSR Sharpness Slider
    auto* fsr_box = new QHBoxLayout();
    m_slider_fsr_sharpness = new QSlider(Qt::Horizontal, graphics_group);
    m_slider_fsr_sharpness->setRange(0, 100);
    m_slider_fsr_sharpness->setValue(25);
    m_label_fsr_sharpness = new QLabel(QStringLiteral("25%"), graphics_group);
    m_label_fsr_sharpness->setFixedWidth(36);
    connect(m_slider_fsr_sharpness, &QSlider::valueChanged, this, [this](int val) {
        m_label_fsr_sharpness->setText(QStringLiteral("%1%").arg(val));
    });
    fsr_box->addWidget(m_slider_fsr_sharpness);
    fsr_box->addWidget(m_label_fsr_sharpness);
    g_grid->addWidget(new QLabel(QStringLiteral("FSR Sharpness:"), graphics_group), 1, 2);
    g_grid->addLayout(fsr_box, 1, 3);

    // Anti-Aliasing
    g_grid->addWidget(new QLabel(QStringLiteral("Anti-Aliasing (AA):"), graphics_group), 2, 0);
    m_combo_aa = new QComboBox(graphics_group);
    m_combo_aa->addItem(QStringLiteral("None"), static_cast<u32>(Settings::AntiAliasing::None));
    m_combo_aa->addItem(QStringLiteral("FXAA (Fast)"), static_cast<u32>(Settings::AntiAliasing::Fxaa));
    m_combo_aa->addItem(QStringLiteral("SMAA (High Quality)"), static_cast<u32>(Settings::AntiAliasing::Smaa));
    g_grid->addWidget(m_combo_aa, 2, 1);

    // Docked Mode
    m_check_docked = new QCheckBox(QStringLiteral("Enable Docked Mode (Higher TDP & Clock)"), graphics_group);
    m_check_docked->setChecked(true);
    g_grid->addWidget(m_check_docked, 2, 2, 1, 2);

    layout->addWidget(graphics_group);

    // Audio & General Group
    auto* audio_group = new QGroupBox(QStringLiteral("AUDIO & CONSOLE"), widget);
    audio_group->setStyleSheet(group_box_style);
    auto* a_grid = new QGridLayout(audio_group);
    a_grid->setContentsMargins(16, 16, 16, 16);
    a_grid->setHorizontalSpacing(18);

    a_grid->addWidget(new QLabel(QStringLiteral("Master Audio Volume:"), audio_group), 0, 0);
    auto* vol_box = new QHBoxLayout();
    m_slider_volume = new QSlider(Qt::Horizontal, audio_group);
    m_slider_volume->setRange(0, 100);
    m_slider_volume->setValue(100);
    m_label_volume = new QLabel(QStringLiteral("100%"), audio_group);
    m_label_volume->setFixedWidth(36);
    connect(m_slider_volume, &QSlider::valueChanged, this, [this](int val) {
        m_label_volume->setText(QStringLiteral("%1%").arg(val));
    });
    vol_box->addWidget(m_slider_volume);
    vol_box->addWidget(m_label_volume);
    a_grid->addLayout(vol_box, 0, 1);

    layout->addWidget(audio_group);
    layout->addStretch();
    return widget;
}

QWidget* DaliaSettingsDialog::CreateAdvancedPage() {
    auto* widget = new QWidget(this);
    auto* layout = new QVBoxLayout(widget);
    layout->setContentsMargins(0, 8, 0, 0);
    layout->setSpacing(16);

    const QString group_box_style = QStringLiteral(
        "QGroupBox { font-weight: bold; font-size: 12px; color: #00C3E3; border: 1px solid #282C37; border-radius: 10px; margin-top: 14px; padding-top: 14px; background: #161820; }"
        "QGroupBox::title { subcontrol-origin: margin; left: 14px; padding: 0 6px; }");

    // Vulkan & GPU Architecture
    auto* vk_group = new QGroupBox(QStringLiteral("VULKAN & GPU PIPELINES"), widget);
    vk_group->setStyleSheet(group_box_style);
    auto* vk_grid = new QGridLayout(vk_group);
    vk_grid->setContentsMargins(16, 16, 16, 16);
    vk_grid->setHorizontalSpacing(18);
    vk_grid->setVerticalSpacing(12);

    vk_grid->addWidget(new QLabel(QStringLiteral("Vulkan Device:"), vk_group), 0, 0);
    m_combo_vk_device = new QComboBox(vk_group);
    if (m_vk_records.empty()) {
        m_combo_vk_device->addItem(QStringLiteral("AMD Radeon RX 7900 XT (Vulkan)"));
    } else {
        for (const auto& rec : m_vk_records) {
            m_combo_vk_device->addItem(QString::fromStdString(rec.name));
        }
    }
    vk_grid->addWidget(m_combo_vk_device, 0, 1);

    vk_grid->addWidget(new QLabel(QStringLiteral("ASTC Texture Recompression:"), vk_group), 0, 2);
    m_combo_astc_recompression = new QComboBox(vk_group);
    m_combo_astc_recompression->addItem(QStringLiteral("Uncompressed (High VRAM footprint)"), static_cast<u32>(Settings::AstcRecompression::Uncompressed));
    m_combo_astc_recompression->addItem(QStringLiteral("BC1 (Low VRAM, lower color quality)"), static_cast<u32>(Settings::AstcRecompression::Bc1));
    m_combo_astc_recompression->addItem(QStringLiteral("BC3 (Recommended — 1/4 VRAM, prevents crash)"), static_cast<u32>(Settings::AstcRecompression::Bc3));
    vk_grid->addWidget(m_combo_astc_recompression, 0, 3);

    vk_grid->addWidget(new QLabel(QStringLiteral("VRAM Usage Mode:"), vk_group), 1, 0);
    m_combo_vram_mode = new QComboBox(vk_group);
    m_combo_vram_mode->addItem(QStringLiteral("Conservative (Headroom protected — Recommended)"), static_cast<u32>(Settings::VramUsageMode::Conservative));
    m_combo_vram_mode->addItem(QStringLiteral("Aggressive (May cause VK_ERROR_DEVICE_LOST)"), static_cast<u32>(Settings::VramUsageMode::Aggressive));
    vk_grid->addWidget(m_combo_vram_mode, 1, 1);

    vk_grid->addWidget(new QLabel(QStringLiteral("Extended Dynamic State:"), vk_group), 1, 2);
    m_combo_extended_dynamic_state = new QComboBox(vk_group);
    m_combo_extended_dynamic_state->addItem(QStringLiteral("Disabled"), static_cast<u32>(Settings::ExtendedDynamicState::Disabled));
    m_combo_extended_dynamic_state->addItem(QStringLiteral("EDS 1"), static_cast<u32>(Settings::ExtendedDynamicState::EDS1));
    m_combo_extended_dynamic_state->addItem(QStringLiteral("EDS 2"), static_cast<u32>(Settings::ExtendedDynamicState::EDS2));
    m_combo_extended_dynamic_state->addItem(QStringLiteral("EDS 3 (Vulkan 1.3 Recommended)"), static_cast<u32>(Settings::ExtendedDynamicState::EDS3));
    vk_grid->addWidget(m_combo_extended_dynamic_state, 1, 3);

    m_check_async_gpu = new QCheckBox(QStringLiteral("Use Asynchronous GPU Emulation"), vk_group);
    m_check_disk_shaders = new QCheckBox(QStringLiteral("Use Disk Shader Cache"), vk_group);
    m_check_driver_cache = new QCheckBox(QStringLiteral("Use Vulkan Driver Pipeline Cache"), vk_group);
    m_check_reactive_flushing = new QCheckBox(QStringLiteral("Use Reactive Flushing"), vk_group);
    m_check_gpl =
        new QCheckBox(QStringLiteral("Use Vulkan Graphics Pipeline Library (VK_EXT_gpl)"), vk_group);
    m_check_precompile_shaders =
        new QCheckBox(QStringLiteral("Precompile Game Shaders Before Launch"), vk_group);

    vk_grid->addWidget(m_check_async_gpu, 2, 0);
    vk_grid->addWidget(m_check_disk_shaders, 2, 1);
    vk_grid->addWidget(m_check_driver_cache, 2, 2);
    vk_grid->addWidget(m_check_reactive_flushing, 2, 3);
    vk_grid->addWidget(m_check_gpl, 3, 0, 1, 2);
    vk_grid->addWidget(m_check_precompile_shaders, 3, 2, 1, 2);

    layout->addWidget(vk_group);

    // CPU & Memory Architecture
    auto* cpu_group = new QGroupBox(QStringLiteral("CPU & SWITCH 2 MEMORY ARCHITECTURE"), widget);
    cpu_group->setStyleSheet(group_box_style);
    auto* cpu_grid = new QGridLayout(cpu_group);
    cpu_grid->setContentsMargins(16, 16, 16, 16);
    cpu_grid->setHorizontalSpacing(18);
    cpu_grid->setVerticalSpacing(12);

    cpu_grid->addWidget(new QLabel(QStringLiteral("CPU Accuracy:"), cpu_group), 0, 0);
    m_combo_cpu_accuracy = new QComboBox(cpu_group);
    m_combo_cpu_accuracy->addItem(QStringLiteral("Auto (Recommended)"), static_cast<u32>(Settings::CpuAccuracy::Auto));
    m_combo_cpu_accuracy->addItem(QStringLiteral("Accurate"), static_cast<u32>(Settings::CpuAccuracy::Accurate));
    m_combo_cpu_accuracy->addItem(QStringLiteral("Unsafe (Fastest)"), static_cast<u32>(Settings::CpuAccuracy::Unsafe));
    cpu_grid->addWidget(m_combo_cpu_accuracy, 0, 1);

    cpu_grid->addWidget(new QLabel(QStringLiteral("Memory Layout:"), cpu_group), 0, 2);
    m_combo_memory_layout = new QComboBox(cpu_group);
    m_combo_memory_layout->addItem(QStringLiteral("4 GB (Standard Switch)"), static_cast<u32>(Settings::MemoryLayout::Memory_4Gb));
    m_combo_memory_layout->addItem(QStringLiteral("6 GB (Enhanced)"), static_cast<u32>(Settings::MemoryLayout::Memory_6Gb));
    m_combo_memory_layout->addItem(QStringLiteral("8 GB (Pro)"), static_cast<u32>(Settings::MemoryLayout::Memory_8Gb));
    m_combo_memory_layout->addItem(QStringLiteral("12 GB (Nintendo Switch 2 Architecture)"), static_cast<u32>(Settings::MemoryLayout::Memory_12Gb));
    cpu_grid->addWidget(m_combo_memory_layout, 0, 3);

    m_check_multi_core = new QCheckBox(QStringLiteral("Enable Multi-Core CPU Emulation (Prioritize Intel P-Cores)"), cpu_group);
    cpu_grid->addWidget(m_check_multi_core, 1, 0, 1, 2);

    m_check_speed_limit = new QCheckBox(QStringLiteral("Limit Speed (100%)"), cpu_group);
    cpu_grid->addWidget(m_check_speed_limit, 1, 2, 1, 2);

    layout->addWidget(cpu_group);

    // Cache Management Box
    auto* cache_box = new QHBoxLayout();
    cache_box->setContentsMargins(4, 4, 4, 4);

    m_btn_clear_shaders = new QPushButton(QStringLiteral("🧹 Purge Global Vulkan Pipeline & Shader Cache"), widget);
    m_btn_clear_shaders->setCursor(Qt::PointingHandCursor);
    m_btn_clear_shaders->setStyleSheet(QStringLiteral(
        "QPushButton { background: #2F1A24; color: #FF4D6D; border: 1px solid #5C2237; border-radius: 8px; padding: 8px 16px; font-weight: bold; font-size: 11px; }"
        "QPushButton:hover { background: #472132; color: #FFA8B7; }"));
    connect(m_btn_clear_shaders, &QPushButton::clicked, this, &DaliaSettingsDialog::ClearShaderCache);
    cache_box->addWidget(m_btn_clear_shaders);
    cache_box->addStretch();

    layout->addLayout(cache_box);
    layout->addStretch();
    return widget;
}

void DaliaSettingsDialog::ApplyProfile(int profile_index) {
    if (profile_index < 0) return;

    switch (profile_index) {
    case 0: // Balanced Console
        m_combo_resolution->setCurrentIndex(2); // 1X Native
        m_combo_aspect->setCurrentIndex(0);     // 16:9
        m_combo_filter->setCurrentIndex(0);     // Bilinear
        m_combo_aa->setCurrentIndex(1);         // FXAA
        m_check_docked->setChecked(true);
        m_combo_astc_recompression->setCurrentIndex(2); // BC3
        m_combo_vram_mode->setCurrentIndex(0);          // Conservative
        m_combo_memory_layout->setCurrentIndex(3);      // 12GB
        break;
    case 1: // Handheld
        m_combo_resolution->setCurrentIndex(2); // 1X
        m_combo_aspect->setCurrentIndex(0);
        m_combo_filter->setCurrentIndex(0);
        m_combo_aa->setCurrentIndex(0); // None
        m_check_docked->setChecked(false);
        m_combo_astc_recompression->setCurrentIndex(2); // BC3
        m_combo_vram_mode->setCurrentIndex(0);
        break;
    case 2: // High Performance (RX 7900 XT / i5-13400F)
        m_combo_resolution->setCurrentIndex(4); // 2X 1440p/4K
        m_combo_aspect->setCurrentIndex(0);
        m_combo_filter->setCurrentIndex(3); // FSR
        m_slider_fsr_sharpness->setValue(40);
        m_combo_aa->setCurrentIndex(2); // SMAA
        m_check_docked->setChecked(true);
        m_combo_astc_recompression->setCurrentIndex(2); // BC3
        m_combo_vram_mode->setCurrentIndex(0);          // Conservative
        m_combo_memory_layout->setCurrentIndex(3);      // 12GB Switch 2
        m_combo_cpu_accuracy->setCurrentIndex(0);       // Auto
        m_check_multi_core->setChecked(true);
        m_check_async_gpu->setChecked(true);
        break;
    case 3: // Ultra 4K
        m_combo_resolution->setCurrentIndex(6); // 4X UHD
        m_combo_aspect->setCurrentIndex(0);
        m_combo_filter->setCurrentIndex(3); // FSR
        m_combo_aa->setCurrentIndex(2);     // SMAA
        m_check_docked->setChecked(true);
        m_combo_astc_recompression->setCurrentIndex(2); // BC3
        m_combo_vram_mode->setCurrentIndex(0);
        m_combo_memory_layout->setCurrentIndex(3); // 12GB
        break;
    }
}

void DaliaSettingsDialog::LoadSettings() {
    // Basic
    const auto res = Settings::values.resolution_setup.GetValue();
    for (int i = 0; i < m_combo_resolution->count(); ++i) {
        if (m_combo_resolution->itemData(i).toUInt() == static_cast<u32>(res)) {
            m_combo_resolution->setCurrentIndex(i);
            break;
        }
    }

    const auto asp = Settings::values.aspect_ratio.GetValue();
    for (int i = 0; i < m_combo_aspect->count(); ++i) {
        if (m_combo_aspect->itemData(i).toUInt() == static_cast<u32>(asp)) {
            m_combo_aspect->setCurrentIndex(i);
            break;
        }
    }

    const auto flt = Settings::values.scaling_filter.GetValue();
    for (int i = 0; i < m_combo_filter->count(); ++i) {
        if (m_combo_filter->itemData(i).toUInt() == static_cast<u32>(flt)) {
            m_combo_filter->setCurrentIndex(i);
            break;
        }
    }

    m_slider_fsr_sharpness->setValue(Settings::values.fsr_sharpening_slider.GetValue());

    const auto aa = Settings::values.anti_aliasing.GetValue();
    for (int i = 0; i < m_combo_aa->count(); ++i) {
        if (m_combo_aa->itemData(i).toUInt() == static_cast<u32>(aa)) {
            m_combo_aa->setCurrentIndex(i);
            break;
        }
    }

    m_check_docked->setChecked(Settings::values.use_docked_mode.GetValue() == Settings::ConsoleMode::Docked);
    m_slider_volume->setValue(Settings::values.volume.GetValue());

    // Advanced
    const u32 dev_index = Settings::values.vulkan_device.GetValue();
    if (dev_index < static_cast<u32>(m_combo_vk_device->count())) {
        m_combo_vk_device->setCurrentIndex(dev_index);
    }

    const auto astc = Settings::values.astc_recompression.GetValue();
    for (int i = 0; i < m_combo_astc_recompression->count(); ++i) {
        if (m_combo_astc_recompression->itemData(i).toUInt() == static_cast<u32>(astc)) {
            m_combo_astc_recompression->setCurrentIndex(i);
            break;
        }
    }

    const auto vram = Settings::values.vram_usage_mode.GetValue();
    for (int i = 0; i < m_combo_vram_mode->count(); ++i) {
        if (m_combo_vram_mode->itemData(i).toUInt() == static_cast<u32>(vram)) {
            m_combo_vram_mode->setCurrentIndex(i);
            break;
        }
    }

    const auto eds = Settings::values.dyna_state.GetValue();
    for (int i = 0; i < m_combo_extended_dynamic_state->count(); ++i) {
        if (m_combo_extended_dynamic_state->itemData(i).toUInt() == static_cast<u32>(eds)) {
            m_combo_extended_dynamic_state->setCurrentIndex(i);
            break;
        }
    }

    m_check_async_gpu->setChecked(Settings::values.use_asynchronous_gpu_emulation.GetValue());
    m_check_disk_shaders->setChecked(Settings::values.use_disk_shader_cache.GetValue());
    m_check_driver_cache->setChecked(Settings::values.use_vulkan_driver_pipeline_cache.GetValue());
    m_check_reactive_flushing->setChecked(Settings::values.use_reactive_flushing.GetValue());
    m_check_gpl->setChecked(Settings::values.use_graphics_pipeline_library.GetValue());
    m_check_precompile_shaders->setChecked(Settings::values.precompile_shaders.GetValue());

    const auto cpu_acc = Settings::values.cpu_accuracy.GetValue();
    for (int i = 0; i < m_combo_cpu_accuracy->count(); ++i) {
        if (m_combo_cpu_accuracy->itemData(i).toUInt() == static_cast<u32>(cpu_acc)) {
            m_combo_cpu_accuracy->setCurrentIndex(i);
            break;
        }
    }

    m_check_multi_core->setChecked(Settings::values.use_multi_core.GetValue());

    const auto mem = Settings::values.memory_layout_mode.GetValue();
    for (int i = 0; i < m_combo_memory_layout->count(); ++i) {
        if (m_combo_memory_layout->itemData(i).toUInt() == static_cast<u32>(mem)) {
            m_combo_memory_layout->setCurrentIndex(i);
            break;
        }
    }

    m_check_speed_limit->setChecked(Settings::values.use_speed_limit.GetValue());
}

void DaliaSettingsDialog::SaveSettings() {
    Settings::values.resolution_setup.SetValue(
        static_cast<Settings::ResolutionSetup>(m_combo_resolution->currentData().toUInt()));
    Settings::values.aspect_ratio.SetValue(
        static_cast<Settings::AspectRatio>(m_combo_aspect->currentData().toUInt()));
    Settings::values.scaling_filter.SetValue(
        static_cast<Settings::ScalingFilter>(m_combo_filter->currentData().toUInt()));
    Settings::values.fsr_sharpening_slider.SetValue(m_slider_fsr_sharpness->value());
    Settings::values.anti_aliasing.SetValue(
        static_cast<Settings::AntiAliasing>(m_combo_aa->currentData().toUInt()));
    Settings::values.use_docked_mode.SetValue(
        m_check_docked->isChecked() ? Settings::ConsoleMode::Docked : Settings::ConsoleMode::Handheld);
    Settings::values.volume.SetValue(static_cast<u8>(m_slider_volume->value()));

    Settings::values.vulkan_device.SetValue(m_combo_vk_device->currentIndex());
    Settings::values.astc_recompression.SetValue(
        static_cast<Settings::AstcRecompression>(m_combo_astc_recompression->currentData().toUInt()));
    Settings::values.vram_usage_mode.SetValue(
        static_cast<Settings::VramUsageMode>(m_combo_vram_mode->currentData().toUInt()));
    Settings::values.dyna_state.SetValue(
        static_cast<Settings::ExtendedDynamicState>(m_combo_extended_dynamic_state->currentData().toUInt()));

    Settings::values.use_asynchronous_gpu_emulation.SetValue(m_check_async_gpu->isChecked());
    Settings::values.use_disk_shader_cache.SetValue(m_check_disk_shaders->isChecked());
    Settings::values.use_vulkan_driver_pipeline_cache.SetValue(m_check_driver_cache->isChecked());
    Settings::values.use_reactive_flushing.SetValue(m_check_reactive_flushing->isChecked());
    Settings::values.use_graphics_pipeline_library.SetValue(m_check_gpl->isChecked());
    Settings::values.precompile_shaders.SetValue(m_check_precompile_shaders->isChecked());

    Settings::values.cpu_accuracy.SetValue(
        static_cast<Settings::CpuAccuracy>(m_combo_cpu_accuracy->currentData().toUInt()));
    Settings::values.use_multi_core.SetValue(m_check_multi_core->isChecked());
    Settings::values.memory_layout_mode.SetValue(
        static_cast<Settings::MemoryLayout>(m_combo_memory_layout->currentData().toUInt()));
    Settings::values.use_speed_limit.SetValue(m_check_speed_limit->isChecked());

    if (QtCommon::system) {
        QtCommon::system->ApplySettings();
    }
    Settings::LogSettings();
    UISettings::values.configuration_applied = true;
    accept();
}

void DaliaSettingsDialog::RestoreDefaults() {
    auto res = QMessageBox::question(
        this, tr("Reset Settings"),
        tr("Are you sure you want to restore recommended DALIA defaults for RX 7900 XT and Core i5-13400F?"),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);

    if (res == QMessageBox::Yes) {
        ApplyProfile(2); // High Performance
    }
}

void DaliaSettingsDialog::ClearShaderCache() {
    auto res = QMessageBox::question(
        this, tr("Purge Vulkan Shader Cache"),
        tr("Purging the Vulkan pipeline and shader cache will force all shaders to recompile cleanly on next launch. Continue?"),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);

    if (res != QMessageBox::Yes) return;

    try {
        const auto shader_dir = Common::FS::GetEdenPath(Common::FS::EdenPath::ShaderDir);
        Common::FS::RemoveDirRecursively(shader_dir);
        QMessageBox::information(this, tr("Success"), tr("Shader cache has been cleared successfully."));
    } catch (const std::exception& e) {
        QMessageBox::warning(this, tr("Error"), tr("Failed to clear cache: %1").arg(e.what()));
    }
}

void DaliaSettingsDialog::paintEvent(QPaintEvent* event) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);

    // Deep graphite background
    painter.fillRect(rect(), QColor(18, 20, 26));

    // Outer subtle neon border
    painter.setPen(QPen(QColor(0, 195, 227, 45), 1.5));
    painter.setBrush(Qt::NoBrush);
    painter.drawRoundedRect(rect().adjusted(1, 1, -1, -1), 12, 12);
}

void DaliaSettingsDialog::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Escape) {
        reject();
        return;
    }
    QDialog::keyPressEvent(event);
}

