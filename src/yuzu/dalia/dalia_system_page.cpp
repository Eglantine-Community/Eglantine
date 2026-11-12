// SPDX-FileCopyrightText: Copyright 2026 DALIA Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <filesystem>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#endif

#include <QApplication>
#include <QClipboard>
#include <QDateTime>
#include <QDesktopServices>
#include <QFile>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QTabWidget>
#include <QTextEdit>
#include <QTextStream>
#include <QUrl>
#include <QVBoxLayout>

#include "common/cpu_features.h"
#include "common/fs/fs_paths.h"
#include "common/fs/path_util.h"
#include "common/settings.h"
#include "yuzu/dalia/dalia_system_page.h"
#include "yuzu/dalia/dalia_theme.h"
#include "yuzu/main_window.h"

DaliaSystemPage::DaliaSystemPage(MainWindow* main_window, const std::vector<VkDeviceInfo::Record>& vk_records, QWidget* parent)
    : QWidget{parent}, m_main_window{main_window}, m_vk_records{vk_records} {
    SetupUI();
    RefreshTelemetry();
    RefreshLogs();
}

DaliaSystemPage::~DaliaSystemPage() = default;

void DaliaSystemPage::SetupUI() {
    auto* main_layout = new QVBoxLayout(this);
    main_layout->setContentsMargins(28, 22, 28, 16);
    main_layout->setSpacing(14);

    // Header
    auto* header_layout = new QHBoxLayout();
    auto* title_box = new QVBoxLayout();
    title_box->setSpacing(2);

    auto* title_label = new QLabel(QStringLiteral("Sistema y Diagnóstico"), this);
    QFont title_font = title_label->font();
    title_font.setPointSize(20);
    title_font.setBold(true);
    title_label->setFont(title_font);
    title_label->setStyleSheet(QStringLiteral("color: %1; letter-spacing: 0.3px;").arg(DaliaTheme::ColorTextPrimary));

    auto* subtitle_label = new QLabel(QStringLiteral("Inspección de hardware en vivo, memoria Vulkan y registros del motor"), this);
    subtitle_label->setStyleSheet(QStringLiteral("color: %1; font-size: 13px; font-weight: 500;").arg(DaliaTheme::ColorTextSecondary));

    title_box->addWidget(title_label);
    title_box->addWidget(subtitle_label);
    header_layout->addLayout(title_box);
    header_layout->addStretch();

    auto* status_pill = new QLabel(QStringLiteral("● MOTOR VULKAN NOMINAL"), this);
    status_pill->setStyleSheet(QStringLiteral(
        "background-color: rgba(53, 232, 125, 0.15); color: %1; border: 1px solid rgba(53, 232, 125, 0.4); "
        "border-radius: 12px; padding: 6px 14px; font-weight: bold; font-size: 11px; letter-spacing: 1px;")
        .arg(DaliaTheme::ColorStatusGreen));
    header_layout->addWidget(status_pill);
    main_layout->addLayout(header_layout);

    // Tabs (Consolidated to 2 focused tabs)
    m_tab_widget = new QTabWidget(this);
    m_tab_widget->setStyleSheet(QStringLiteral(
        "QTabWidget::pane { border: 1px solid %1; background: %2; border-radius: 12px; }"
        "QTabBar::tab { background: %3; color: %4; font-weight: 600; padding: 10px 22px; "
        "border-top-left-radius: 8px; border-top-right-radius: 8px; margin-right: 4px; font-size: 12px; }"
        "QTabBar::tab:selected { background: %5; color: %6; border-bottom: 2px solid %6; }"
        "QTabBar::tab:hover:!selected { background: %7; color: #FFFFFF; }")
        .arg(DaliaTheme::ColorBorderCard, DaliaTheme::ColorSurfaceCard,
             DaliaTheme::ColorSurfaceSidebar, DaliaTheme::ColorTextSecondary,
             DaliaTheme::ColorSurfaceElevated, DaliaTheme::ColorAccentIceBlue,
             DaliaTheme::ColorSurfaceHover));

    m_tab_widget->addTab(CreateHardwareOverviewTab(), QStringLiteral("Hardware y Estado Vulkan"));
    m_tab_widget->addTab(CreateLogViewerTab(), QStringLiteral("Visor de Registros"));
    main_layout->addWidget(m_tab_widget, 1);

    // Footer actions
    auto* footer_layout = new QHBoxLayout();
    footer_layout->setSpacing(12);

    auto* btn_export = new QPushButton(QStringLiteral("Exportar Informe (.md)"), this);
    btn_export->setCursor(Qt::PointingHandCursor);
    btn_export->setStyleSheet(QStringLiteral(
        "QPushButton { background: %1; color: #101318; border-radius: 8px; padding: 8px 18px; font-weight: bold; border: none; font-size: 12px; }"
        "QPushButton:hover { background: #5EC4F0; }")
        .arg(DaliaTheme::ColorAccentIceBlue));
    connect(btn_export, &QPushButton::clicked, this, &DaliaSystemPage::ExportDiagnosticReport);
    footer_layout->addWidget(btn_export);

    auto* btn_copy = new QPushButton(QStringLiteral("Copiar Resumen"), this);
    btn_copy->setCursor(Qt::PointingHandCursor);
    btn_copy->setStyleSheet(QStringLiteral(
        "QPushButton { background: %1; color: %2; border-radius: 8px; padding: 8px 16px; font-weight: 600; border: 1px solid %3; font-size: 12px; }"
        "QPushButton:hover { background: %4; color: #FFFFFF; }")
        .arg(DaliaTheme::ColorSurfaceElevated, DaliaTheme::ColorTextSecondary,
             DaliaTheme::ColorBorderCard, DaliaTheme::ColorSurfaceHover));
    connect(btn_copy, &QPushButton::clicked, this, &DaliaSystemPage::CopySummaryToClipboard);
    footer_layout->addWidget(btn_copy);
    footer_layout->addStretch();

    main_layout->addLayout(footer_layout);
}

