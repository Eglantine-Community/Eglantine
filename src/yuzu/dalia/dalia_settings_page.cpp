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
#include <QPushButton>
#include <QScrollArea>
#include <QSlider>
#include <QStackedWidget>
#include <QTimer>
#include <QVBoxLayout>

#include "common/fs/fs.h"
#include "common/fs/fs_paths.h"
#include "common/fs/path_util.h"
#include "common/settings.h"
#include "qt_common/config/uisettings.h"
#include "qt_common/qt_common.h"
#include "yuzu/dalia/dalia_settings_page.h"
#include "yuzu/dalia/dalia_theme.h"
#include "yuzu/main_window.h"

DaliaSettingsPage::DaliaSettingsPage(MainWindow* main_window, const std::vector<VkDeviceInfo::Record>& vk_records, QWidget* parent)
    : QWidget{parent}, m_main_window{main_window}, m_vk_records{vk_records} {
    setStyleSheet(DaliaTheme::GetGlobalControlStyle());
    SetupUI();
    LoadSettings();
}

DaliaSettingsPage::~DaliaSettingsPage() = default;

void DaliaSettingsPage::SetupUI() {
    auto* main_layout = new QVBoxLayout(this);
    main_layout->setContentsMargins(28, 22, 28, 16);
    main_layout->setSpacing(14);

    // Header
    auto* header_layout = new QHBoxLayout();
    header_layout->setSpacing(16);

    auto* title_box = new QVBoxLayout();
    title_box->setSpacing(2);

    auto* title_label = new QLabel(QStringLiteral("Ajustes del Sistema"), this);
    QFont title_font = title_label->font();
    title_font.setPointSize(20);
    title_font.setBold(true);
    title_label->setFont(title_font);
    title_label->setStyleSheet(QStringLiteral("color: %1; letter-spacing: 0.3px;").arg(DaliaTheme::ColorTextPrimary));

    auto* subtitle_label = new QLabel(QStringLiteral("Configuración gráfica, Vulkan, CPU y arquitectura de memoria"), this);
    subtitle_label->setStyleSheet(QStringLiteral("color: %1; font-size: 13px; font-weight: 500;").arg(DaliaTheme::ColorTextSecondary));

    title_box->addWidget(title_label);
    title_box->addWidget(subtitle_label);
    header_layout->addLayout(title_box);
    header_layout->addStretch();

    // Mode Toggle Pill: Basic vs Advanced
    auto* toggle_container = new QFrame(this);
    toggle_container->setStyleSheet(QStringLiteral(
        "QFrame { background-color: %1; border: 1px solid %2; border-radius: 16px; padding: 2px; }")
        .arg(DaliaTheme::ColorSurfaceCard, DaliaTheme::ColorBorderCard));
    auto* toggle_layout = new QHBoxLayout(toggle_container);
    toggle_layout->setContentsMargins(2, 2, 2, 2);
    toggle_layout->setSpacing(4);

    m_btn_mode_basic = new QPushButton(QStringLiteral("Básico"), this);
    m_btn_mode_basic->setCheckable(true);
    m_btn_mode_basic->setChecked(true);
    m_btn_mode_basic->setCursor(Qt::PointingHandCursor);
    m_btn_mode_basic->setFixedHeight(28);

    m_btn_mode_advanced = new QPushButton(QStringLiteral("Avanzado"), this);
    m_btn_mode_advanced->setCheckable(true);
    m_btn_mode_advanced->setChecked(false);
    m_btn_mode_advanced->setCursor(Qt::PointingHandCursor);
    m_btn_mode_advanced->setFixedHeight(28);

    const QString active_style = QStringLiteral(
        "QPushButton { background-color: %1; color: #101318; font-weight: bold; border-radius: 14px; padding: 4px 18px; border: none; font-size: 11px; }")
        .arg(DaliaTheme::ColorAccentIceBlue);
    const QString inactive_style = QStringLiteral(
        "QPushButton { background-color: transparent; color: #8E9297; font-weight: 600; border-radius: 14px; padding: 4px 18px; border: none; font-size: 11px; }"
        "QPushButton:hover { color: #FFFFFF; }");

    m_btn_mode_basic->setStyleSheet(active_style);
    m_btn_mode_advanced->setStyleSheet(inactive_style);

    toggle_layout->addWidget(m_btn_mode_basic);
    toggle_layout->addWidget(m_btn_mode_advanced);
    header_layout->addWidget(toggle_container);
    main_layout->addLayout(header_layout);

    // Scrollable Stack
    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);

    m_mode_stack = new QStackedWidget(scroll);
    m_mode_stack->setStyleSheet(QStringLiteral("background: transparent;"));
    m_mode_stack->addWidget(CreateBasicView());
    m_mode_stack->addWidget(CreateAdvancedView());
    scroll->setWidget(m_mode_stack);

    main_layout->addWidget(scroll, 1);

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

    // Action Bar
    auto* footer_layout = new QHBoxLayout();
    footer_layout->setSpacing(12);

    m_btn_restore = new QPushButton(QStringLiteral("Restaurar Recomendados"), this);
    m_btn_restore->setCursor(Qt::PointingHandCursor);
    m_btn_restore->setStyleSheet(QStringLiteral(
        "QPushButton { background: %1; color: %2; border-radius: 8px; padding: 9px 16px; font-weight: 600; border: 1px solid %3; font-size: 12px; }"
        "QPushButton:hover { background: %4; color: #FFFFFF; }")
        .arg(DaliaTheme::ColorSurfaceElevated, DaliaTheme::ColorTextSecondary,
             DaliaTheme::ColorBorderCard, DaliaTheme::ColorSurfaceHover));
    connect(m_btn_restore, &QPushButton::clicked, this, &DaliaSettingsPage::RestoreDefaults);
    footer_layout->addWidget(m_btn_restore);

    m_status_feedback = new QLabel(this);
    m_status_feedback->setStyleSheet(QStringLiteral("color: %1; font-size: 12px; font-weight: bold;").arg(DaliaTheme::ColorStatusGreen));
    footer_layout->addWidget(m_status_feedback);

    footer_layout->addStretch();

    m_btn_apply = new QPushButton(QStringLiteral(" Aplicar y Guardar "), this);
    m_btn_apply->setCursor(Qt::PointingHandCursor);
    m_btn_apply->setStyleSheet(QStringLiteral(
        "QPushButton { background: %1; color: #101318; border-radius: 8px; padding: 9px 24px; font-weight: bold; border: none; font-size: 12px; }"
        "QPushButton:hover { background: #5EC4F0; }")
        .arg(DaliaTheme::ColorAccentIceBlue));
    connect(m_btn_apply, &QPushButton::clicked, this, &DaliaSettingsPage::SaveSettings);
    footer_layout->addWidget(m_btn_apply);

    main_layout->addLayout(footer_layout);
}

