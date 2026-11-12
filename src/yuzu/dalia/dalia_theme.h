// SPDX-FileCopyrightText: Copyright 2026 DALIA Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QColor>
#include <QString>

namespace DaliaTheme {

// Master Color Palette
constexpr auto ColorBackgroundMain    = "#13161D"; // Deep graphite window background
constexpr auto ColorSurfaceSidebar    = "#161922"; // Left sidebar surface
constexpr auto ColorSurfaceCard       = "#1B2029"; // Card and container surface
constexpr auto ColorSurfaceElevated   = "#242A35"; // Elevated panels / active item background
constexpr auto ColorSurfaceHover      = "#2C3340"; // Hover state on surfaces
constexpr auto ColorSurfaceHighlight  = "#303744"; // Focused surface

// Accent Colors
constexpr auto ColorAccentCoral       = "#E85C83"; // Primary brand coral accent
constexpr auto ColorAccentIceBlue     = "#48BDEB"; // Secondary ice-blue neon accent

// Functional State Colors
constexpr auto ColorStatusGreen       = "#35E87D"; // Online / Success / High FPS
constexpr auto ColorStatusAmber       = "#FFB703"; // Warning / Compiling Shaders
constexpr auto ColorStatusRed         = "#FF4D6D"; // Error / Low FPS / Stop

// Typography Colors
constexpr auto ColorTextPrimary       = "#FFFFFF"; // Crisp primary text
constexpr auto ColorTextSecondary     = "#8E9297"; // Muted label text
constexpr auto ColorTextTertiary      = "#5C6270"; // Subtle info text

// Border Colors
constexpr auto ColorBorderSubtle      = "#242A35"; // Panel dividers
constexpr auto ColorBorderCard        = "#282C37"; // Resting card border
constexpr auto ColorBorderFocused     = "#48BDEB"; // Active focus ring

inline QString GetSidebarStyle() {
    return QStringLiteral(
        "QFrame#DaliaSidebar { background-color: %1; border-right: 1px solid %2; }")
        .arg(ColorSurfaceSidebar, ColorBorderSubtle);
}

inline QString GetSidebarButtonStyle(bool active) {
    if (active) {
        return QStringLiteral(
            "QPushButton {"
            "  background-color: %1;"
            "  color: %2;"
            "  border: none;"
            "  border-left: 3px solid %3;"
            "  border-radius: 0px 8px 8px 0px;"
            "  padding: 10px 16px;"
            "  font-size: 13px;"
            "  font-weight: bold;"
            "  text-align: left;"
            "}")
            .arg(ColorSurfaceElevated, ColorTextPrimary, ColorAccentCoral);
    } else {
        return QStringLiteral(
            "QPushButton {"
            "  background-color: transparent;"
            "  color: %1;"
            "  border: none;"
            "  border-left: 3px solid transparent;"
            "  border-radius: 0px 8px 8px 0px;"
            "  padding: 10px 16px;"
            "  font-size: 13px;"
            "  font-weight: 500;"
            "  text-align: left;"
            "}"
            "QPushButton:hover {"
            "  background-color: %2;"
            "  color: %3;"
            "}")
            .arg(ColorTextSecondary, ColorSurfaceCard, ColorTextPrimary);
    }
}

inline QString GetCardStyle() {
    return QStringLiteral(
        "QFrame { background-color: %1; border: 1px solid %2; border-radius: 14px; }")
        .arg(ColorSurfaceCard, ColorBorderCard);
}

inline QString GetGlobalControlStyle() {
    return QStringLiteral(
        "QScrollArea { background: transparent; border: none; }"
        "QScrollBar:vertical {"
        "  background: %1;"
        "  width: 8px;"
        "  border-radius: 4px;"
        "  margin: 0px;"
        "}"
        "QScrollBar::handle:vertical {"
        "  background: %2;"
        "  min-height: 24px;"
        "  border-radius: 4px;"
        "}"
        "QScrollBar::handle:vertical:hover {"
        "  background: %3;"
        "}"
        "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0px; }"
        "QScrollBar:horizontal {"
        "  background: %1;"
        "  height: 8px;"
        "  border-radius: 4px;"
        "  margin: 0px;"
        "}"
        "QScrollBar::handle:horizontal {"
        "  background: %2;"
        "  min-width: 24px;"
        "  border-radius: 4px;"
        "}"
        "QScrollBar::handle:horizontal:hover {"
        "  background: %3;"
        "}"
        "QScrollBar::add-line:horizontal, QScrollBar::sub-line:horizontal { width: 0px; }"
        "QComboBox {"
        "  background-color: %4;"
        "  color: #FFFFFF;"
        "  border: 1px solid %5;"
        "  border-radius: 8px;"
        "  padding: 6px 12px;"
        "  font-size: 12px;"
        "  font-weight: 500;"
        "}"
        "QComboBox:hover {"
        "  border-color: %6;"
        "}"
        "QComboBox::drop-down {"
        "  border: none;"
        "  subcontrol-origin: padding;"
        "  subcontrol-position: top right;"
        "  width: 24px;"
        "}"
        "QComboBox QAbstractItemView {"
        "  background-color: %7;"
        "  color: #FFFFFF;"
        "  selection-background-color: %4;"
        "  selection-color: %6;"
        "  border: 1px solid %5;"
        "  border-radius: 8px;"
        "  padding: 4px;"
        "  outline: none;"
        "}"
        "QCheckBox {"
        "  color: #E2E6EE;"
        "  font-size: 12px;"
        "  font-weight: 500;"
        "  spacing: 8px;"
        "}"
        "QCheckBox::indicator {"
        "  width: 18px;"
        "  height: 18px;"
        "  border-radius: 5px;"
        "  border: 1px solid %5;"
        "  background-color: %4;"
        "}"
        "QCheckBox::indicator:hover {"
        "  border-color: %6;"
        "}"
        "QCheckBox::indicator:checked {"
        "  background-color: %6;"
        "  border-color: %6;"
        "  image: none;"
        "}"
        "QSlider::groove:horizontal {"
        "  height: 6px;"
        "  background: %4;"
        "  border-radius: 3px;"
        "}"
        "QSlider::sub-page:horizontal {"
        "  background: %6;"
        "  border-radius: 3px;"
        "}"
        "QSlider::handle:horizontal {"
        "  background: #FFFFFF;"
        "  width: 16px;"
        "  margin-top: -5px;"
        "  margin-bottom: -5px;"
        "  border-radius: 8px;"
        "}"
        "QSlider::handle:horizontal:hover {"
        "  background: %8;"
        "}")
        .arg(ColorBackgroundMain, ColorSurfaceElevated, ColorSurfaceHover,
             ColorSurfaceElevated, ColorBorderCard, ColorAccentIceBlue,
             ColorSurfaceCard, ColorAccentCoral);
}

} // namespace DaliaTheme
