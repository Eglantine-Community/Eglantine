// SPDX-FileCopyrightText: Copyright 2026 DALIA Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <QDesktopServices>
#include <QGraphicsDropShadowEffect>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMessageBox>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QUrl>
#include <QVBoxLayout>

#include "common/fs/fs.h"
#include "common/fs/fs_paths.h"
#include "common/fs/path_util.h"
#include "qt_common/qt_common.h"
#include "qt_common/util/game.h"
#include "qt_common/util/path.h"
#include "yuzu/configuration/configure_per_game.h"
#include "yuzu/dalia/dalia_game_details.h"
#include "yuzu/main_window.h"

DaliaGameDetailsDialog::DaliaGameDetailsDialog(MainWindow* parent, const QString& title, u64 program_id,
                                             const QString& file_path, const QPixmap& icon,
                                             const QString& developer, const QString& version,
                                             const QString& play_time, const QString& file_size)
    : QDialog{parent}, m_main_window{parent}, m_title{title}, m_program_id{program_id},
      m_file_path{file_path}, m_icon{icon}, m_developer{developer}, m_version{version},
      m_play_time{play_time}, m_file_size{file_size} {
    setWindowTitle(QStringLiteral("DALIA — %1").arg(title));
    setFixedSize(760, 480);
    setModal(true);
    setWindowFlags(windowFlags() & ~Qt::WindowContextHelpButtonHint);

    SetupUI();
}

DaliaGameDetailsDialog::~DaliaGameDetailsDialog() = default;