QWidget* DaliaSettingsPage::CreateBasicView() {
    auto* widget = new QWidget(this);
    widget->setStyleSheet(QStringLiteral("background: transparent;"));
    auto* layout = new QVBoxLayout(widget);
    layout->setContentsMargins(0, 4, 6, 4);
    layout->setSpacing(14);

    const QString group_style = QStringLiteral(
        "QGroupBox { font-weight: bold; font-size: 11px; color: %1; border: 1px solid %2; border-radius: 12px; margin-top: 14px; padding-top: 16px; background: %3; }"
        "QGroupBox::title { subcontrol-origin: margin; left: 16px; padding: 0 8px; letter-spacing: 1px; }"
        "QLabel { color: #D2D7E0; font-size: 12px; background: transparent; }")
        .arg(DaliaTheme::ColorAccentIceBlue, DaliaTheme::ColorBorderCard, DaliaTheme::ColorSurfaceCard);

    // Profiles
    auto* profile_group = new QGroupBox(QStringLiteral("PERFIL DE RENDIMIENTO"), widget);
    profile_group->setStyleSheet(group_style);
    auto* p_layout = new QHBoxLayout(profile_group);
    p_layout->setContentsMargins(18, 12, 18, 14);
    p_layout->setSpacing(14);

    p_layout->addWidget(new QLabel(QStringLiteral("Perfil rápido:"), profile_group));
    m_combo_preset = new QComboBox(profile_group);
    m_combo_preset->addItem(QStringLiteral("Consola Equilibrada (1080p Nativo, BC3, 60 FPS)"));
    m_combo_preset->addItem(QStringLiteral("Modo Portátil (720p, Ahorro de Energía)"));
    m_combo_preset->addItem(QStringLiteral("Alto Rendimiento — Vulkan 1.3 RDNA 3 (1440p / 4K, 12 GB)"));
    m_combo_preset->addItem(QStringLiteral("Ultra Calidad 4K (2160p UHD, SMAA)"));
    p_layout->addWidget(m_combo_preset, 1);
    layout->addWidget(profile_group);

    connect(m_combo_preset, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &DaliaSettingsPage::ApplyProfile);

    // Graphics Grid
    auto* graphics_group = new QGroupBox(QStringLiteral("GRÁFICOS Y PANTALLA"), widget);
    graphics_group->setStyleSheet(group_style);
    auto* g_grid = new QGridLayout(graphics_group);
    g_grid->setContentsMargins(18, 14, 18, 16);
    g_grid->setHorizontalSpacing(20);
    g_grid->setVerticalSpacing(14);

    g_grid->addWidget(new QLabel(QStringLiteral("Resolución:"), graphics_group), 0, 0);
    m_combo_resolution = new QComboBox(graphics_group);
    m_combo_resolution->addItem(QStringLiteral("0.5X (360p / 540p)"), static_cast<u32>(Settings::ResolutionSetup::Res1_2X));
    m_combo_resolution->addItem(QStringLiteral("0.75X (540p / 810p)"), static_cast<u32>(Settings::ResolutionSetup::Res3_4X));
    m_combo_resolution->addItem(QStringLiteral("1X Nativa (720p / 1080p)"), static_cast<u32>(Settings::ResolutionSetup::Res1X));
    m_combo_resolution->addItem(QStringLiteral("1.5X (1080p / 1620p)"), static_cast<u32>(Settings::ResolutionSetup::Res3_2X));
    m_combo_resolution->addItem(QStringLiteral("2X QHD / 4K (Recomendado)"), static_cast<u32>(Settings::ResolutionSetup::Res2X));
    m_combo_resolution->addItem(QStringLiteral("3X (2160p / 3240p)"), static_cast<u32>(Settings::ResolutionSetup::Res3X));
    m_combo_resolution->addItem(QStringLiteral("4X 4K / 8K UHD"), static_cast<u32>(Settings::ResolutionSetup::Res4X));
    g_grid->addWidget(m_combo_resolution, 0, 1);

    g_grid->addWidget(new QLabel(QStringLiteral("Relación de Aspecto:"), graphics_group), 0, 2);
    m_combo_aspect = new QComboBox(graphics_group);
    m_combo_aspect->addItem(QStringLiteral("16:9 (Estándar)"), static_cast<u32>(Settings::AspectRatio::R16_9));
    m_combo_aspect->addItem(QStringLiteral("4:3"), static_cast<u32>(Settings::AspectRatio::R4_3));
    m_combo_aspect->addItem(QStringLiteral("21:9 (Ultrawide)"), static_cast<u32>(Settings::AspectRatio::R21_9));
    m_combo_aspect->addItem(QStringLiteral("16:10"), static_cast<u32>(Settings::AspectRatio::R16_10));
    m_combo_aspect->addItem(QStringLiteral("Estirar a la Ventana"), static_cast<u32>(Settings::AspectRatio::Stretch));
    g_grid->addWidget(m_combo_aspect, 0, 3);

    g_grid->addWidget(new QLabel(QStringLiteral("Filtro de Escalado:"), graphics_group), 1, 0);
    m_combo_filter = new QComboBox(graphics_group);
    m_combo_filter->addItem(QStringLiteral("Bilinear"), static_cast<u32>(Settings::ScalingFilter::Bilinear));
    m_combo_filter->addItem(QStringLiteral("Bicubic"), static_cast<u32>(Settings::ScalingFilter::Bicubic));
    m_combo_filter->addItem(QStringLiteral("ScaleForce"), static_cast<u32>(Settings::ScalingFilter::ScaleForce));
    m_combo_filter->addItem(QStringLiteral("AMD FSR"), static_cast<u32>(Settings::ScalingFilter::Fsr));
    m_combo_filter->addItem(QStringLiteral("AMD FSR 3"), static_cast<u32>(Settings::ScalingFilter::Fsr3));
    g_grid->addWidget(m_combo_filter, 1, 1);

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
    g_grid->addWidget(new QLabel(QStringLiteral("Nitidez FSR:"), graphics_group), 1, 2);
    g_grid->addLayout(fsr_box, 1, 3);

    g_grid->addWidget(new QLabel(QStringLiteral("Anti-Aliasing:"), graphics_group), 2, 0);
    m_combo_aa = new QComboBox(graphics_group);
    m_combo_aa->addItem(QStringLiteral("Desactivado"), static_cast<u32>(Settings::AntiAliasing::None));
    m_combo_aa->addItem(QStringLiteral("FXAA (Rápido)"), static_cast<u32>(Settings::AntiAliasing::Fxaa));
    m_combo_aa->addItem(QStringLiteral("SMAA (Alta Calidad)"), static_cast<u32>(Settings::AntiAliasing::Smaa));
    g_grid->addWidget(m_combo_aa, 2, 1);

    m_check_docked = new QCheckBox(QStringLiteral("Modo Televisor (Docked — Mayor calidad y frecuencia)"), graphics_group);
    m_check_docked->setChecked(true);
    g_grid->addWidget(m_check_docked, 2, 2, 1, 2);

    layout->addWidget(graphics_group);

    // Audio
    auto* audio_group = new QGroupBox(QStringLiteral("AUDIO"), widget);
    audio_group->setStyleSheet(group_style);
    auto* a_grid = new QGridLayout(audio_group);
    a_grid->setContentsMargins(18, 14, 18, 16);

    a_grid->addWidget(new QLabel(QStringLiteral("Volumen Maestro:"), audio_group), 0, 0);
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

QWidget* DaliaSettingsPage::CreateAdvancedView() {
    auto* widget = new QWidget(this);
    widget->setStyleSheet(QStringLiteral("background: transparent;"));
    auto* layout = new QVBoxLayout(widget);
    layout->setContentsMargins(0, 4, 6, 4);
    layout->setSpacing(14);

    const QString group_style = QStringLiteral(
        "QGroupBox { font-weight: bold; font-size: 11px; color: %1; border: 1px solid %2; border-radius: 12px; margin-top: 14px; padding-top: 16px; background: %3; }"
        "QGroupBox::title { subcontrol-origin: margin; left: 16px; padding: 0 8px; letter-spacing: 1px; }"
        "QLabel { color: #D2D7E0; font-size: 12px; background: transparent; }")
        .arg(DaliaTheme::ColorAccentIceBlue, DaliaTheme::ColorBorderCard, DaliaTheme::ColorSurfaceCard);

    // Vulkan & GPU
    auto* vk_group = new QGroupBox(QStringLiteral("VULKAN Y MOTOR GRÁFICO"), widget);
    vk_group->setStyleSheet(group_style);
    auto* vk_grid = new QGridLayout(vk_group);
    vk_grid->setContentsMargins(18, 14, 18, 16);
    vk_grid->setHorizontalSpacing(20);
    vk_grid->setVerticalSpacing(14);

    vk_grid->addWidget(new QLabel(QStringLiteral("Dispositivo Vulkan:"), vk_group), 0, 0);
    m_combo_vk_device = new QComboBox(vk_group);
    if (m_vk_records.empty()) {
        m_combo_vk_device->addItem(QStringLiteral("GPU Predeterminada (Vulkan)"));
    } else {
        for (const auto& rec : m_vk_records) {
            m_combo_vk_device->addItem(QString::fromStdString(rec.name));
        }
    }
    vk_grid->addWidget(m_combo_vk_device, 0, 1);

    vk_grid->addWidget(new QLabel(QStringLiteral("Recompresión ASTC:"), vk_group), 0, 2);
    m_combo_astc_recompression = new QComboBox(vk_group);
    m_combo_astc_recompression->addItem(QStringLiteral("Sin comprimir (VRAM Alta)"), static_cast<u32>(Settings::AstcRecompression::Uncompressed));
    m_combo_astc_recompression->addItem(QStringLiteral("BC1 (VRAM Baja)"), static_cast<u32>(Settings::AstcRecompression::Bc1));
    m_combo_astc_recompression->addItem(QStringLiteral("BC3 (Recomendado)"), static_cast<u32>(Settings::AstcRecompression::Bc3));
    vk_grid->addWidget(m_combo_astc_recompression, 0, 3);

    vk_grid->addWidget(new QLabel(QStringLiteral("Uso de VRAM:"), vk_group), 1, 0);
    m_combo_vram_mode = new QComboBox(vk_group);
    m_combo_vram_mode->addItem(QStringLiteral("Conservador (Recomendado)"), static_cast<u32>(Settings::VramUsageMode::Conservative));
    m_combo_vram_mode->addItem(QStringLiteral("Agresivo"), static_cast<u32>(Settings::VramUsageMode::Aggressive));
    vk_grid->addWidget(m_combo_vram_mode, 1, 1);

    vk_grid->addWidget(new QLabel(QStringLiteral("Extended Dynamic State:"), vk_group), 1, 2);
    m_combo_extended_dynamic_state = new QComboBox(vk_group);
    m_combo_extended_dynamic_state->addItem(QStringLiteral("Desactivado"), static_cast<u32>(Settings::ExtendedDynamicState::Disabled));
    m_combo_extended_dynamic_state->addItem(QStringLiteral("EDS 1"), static_cast<u32>(Settings::ExtendedDynamicState::EDS1));
    m_combo_extended_dynamic_state->addItem(QStringLiteral("EDS 2"), static_cast<u32>(Settings::ExtendedDynamicState::EDS2));
    m_combo_extended_dynamic_state->addItem(QStringLiteral("EDS 3 (Vulkan 1.3)"), static_cast<u32>(Settings::ExtendedDynamicState::EDS3));
    vk_grid->addWidget(m_combo_extended_dynamic_state, 1, 3);

    m_check_async_gpu = new QCheckBox(QStringLiteral("GPU Asíncrona (Multihilo)"), vk_group);
    m_check_disk_shaders = new QCheckBox(QStringLiteral("Caché de Shaders en Disco"), vk_group);
    m_check_driver_cache = new QCheckBox(QStringLiteral("Caché de Pipelines del Driver"), vk_group);
    m_check_reactive_flushing = new QCheckBox(QStringLiteral("Flushing Reactivo"), vk_group);
    m_check_gpl =
        new QCheckBox(QStringLiteral("Librerías de Pipeline Vulkan (VK_EXT_gpl)"), vk_group);
    m_check_precompile_shaders =
        new QCheckBox(QStringLiteral("Precompilar Shaders al Iniciar"), vk_group);

    vk_grid->addWidget(m_check_async_gpu, 2, 0);
    vk_grid->addWidget(m_check_disk_shaders, 2, 1);
    vk_grid->addWidget(m_check_driver_cache, 2, 2);
    vk_grid->addWidget(m_check_reactive_flushing, 2, 3);
    vk_grid->addWidget(m_check_gpl, 3, 0, 1, 2);
    vk_grid->addWidget(m_check_precompile_shaders, 3, 2, 1, 2);

    layout->addWidget(vk_group);

    // CPU & Switch 2 Architecture
    auto* cpu_group = new QGroupBox(QStringLiteral("CPU Y MEMORIA DE CONSOLA"), widget);
    cpu_group->setStyleSheet(group_style);
    auto* cpu_grid = new QGridLayout(cpu_group);
    cpu_grid->setContentsMargins(18, 14, 18, 16);
    cpu_grid->setHorizontalSpacing(20);
    cpu_grid->setVerticalSpacing(14);

    cpu_grid->addWidget(new QLabel(QStringLiteral("Precisión de CPU:"), cpu_group), 0, 0);
    m_combo_cpu_accuracy = new QComboBox(cpu_group);
    m_combo_cpu_accuracy->addItem(QStringLiteral("Auto (Recomendado)"), static_cast<u32>(Settings::CpuAccuracy::Auto));
    m_combo_cpu_accuracy->addItem(QStringLiteral("Precisa (Accurate)"), static_cast<u32>(Settings::CpuAccuracy::Accurate));
    m_combo_cpu_accuracy->addItem(QStringLiteral("Rápida (Unsafe)"), static_cast<u32>(Settings::CpuAccuracy::Unsafe));
    cpu_grid->addWidget(m_combo_cpu_accuracy, 0, 1);

    cpu_grid->addWidget(new QLabel(QStringLiteral("Layout de Memoria:"), cpu_group), 0, 2);
    m_combo_memory_layout = new QComboBox(cpu_group);
    m_combo_memory_layout->addItem(QStringLiteral("4 GB (Switch Estándar)"), static_cast<u32>(Settings::MemoryLayout::Memory_4Gb));
    m_combo_memory_layout->addItem(QStringLiteral("6 GB (Mejorado)"), static_cast<u32>(Settings::MemoryLayout::Memory_6Gb));
    m_combo_memory_layout->addItem(QStringLiteral("8 GB (Pro)"), static_cast<u32>(Settings::MemoryLayout::Memory_8Gb));
    m_combo_memory_layout->addItem(QStringLiteral("10 GB (Extendido)"), static_cast<u32>(Settings::MemoryLayout::Memory_10Gb));
    m_combo_memory_layout->addItem(QStringLiteral("12 GB (Switch 2)"), static_cast<u32>(Settings::MemoryLayout::Memory_12Gb));
    cpu_grid->addWidget(m_combo_memory_layout, 0, 3);

    m_check_multi_core = new QCheckBox(QStringLiteral("Emulación CPU Multinúcleo"), cpu_group);
    cpu_grid->addWidget(m_check_multi_core, 1, 0, 1, 2);

    m_check_speed_limit = new QCheckBox(QStringLiteral("Limitar Velocidad al 100%"), cpu_group);
    cpu_grid->addWidget(m_check_speed_limit, 1, 2, 1, 2);

    layout->addWidget(cpu_group);

    // Maintenance
    m_btn_clear_shaders = new QPushButton(QStringLiteral("Purgar Caché Global de Shaders Vulkan"), widget);
    m_btn_clear_shaders->setCursor(Qt::PointingHandCursor);
    m_btn_clear_shaders->setStyleSheet(QStringLiteral(
        "QPushButton { background: #2D1922; color: %1; border: 1px solid #592337; border-radius: 8px; padding: 10px 18px; font-weight: bold; font-size: 12px; }"
        "QPushButton:hover { background: #452030; color: #FFFFFF; }")
        .arg(DaliaTheme::ColorStatusRed));
    connect(m_btn_clear_shaders, &QPushButton::clicked, this, &DaliaSettingsPage::ClearShaderCache);
    layout->addWidget(m_btn_clear_shaders);

    layout->addStretch();
    return widget;
}

void DaliaSettingsPage::ApplyProfile(int profile_index) {
    if (profile_index < 0) return;

    const int idx_12gb = m_combo_memory_layout->findData(static_cast<u32>(Settings::MemoryLayout::Memory_12Gb));

    switch (profile_index) {
    case 0: // Balanced Console
        m_combo_resolution->setCurrentIndex(2); // 1X Native
        m_combo_aspect->setCurrentIndex(0);
        m_combo_filter->setCurrentIndex(0);
        m_combo_aa->setCurrentIndex(1); // FXAA
        m_check_docked->setChecked(true);
        m_combo_astc_recompression->setCurrentIndex(2); // BC3
        m_combo_vram_mode->setCurrentIndex(0);
        m_combo_memory_layout->setCurrentIndex(idx_12gb);
        break;
    case 1: // Handheld
        m_combo_resolution->setCurrentIndex(2); // 1X
        m_combo_aspect->setCurrentIndex(0);
        m_combo_filter->setCurrentIndex(0);
        m_combo_aa->setCurrentIndex(0); // None
        m_check_docked->setChecked(false);
        m_combo_astc_recompression->setCurrentIndex(2);
        m_combo_vram_mode->setCurrentIndex(0);
        break;
    case 2: // High Performance
        m_combo_resolution->setCurrentIndex(4); // 2X 1440p/4K
        m_combo_aspect->setCurrentIndex(0);
        m_combo_filter->setCurrentIndex(3); // FSR
        m_slider_fsr_sharpness->setValue(40);
        m_combo_aa->setCurrentIndex(2); // SMAA
        m_check_docked->setChecked(true);
        m_combo_astc_recompression->setCurrentIndex(2); // BC3
        m_combo_vram_mode->setCurrentIndex(0);
        m_combo_memory_layout->setCurrentIndex(idx_12gb);
        m_combo_cpu_accuracy->setCurrentIndex(0);
        m_check_multi_core->setChecked(true);
        m_check_async_gpu->setChecked(true);
        break;
    case 3: // Ultra 4K
        m_combo_resolution->setCurrentIndex(6); // 4X UHD
        m_combo_aspect->setCurrentIndex(0);
        m_combo_filter->setCurrentIndex(3);
        m_combo_aa->setCurrentIndex(2);
        m_check_docked->setChecked(true);
        m_combo_astc_recompression->setCurrentIndex(2);
        m_combo_vram_mode->setCurrentIndex(0);
        m_combo_memory_layout->setCurrentIndex(idx_12gb);
        break;
    }
}

void DaliaSettingsPage::LoadSettings() {
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
    m_check_gpl->setChecked(true);
    m_check_precompile_shaders->setChecked(Settings::values.precache_shaders.GetValue() != Settings::PrecacheShaders::Off);

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

void DaliaSettingsPage::SaveSettings() {
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
    Settings::values.precache_shaders.SetValue(
        m_check_precompile_shaders->isChecked() ? Settings::PrecacheShaders::Always : Settings::PrecacheShaders::Off);

    Settings::values.cpu_accuracy.SetValue(
        static_cast<Settings::CpuAccuracy>(m_combo_cpu_accuracy->currentData().toUInt()));
    Settings::values.use_multi_core.SetValue(m_check_multi_core->isChecked());
    Settings::values.memory_layout_mode.SetValue(
        static_cast<Settings::MemoryLayout>(m_combo_memory_layout->currentData().toUInt()));
    Settings::values.use_speed_limit.SetValue(m_check_speed_limit->isChecked());

    if (QtCommon::system) {
        QtCommon::system->ApplySettings();
    }
    if (m_main_window) {
        m_main_window->OnSaveConfig();
        m_main_window->UpdateDockedButton();
        m_main_window->UpdateFilterText();
        m_main_window->UpdateAAText();
        m_main_window->UpdateVolumeUI();
    }
    Settings::LogSettings();
    UISettings::values.configuration_applied = true;

    m_status_feedback->setText(QStringLiteral("✓ Ajustes aplicados y guardados"));
    QTimer::singleShot(3000, m_status_feedback, [this] { m_status_feedback->clear(); });
}

void DaliaSettingsPage::RestoreDefaults() {
    ApplyProfile(2); // High Performance
    SaveSettings();
}

void DaliaSettingsPage::ClearShaderCache() {
    auto res = QMessageBox::question(
        this, tr("Purgar Caché Vulkan"),
        tr("Se eliminará la caché de shaders y pipelines en disco. ¿Continuar?"),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);

    if (res != QMessageBox::Yes) return;

    try {
        const auto shader_dir = Common::FS::GetEdenPath(Common::FS::EdenPath::ShaderDir);
        Common::FS::RemoveDirRecursively(shader_dir);
        m_status_feedback->setText(QStringLiteral("✓ Caché de shaders purgada"));
        QTimer::singleShot(3000, m_status_feedback, [this] { m_status_feedback->clear(); });
    } catch (const std::exception& e) {
        QMessageBox::warning(this, tr("Error"), tr("Error al purgar la caché: %1").arg(e.what()));
    }
}

void DaliaSettingsPage::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Left) {
        emit FocusSidebarRequested();
        event->accept();
        return;
    }
    QWidget::keyPressEvent(event);
}

void DaliaSettingsPage::RefreshVkRecords(const std::vector<VkDeviceInfo::Record>& vk_records) {
    m_vk_records.clear();
    m_vk_records.reserve(vk_records.size());
    for (const auto& rec : vk_records) {
        m_vk_records.emplace_back(rec);
    }
    if (!m_combo_vk_device) {
        return;
    }
    m_combo_vk_device->blockSignals(true);
    m_combo_vk_device->clear();
    if (m_vk_records.empty()) {
        m_combo_vk_device->addItem(QStringLiteral("GPU Predeterminada (Vulkan)"));
    } else {
        for (const auto& rec : m_vk_records) {
            m_combo_vk_device->addItem(QString::fromStdString(rec.name));
        }
    }
    const u32 dev_index = Settings::values.vulkan_device.GetValue();
    if (dev_index < static_cast<u32>(m_combo_vk_device->count())) {
        m_combo_vk_device->setCurrentIndex(dev_index);
    }
    m_combo_vk_device->blockSignals(false);
}