QWidget* DaliaSystemPage::CreateHardwareOverviewTab() {
    auto* widget = new QWidget(this);
    widget->setStyleSheet(QStringLiteral("background: transparent;"));
    auto* layout = new QVBoxLayout(widget);
    layout->setContentsMargins(24, 24, 24, 24);

    auto* grid = new QGridLayout();
    grid->setHorizontalSpacing(28);
    grid->setVerticalSpacing(16);

    auto make_row = [grid](int row, const QString& label) {
        auto* lbl = new QLabel(label);
        lbl->setStyleSheet(QStringLiteral("color: %1; font-size: 12px; font-weight: 500;").arg(DaliaTheme::ColorTextSecondary));
        grid->addWidget(lbl, row, 0);

        auto* val = new QLabel(QStringLiteral("..."));
        val->setStyleSheet(QStringLiteral("color: #FFFFFF; font-size: 13px; font-weight: bold;"));
        grid->addWidget(val, row, 1);
        return val;
    };

    m_val_cpu = make_row(0, QStringLiteral("Procesador CPU:"));
    m_val_ram = make_row(1, QStringLiteral("Memoria Física (RAM):"));
    m_val_gpu = make_row(2, QStringLiteral("Dispositivo Vulkan:"));
    m_val_vram_mode = make_row(3, QStringLiteral("Gestión de VRAM:"));
    m_val_astc = make_row(4, QStringLiteral("Recompresión ASTC:"));
    m_val_mem_layout = make_row(5, QStringLiteral("Layout de Memoria Consola:"));
    m_val_shader_cache = make_row(6, QStringLiteral("Caché de Shaders en Disco:"));

    layout->addLayout(grid);
    layout->addStretch();
    return widget;
}

