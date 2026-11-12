// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>
#include "game_card.h"
#include "qt_common/config/uisettings.h"

GameCard::GameCard(QObject* parent) : QStyledItemDelegate{parent} {
    setObjectName("GameCard");
}

void GameCard::paint(QPainter* painter, const QStyleOptionViewItem& option,
                     const QModelIndex& index) const {
    if (!index.isValid())
        return;

    painter->save();
    painter->setRenderHint(QPainter::Antialiasing, true);
    painter->setRenderHint(QPainter::SmoothPixmapTransform, true);

    const QRect cardRect = getCardRect(option, index);
    const u32 icon_size = UISettings::values.game_icon_size.GetValue();
    QPixmap icon_pixmap = index.data(Qt::DecorationRole).value<QPixmap>();

    const bool isSelected = (option.state & QStyle::State_Selected) != 0;
    const bool isHovered = (option.state & QStyle::State_MouseOver) != 0;
    const bool isDark = UISettings::IsDarkTheme();

    // Corner radius for Switch / Dalia card (matching modern rounded console cards)
    const int tileCornerRadius = 14;

    // Calculate tile rect (square centered horizontally in cardRect)
    int display_width = static_cast<int>(icon_size);
    int display_height = static_cast<int>(icon_size);
    if (!icon_pixmap.isNull()) {
        QSize scaled = icon_pixmap.size();
        scaled.scale(icon_size, icon_size, Qt::KeepAspectRatio);
        display_width = scaled.width();
        display_height = scaled.height();
    }

    const int tile_x = cardRect.left() + (cardRect.width() - display_width) / 2;
    const int tile_y = cardRect.top() + cardMargin;
    const QRect tileRect(tile_x, tile_y, display_width, display_height);

    // 1. Draw base tile background (subtle light gray in light mode, dark graphite in dark mode)
    const QColor baseTileCol = isDark ? QColor(27, 32, 41) : QColor(228, 228, 228);
    painter->setPen(Qt::NoPen);
    painter->setBrush(baseTileCol);
    painter->drawRoundedRect(tileRect, tileCornerRadius, tileCornerRadius);

    // 2. Draw Game Icon with rounded corners
    if (!icon_pixmap.isNull()) {
        painter->save();
        QPainterPath clip_path;
        clip_path.addRoundedRect(tileRect, tileCornerRadius, tileCornerRadius);
        painter->setClipPath(clip_path);
        painter->drawPixmap(tileRect, icon_pixmap);
        painter->restore();
    } else {
        // Draw Nintendo Switch cartridge / Joy-Con placeholder
        painter->save();
        QPainterPath clip_path;
        clip_path.addRoundedRect(tileRect, tileCornerRadius, tileCornerRadius);
        painter->setClipPath(clip_path);

        QLinearGradient grad(tileRect.topLeft(), tileRect.bottomRight());
        grad.setColorAt(0.0, isDark ? QColor(48, 50, 54) : QColor(235, 235, 235));
        grad.setColorAt(1.0, isDark ? QColor(26, 28, 30) : QColor(215, 215, 215));
        painter->fillRect(tileRect, grad);

        // Stylized Switch Joy-Con accents in center
        const int barW = std::max(4, display_width / 16);
        const int barH = std::max(16, display_height / 3);
        const int cy = tileRect.center().y() - barH / 2;
        const int cx = tileRect.center().x();

        // Left Joy-Con Neon Red bar
        painter->setBrush(QColor(255, 60, 40));
        painter->setPen(Qt::NoPen);
        painter->drawRoundedRect(QRect(cx - barW - 3, cy, barW, barH), barW / 2, barW / 2);

        // Right Joy-Con Neon Blue bar
        painter->setBrush(QColor(10, 185, 230));
        painter->drawRoundedRect(QRect(cx + 3, cy, barW, barH), barW / 2, barW / 2);

        painter->restore();
    }

    // 3. Draw focus / selection / hover border around the tile (Dalia Coral and Ice-Blue theme)
    if (isSelected) {
        // Outer glow (DALIA Coral focus aura)
        painter->setBrush(Qt::NoBrush);
        painter->setPen(QPen(QColor(232, 92, 131, 85), 5.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        painter->drawRoundedRect(tileRect.adjusted(-3, -3, 3, 3), tileCornerRadius + 3, tileCornerRadius + 3);

        // Main Coral focus ring
        painter->setPen(QPen(QColor(232, 92, 131), 3.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        painter->drawRoundedRect(tileRect.adjusted(-1, -1, 1, 1), tileCornerRadius + 1, tileCornerRadius + 1);

        // Inner white highlight rim
        painter->setPen(QPen(QColor(255, 255, 255, 60), 1.0));
        painter->drawRoundedRect(tileRect, tileCornerRadius, tileCornerRadius);
    } else if (isHovered) {
        // Mouse hover outline (Ice-blue accent)
        painter->setBrush(Qt::NoBrush);
        painter->setPen(QPen(QColor(72, 189, 235, 180), 2.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        painter->drawRoundedRect(tileRect.adjusted(-1, -1, 1, 1), tileCornerRadius + 1, tileCornerRadius + 1);
    } else {
        // Subtle resting border (soft light gray in light mode, deep slate in dark mode)
        const QColor restBorder = isDark ? QColor(40, 44, 55) : QColor(212, 212, 212);
        painter->setBrush(Qt::NoBrush);
        painter->setPen(QPen(restBorder, 1.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        painter->drawRoundedRect(tileRect, tileCornerRadius, tileCornerRadius);
    }

    // 4. Draw Game Title cleanly underneath the cover art
    if (UISettings::values.show_game_name.GetValue()) {
        QRect textRect = cardRect;
        textRect.setTop(tileRect.bottom() + 8);
        textRect.setLeft(cardRect.left() + 2);
        textRect.setRight(cardRect.right() - 2);

        QString title = index.data(Qt::DisplayRole).toString().split(QLatin1Char('\n')).first();

        QFont font = option.font;
        font.setBold(true);
        const int fontSize = std::clamp(static_cast<int>(std::sqrt(static_cast<double>(icon_size)) * 1.05), 11, 15);
        font.setPixelSize(fontSize);
        font.setStyleHint(QFont::SansSerif);
        painter->setFont(font);

        if (isSelected) {
            // Text shadow for high contrast
            painter->setPen(QColor(0, 0, 0, 220));
            painter->drawText(textRect.adjusted(1, 1, 1, 1), Qt::AlignHCenter | Qt::AlignTop | Qt::TextWordWrap, title);
            painter->setPen(QColor(255, 255, 255));
        } else if (isHovered) {
            painter->setPen(QColor(230, 235, 240));
        } else {
            painter->setPen(QColor(165, 170, 178));
        }

        painter->drawText(textRect, Qt::AlignHCenter | Qt::AlignTop | Qt::TextWordWrap, title);
    }

    painter->restore();
}

QRect GameCard::getCardRect(const QStyleOptionViewItem& option, const QModelIndex& index) const {
    const int cell_width = option.rect.width();

    const int card_width = cell_width - m_padding;

    int card_left, card_top, card_height;

    if (m_columns >= 1) {
        // grid mode

        // center everything in-line, such that the leftmost and rightmost cards
        // have ~ equal padding to the edge of the viewport
        // spacing between each card is larger, but equal to each other
        const int column = index.row() % m_columns;

        const int row_width = m_columns * cell_width;
        const int total_gap = row_width - cardMargin * 2 - m_columns * card_width;
        const int gap = (m_columns > 1) ? (total_gap / (m_columns - 1)) : 0;
        card_left =
            option.rect.left() - column * cell_width + cardMargin + column * (card_width + gap) + 4;

        // fill cell vertically
        card_top = option.rect.top() + cardMargin;
        card_height = option.rect.height() - cardMargin - 1;
    } else {
        // carousel mode
        card_left = option.rect.left() + cardMargin + 4;

        // the delegate itself takes up the full height, but the card itself
        // gets centered
        const int content_height = m_contentSize.height() - cardMargin;
        const int cell_height = option.rect.height();

        card_height = std::min(content_height, cell_height - cardMargin * 2) - 1;
        card_top = option.rect.top() + (cell_height - card_height) / 2;
    }

    return QRect(card_left, card_top, card_width - cardMargin, card_height);
}

bool GameCard::hitTest(const QPoint& point, const QModelIndex& index, const QWidget* widget,
                       const QRect& cellRect) const {
    QStyleOptionViewItem option;
    option.initFrom(widget);
    option.rect = cellRect;
    return getCardRect(option, index).contains(point);
}

QSize GameCard::sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const {
    return m_size;
}

void GameCard::setSize(const QSize& newSize, const QSize& contentSize, const int padding,
                       const int columns) {
    m_size = newSize;
    m_contentSize = contentSize;
    m_padding = padding;
    m_columns = columns;
}