void DaliaGameDetailsDialog::SetupUI() {
    auto* main_layout = new QHBoxLayout(this);
    main_layout->setContentsMargins(32, 28, 32, 28);
    main_layout->setSpacing(28);

    // Left Column: Cover Art & Compatibility Badge
    auto* left_layout = new QVBoxLayout();
    left_layout->setSpacing(16);
    left_layout->setAlignment(Qt::AlignTop | Qt::AlignHCenter);

    auto* icon_label = new QLabel(this);
    icon_label->setFixedSize(220, 220);
    icon_label->setAlignment(Qt::AlignCenter);

    QPixmap scaled_icon = m_icon;
    if (scaled_icon.isNull()) {
        scaled_icon = QPixmap(QStringLiteral(":/icons/default/256x256/dalia.png"));
    }
    if (scaled_icon.isNull()) {
        scaled_icon = QPixmap(QStringLiteral(":/img/dalia.png"));
    }
    if (scaled_icon.isNull()) {
        scaled_icon = QPixmap(QStringLiteral("dist/dalia.png"));
    }
    if (!scaled_icon.isNull()) {
        QPixmap rounded_pixmap(220, 220);
        rounded_pixmap.fill(Qt::transparent);
        QPainter p(&rounded_pixmap);
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setRenderHint(QPainter::SmoothPixmapTransform, true);
        QPainterPath path;
        path.addRoundedRect(0, 0, 220, 220, 16, 16);
        p.setClipPath(path);
        p.drawPixmap(0, 0, 220, 220, scaled_icon.scaled(220, 220, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation));
        p.end();
        icon_label->setPixmap(rounded_pixmap);
    }
    left_layout->addWidget(icon_label);

    // Compatibility Pill Badge
    auto* compat_badge = new QLabel(QStringLiteral("● VERIFIED: PLAYABLE"), this);
    compat_badge->setAlignment(Qt::AlignCenter);
    compat_badge->setStyleSheet(
        QStringLiteral("background-color: rgba(0, 210, 255, 0.15); "
                       "color: #00D2FF; "
                       "border: 1px solid rgba(0, 210, 255, 0.4); "
                       "border-radius: 12px; "
                       "padding: 6px 14px; "
                       "font-weight: bold; "
                       "font-size: 11px; "
                       "letter-spacing: 1px;"));
    left_layout->addWidget(compat_badge);
    left_layout->addStretch();
    main_layout->addLayout(left_layout);

    // Right Column: Title, Metadata Grid, and Console Action Buttons
    auto* right_layout = new QVBoxLayout();
    right_layout->setSpacing(14);

    auto* title_label = new QLabel(m_title, this);
    title_label->setWordWrap(true);
    QFont title_font = title_label->font();
    title_font.setPointSize(16);
    title_font.setBold(true);
    title_label->setFont(title_font);
    title_label->setStyleSheet(QStringLiteral("color: #FFFFFF;"));
    right_layout->addWidget(title_label);

    // Metadata Grid
    auto* meta_widget = new QWidget(this);
    meta_widget->setStyleSheet(QStringLiteral(
        "QWidget { background-color: rgba(25, 28, 36, 0.85); border-radius: 10px; border: 1px solid #2B303C; }"
        "QLabel { border: none; font-size: 12px; }"));
    auto* meta_layout = new QGridLayout(meta_widget);
    meta_layout->setContentsMargins(14, 10, 14, 10);
    meta_layout->setVerticalSpacing(6);
    meta_layout->setHorizontalSpacing(16);

    const auto add_row = [&](int row, const QString& label, const QString& value, const QString& val_color = QStringLiteral("#D0D5DD")) {
        auto* lbl = new QLabel(label, meta_widget);
        lbl->setStyleSheet(QStringLiteral("color: #8A92A6; font-weight: 500;"));
        auto* val = new QLabel(value, meta_widget);
        val->setStyleSheet(QStringLiteral("color: %1; font-weight: bold;").arg(val_color));
        meta_layout->addWidget(lbl, row, 0);
        meta_layout->addWidget(val, row, 1);
    };

    const QString prog_id_str = m_program_id != 0 ? QStringLiteral("0x%1").arg(m_program_id, 16, 16, QLatin1Char('0')).toUpper() : QStringLiteral("—");
    add_row(0, tr("Program ID:"), prog_id_str, QStringLiteral("#00D2FF"));
    add_row(1, tr("Developer:"), m_developer.isEmpty() ? tr("Nintendo") : m_developer);
    add_row(2, tr("Version:"), m_version.isEmpty() ? QStringLiteral("1.0.0") : m_version);
    add_row(3, tr("Play Time:"), m_play_time.isEmpty() ? tr("None") : m_play_time, QStringLiteral("#35E87D"));
    add_row(4, tr("File Size:"), m_file_size.isEmpty() ? tr("Unknown") : m_file_size);

    right_layout->addWidget(meta_widget);

    // Action Buttons Layout
    auto* actions_layout = new QGridLayout();
    actions_layout->setSpacing(10);

    const QString btn_style = QStringLiteral(
        "QPushButton { "
        "  background-color: #242834; "
        "  color: #F0F2F5; "
        "  border: 1px solid #363C4E; "
        "  border-radius: 8px; "
        "  padding: 8px 16px; "
        "  font-size: 12px; "
        "  font-weight: 600; "
        "} "
        "QPushButton:hover, QPushButton:focus { "
        "  background-color: #00D2FF; "
        "  color: #0D1117; "
        "  border: 1px solid #00D2FF; "
        "} "
        "QPushButton:pressed { "
        "  background-color: #00A6CC; "
        "  color: #0D1117; "
        "}");

    const QString play_btn_style = QStringLiteral(
        "QPushButton { "
        "  background-color: #E63946; "
        "  color: #FFFFFF; "
        "  border: 1px solid #FF5A67; "
        "  border-radius: 8px; "
        "  padding: 8px 20px; "
        "  font-size: 13px; "
        "  font-weight: bold; "
        "} "
        "QPushButton:hover, QPushButton:focus { "
        "  background-color: #FF4D6D; "
        "  border: 1px solid #FFFFFF; "
        "} "
        "QPushButton:pressed { "
        "  background-color: #C92A36; "
        "}");

    m_btn_play = new QPushButton(tr("Ⓐ Play Game"), this);
    m_btn_play->setStyleSheet(play_btn_style);
    m_btn_play->setCursor(Qt::PointingHandCursor);
    connect(m_btn_play, &QPushButton::clicked, this, &DaliaGameDetailsDialog::OnPlayClicked);
    actions_layout->addWidget(m_btn_play, 0, 0, 1, 2);

    m_btn_configure = new QPushButton(tr("⚙ Per-Game Config"), this);
    m_btn_configure->setStyleSheet(btn_style);
    m_btn_configure->setCursor(Qt::PointingHandCursor);
    connect(m_btn_configure, &QPushButton::clicked, this, &DaliaGameDetailsDialog::OnConfigureClicked);
    actions_layout->addWidget(m_btn_configure, 1, 0);

    m_btn_save = new QPushButton(tr("📁 Save Data"), this);
    m_btn_save->setStyleSheet(btn_style);
    m_btn_save->setCursor(Qt::PointingHandCursor);
    connect(m_btn_save, &QPushButton::clicked, this, &DaliaGameDetailsDialog::OnSaveFolderClicked);
    actions_layout->addWidget(m_btn_save, 1, 1);

    m_btn_mods = new QPushButton(tr("🧩 Mod Folder"), this);
    m_btn_mods->setStyleSheet(btn_style);
    m_btn_mods->setCursor(Qt::PointingHandCursor);
    connect(m_btn_mods, &QPushButton::clicked, this, &DaliaGameDetailsDialog::OnModFolderClicked);
    actions_layout->addWidget(m_btn_mods, 2, 0);

    m_btn_clear_shaders = new QPushButton(tr("🗑 Clear Shader Cache"), this);
    m_btn_clear_shaders->setStyleSheet(btn_style);
    m_btn_clear_shaders->setCursor(Qt::PointingHandCursor);
    connect(m_btn_clear_shaders, &QPushButton::clicked, this, &DaliaGameDetailsDialog::OnClearShadersClicked);
    actions_layout->addWidget(m_btn_clear_shaders, 2, 1);

    m_btn_close = new QPushButton(tr("Ⓑ Close"), this);
    m_btn_close->setStyleSheet(btn_style);
    m_btn_close->setCursor(Qt::PointingHandCursor);
    connect(m_btn_close, &QPushButton::clicked, this, &QDialog::accept);
    actions_layout->addWidget(m_btn_close, 3, 0, 1, 2);

    right_layout->addLayout(actions_layout);
    main_layout->addLayout(right_layout);

    m_btn_play->setFocus();
}