void DaliaSystemPage::RefreshTelemetry() {
    QString cpu_name = QString::fromUtf8(Common::g_cpu_caps.cpu_string).trimmed();
    if (cpu_name.isEmpty()) cpu_name = QStringLiteral("CPU Host");
    const int threads = Common::GetProcessorCount().value_or(static_cast<int>(std::thread::hardware_concurrency()));
    m_val_cpu->setText(QStringLiteral("%1 (%2 hilos lógicos)").arg(cpu_name).arg(threads));

#ifdef _WIN32
    MEMORYSTATUSEX mem_status;
    mem_status.dwLength = sizeof(mem_status);
    if (GlobalMemoryStatusEx(&mem_status)) {
        const double total_gb = static_cast<double>(mem_status.ullTotalPhys) / (1024.0 * 1024.0 * 1024.0);
        const double avail_gb = static_cast<double>(mem_status.ullAvailPhys) / (1024.0 * 1024.0 * 1024.0);
        m_val_ram->setText(QStringLiteral("%1 GB Total (%2 GB Libres)")
                               .arg(QString::number(total_gb, 'f', 1), QString::number(avail_gb, 'f', 1)));
    }
#endif

    QString gpu_name = QStringLiteral("Adaptador Vulkan Predeterminado");
    const u32 dev_idx = Settings::values.vulkan_device.GetValue();
    if (dev_idx < m_vk_records.size()) {
        gpu_name = QString::fromStdString(m_vk_records[dev_idx].name);
    }
    m_val_gpu->setText(gpu_name);

    const auto vram_mode = Settings::values.vram_usage_mode.GetValue();
    m_val_vram_mode->setText(vram_mode == Settings::VramUsageMode::Conservative
                                 ? QStringLiteral("Conservador (Margen seguro anti Device-Lost)")
                                 : QStringLiteral("Agresivo"));

    switch (Settings::values.astc_recompression.GetValue()) {
    case Settings::AstcRecompression::Uncompressed:
        m_val_astc->setText(QStringLiteral("Sin comprimir (Nativo)"));
        break;
    case Settings::AstcRecompression::Bc1:
        m_val_astc->setText(QStringLiteral("BC1 (Compresión baja)"));
        break;
    case Settings::AstcRecompression::Bc3:
        m_val_astc->setText(QStringLiteral("BC3 (Óptimo)"));
        break;
    }

    switch (Settings::values.memory_layout_mode.GetValue()) {
    case Settings::MemoryLayout::Memory_4Gb:
        m_val_mem_layout->setText(QStringLiteral("4 GB (Switch Estándar)"));
        break;
    case Settings::MemoryLayout::Memory_6Gb:
        m_val_mem_layout->setText(QStringLiteral("6 GB"));
        break;
    case Settings::MemoryLayout::Memory_8Gb:
        m_val_mem_layout->setText(QStringLiteral("8 GB (Pro)"));
        break;
    case Settings::MemoryLayout::Memory_10Gb:
        m_val_mem_layout->setText(QStringLiteral("10 GB (Extendido)"));
        break;
    case Settings::MemoryLayout::Memory_12Gb:
        m_val_mem_layout->setText(QStringLiteral("12 GB (Nintendo Switch 2)"));
        break;
    }

    uint64_t cache_bytes = 0;
    try {
        const auto shader_dir = Common::FS::GetEdenPath(Common::FS::EdenPath::ShaderDir);
        if (std::filesystem::exists(shader_dir)) {
            for (const auto& entry : std::filesystem::recursive_directory_iterator(shader_dir)) {
                if (entry.is_regular_file()) {
                    cache_bytes += entry.file_size();
                }
            }
        }
    } catch (...) {}

    const double cache_mb = static_cast<double>(cache_bytes) / (1024.0 * 1024.0);
    m_val_shader_cache->setText(QStringLiteral("%1 MB almacenados en disco").arg(QString::number(cache_mb, 'f', 2)));
}

QWidget* DaliaSystemPage::CreateLogViewerTab() {
    auto* widget = new QWidget(this);
    widget->setStyleSheet(QStringLiteral("background: transparent;"));
    auto* layout = new QVBoxLayout(widget);
    layout->setContentsMargins(16, 16, 16, 16);
    layout->setSpacing(12);

    auto* filter_bar = new QHBoxLayout();
    filter_bar->setSpacing(10);

    filter_bar->addWidget(new QLabel(QStringLiteral("Filtrar:"), widget));

    m_filter_edit = new QLineEdit(widget);
    m_filter_edit->setPlaceholderText(QStringLiteral("Escribe para filtrar (Error, Vulkan, Shader...)"));
    m_filter_edit->setStyleSheet(QStringLiteral(
        "QLineEdit { background: %1; color: #FFFFFF; border: 1px solid %2; border-radius: 6px; padding: 6px 12px; font-size: 12px; }")
        .arg(DaliaTheme::ColorSurfaceElevated, DaliaTheme::ColorBorderCard));
    connect(m_filter_edit, &QLineEdit::textChanged, this, &DaliaSystemPage::FilterLogs);
    filter_bar->addWidget(m_filter_edit, 1);

    auto* btn_refresh = new QPushButton(QStringLiteral("Actualizar"), widget);
    btn_refresh->setCursor(Qt::PointingHandCursor);
    btn_refresh->setStyleSheet(QStringLiteral(
        "QPushButton { background: %1; color: #FFFFFF; border-radius: 6px; padding: 6px 14px; font-weight: 600; border: 1px solid %2; font-size: 12px; }")
        .arg(DaliaTheme::ColorSurfaceElevated, DaliaTheme::ColorBorderCard));
    connect(btn_refresh, &QPushButton::clicked, this, &DaliaSystemPage::RefreshLogs);
    filter_bar->addWidget(btn_refresh);

    layout->addLayout(filter_bar);

    m_log_edit = new QTextEdit(widget);
    m_log_edit->setReadOnly(true);
    m_log_edit->setStyleSheet(QStringLiteral(
        "QTextEdit { background: %1; color: #B4BAC6; font-family: 'Consolas', monospace; font-size: 11px; border: 1px solid %2; border-radius: 8px; padding: 10px; }")
        .arg(DaliaTheme::ColorBackgroundMain, DaliaTheme::ColorBorderCard));
    layout->addWidget(m_log_edit, 1);

    return widget;
}

void DaliaSystemPage::RefreshLogs() {
    try {
        const auto log_dir = Common::FS::GetEdenPath(Common::FS::EdenPath::LogDir);
        const auto log_file = log_dir / "eden_log.txt";

        QFile file(QString::fromStdString(Common::FS::PathToUTF8String(log_file)));
        if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
            QTextStream in(&file);
            QStringList lines;
            while (!in.atEnd()) {
                lines.append(in.readLine());
                if (lines.size() > 500) {
                    lines.removeFirst();
                }
            }
            m_raw_log_content = lines.join(QLatin1Char('\n'));
            FilterLogs(m_filter_edit ? m_filter_edit->text() : QString());
        } else {
            m_log_edit->setPlainText(QStringLiteral("[Diagnóstico] No se encontraron registros recientes."));
        }
    } catch (const std::exception& e) {
        m_log_edit->setPlainText(QStringLiteral("[Diagnóstico] Error al leer el registro: %1").arg(e.what()));
    }
}

void DaliaSystemPage::FilterLogs(const QString& filter_text) {
    if (filter_text.trimmed().isEmpty()) {
        m_log_edit->setPlainText(m_raw_log_content);
    } else {
        const QStringList all_lines = m_raw_log_content.split(QLatin1Char('\n'));
        QStringList matched;
        for (const QString& line : all_lines) {
            if (line.contains(filter_text, Qt::CaseInsensitive)) {
                matched.append(line);
            }
        }
        m_log_edit->setPlainText(matched.join(QLatin1Char('\n')));
    }
    m_log_edit->moveCursor(QTextCursor::End);
}

void DaliaSystemPage::ExportDiagnosticReport() {
    const QString timestamp = QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_hhmmss"));
    const auto log_dir = Common::FS::GetEdenPath(Common::FS::EdenPath::LogDir);
    const QString report_path = QString::fromStdString(Common::FS::PathToUTF8String(log_dir / ("dalia_report_" + timestamp.toStdString() + ".md")));

    QFile file(report_path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QMessageBox::warning(this, tr("Error"), tr("No se pudo crear el archivo de informe."));
        return;
    }

    QTextStream out(&file);
    out << "# INFORME DE DIAGNÓSTICO — DALIA\n\n";
    out << "- **Fecha:** " << QDateTime::currentDateTime().toString() << "\n";
    out << "- **CPU:** " << m_val_cpu->text() << "\n";
    out << "- **RAM:** " << m_val_ram->text() << "\n";
    out << "- **GPU:** " << m_val_gpu->text() << "\n";
    out << "- **Gestión VRAM:** " << m_val_vram_mode->text() << "\n";
    out << "- **ASTC:** " << m_val_astc->text() << "\n";
    out << "- **Memoria:** " << m_val_mem_layout->text() << "\n";
    out << "- **Shader Cache:** " << m_val_shader_cache->text() << "\n\n";
    out << "## Registros Recientes\n```text\n";
    out << m_raw_log_content.right(4000);
    out << "\n```\n";
    file.close();

    QDesktopServices::openUrl(QUrl::fromLocalFile(report_path));
}

void DaliaSystemPage::CopySummaryToClipboard() {
    const QString summary = QStringLiteral(
        "DALIA Resumen:\n"
        "• CPU: %1\n"
        "• GPU: %2\n"
        "• ASTC: %3\n"
        "• Memoria: %4\n")
        .arg(m_val_cpu->text(), m_val_gpu->text(), m_val_astc->text(), m_val_mem_layout->text());

    QApplication::clipboard()->setText(summary);
}

void DaliaSystemPage::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Left) {
        emit FocusSidebarRequested();
        event->accept();
        return;
    }
    QWidget::keyPressEvent(event);
}

void DaliaSystemPage::RefreshVkRecords(const std::vector<VkDeviceInfo::Record>& vk_records) {
    m_vk_records.clear();
    m_vk_records.reserve(vk_records.size());
    for (const auto& rec : vk_records) {
        m_vk_records.emplace_back(rec);
    }
    RefreshTelemetry();
}