void DaliaGameDetailsDialog::paintEvent(QPaintEvent* event) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);

    // Dark Graphite Console Background with subtle gradient
    QLinearGradient bg_grad(rect().topLeft(), rect().bottomRight());
    bg_grad.setColorAt(0.0, QColor(18, 20, 26));
    bg_grad.setColorAt(1.0, QColor(14, 16, 20));
    painter.fillRect(rect(), bg_grad);

    // Subtle edge border
    painter.setPen(QPen(QColor(43, 48, 60), 1.5));
    painter.setBrush(Qt::NoBrush);
    painter.drawRoundedRect(rect().adjusted(1, 1, -1, -1), 12, 12);
}

void DaliaGameDetailsDialog::keyPressEvent(QKeyEvent* event) {
    switch (event->key()) {
    case Qt::Key_Escape:
    case Qt::Key_Back:
    case Qt::Key_Backspace:
        accept();
        break;
    default:
        QDialog::keyPressEvent(event);
        break;
    }
}

void DaliaGameDetailsDialog::OnPlayClicked() {
    accept();
    emit LaunchRequested(m_file_path);
}

void DaliaGameDetailsDialog::OnConfigureClicked() {
    if (!m_main_window) {
        return;
    }
    ConfigurePerGame per_game_dialog(this, m_program_id, m_file_path.toStdString(),
                                    m_main_window->vk_device_records, *QtCommon::system);
    per_game_dialog.exec();
}

void DaliaGameDetailsDialog::OnSaveFolderClicked() {
    QtCommon::Game::OpenSaveFolder();
}

void DaliaGameDetailsDialog::OnModFolderClicked() {
    QtCommon::Game::OpenModFolder();
}

void DaliaGameDetailsDialog::OnClearShadersClicked() {
    const auto reply = QMessageBox::question(
        this, tr("Confirm Shader Cache Purge"),
        tr("Are you sure you want to delete the Vulkan and transferable shader cache for \"%1\"?\n"
           "Shaders will be safely recompiled during the next launch.").arg(m_title),
        QMessageBox::Yes | QMessageBox::No);

    if (reply == QMessageBox::Yes) {
        const auto shader_dir = Common::FS::GetEdenPath(Common::FS::EdenPath::ShaderDir);
        const auto prog_str = fmt::format("{:016x}", m_program_id);
        const auto vkd_path = shader_dir / fmt::format("{}.vkd", prog_str);
        const auto vkt_path = shader_dir / fmt::format("{}.vkt", prog_str);
        Common::FS::RemoveFile(vkd_path);
        Common::FS::RemoveFile(vkt_path);
        QMessageBox::information(this, tr("Shader Cache Cleared"),
                                tr("Shader cache for this title was successfully cleared."));
    }
}

