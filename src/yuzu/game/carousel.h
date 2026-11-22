// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QElapsedTimer>
#include <QListView>
#include <QPersistentModelIndex>
#include <QPixmap>
#include <QString>

#include <array>
#include <vector>

#include "common/common_types.h"

class GameListModel;
class QVariantAnimation;

/// The home screen's carousel: one large centered game card with the neighbouring games stacked
/// behind it on both sides, each tilted and faded with its distance from the center. The base
/// stays QListView so the surrounding wiring (selection model, clicked/doubleClicked, context
/// menus, the transparency style sheet) keeps working; the layout, painting and navigation are
/// entirely this class's.
class GameCarousel : public QListView {
    Q_OBJECT

public:
    /// The centered card's share of the available view height, in the order the
    /// carousel_card_size setting stores them (full, three quarters, two thirds, half, a
    /// third). An out-of-range stored index reads as full.
    static constexpr std::array<qreal, 5> CardSizeFractions{1.0, 0.75, 2.0 / 3.0, 0.5, 1.0 / 3.0};

    explicit GameCarousel(QWidget* parent = nullptr);

    void SetModel(GameListModel* model);
    void ApplyFilter(const QString& edit_filter_text, GameListModel* model);
    /// Kept for the surrounding wiring: the card sizes follow the viewport, not this setting.
    void UpdateIconSize();

    /// The program whose game the backdrop should feature: the slide's destination once one is
    /// running, the settled center otherwise.
    u64 CenteredProgramId() const;

    QModelIndex indexAt(const QPoint& point) const override;
    QRect visualRect(const QModelIndex& index) const override;
    bool isIndexHidden(const QModelIndex& index) const override;

protected:
    void paintEvent(QPaintEvent* event) override;
    void scrollTo(const QModelIndex& index, ScrollHint hint) override;
    QModelIndex moveCursor(CursorAction cursorAction, Qt::KeyboardModifiers modifiers) override;
    void setSelection(const QRect& rect, QItemSelectionModel::SelectionFlags command) override;
    QRegion visualRegionForSelection(const QItemSelection& selection) const override;
    void wheelEvent(QWheelEvent* event) override;
    void currentChanged(const QModelIndex& current, const QModelIndex& previous) override;
    void updateGeometries() override;
    void scrollContentsBy(int dx, int dy) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    bool viewportEvent(QEvent* event) override;

private:
    int PositionOf(int row) const;
    /// The selected game's position, or the centered one's when nothing visible is selected.
    int CurrentPosition() const;
    /// Slides to `position`; a negative duration uses the default.
    void MoveTo(int position, int duration_ms = -1);
    /// Nudges the strip toward `direction` and back: the end of the list was reached.
    void Bounce(int direction);
    void UpdateHover(const QPoint& pos);
    /// The play button on the centered card, in viewport coordinates.
    QRectF PlayButtonRect() const;
    /// Re-applies the stored filter once the model has changed shape.
    void ScheduleRelayout();

    QString m_filter_text;
    /// The rows the filter left visible, left to right.
    std::vector<int> m_visible;
    /// The animated position within m_visible of the centered card.
    qreal m_position = 0.0;
    QVariantAnimation* m_position_anim = nullptr;
    /// The slide's destination position, or -1 when no slide is running.
    int m_pending_center = -1;
    bool m_relayout_queued = false;
    /// The game centered before the rows were removed, reselected once a reload brings it back.
    QString m_restore_path;
    bool m_populating = false;
    /// Wheel travel not yet turned into a step, in eighths of a degree.
    int m_wheel_accum = 0;

    // Mouse: a press becomes a drag past the start-drag distance, otherwise a click on release.
    bool m_pressed = false;
    bool m_dragging = false;
    QPoint m_press_pos;
    qreal m_drag_origin = 0.0;
    QPersistentModelIndex m_press_index;
    /// The card the last click landed on, so a double-click launches it even after it slid.
    QPersistentModelIndex m_last_click_index;
    QElapsedTimer m_drag_clock;
    struct DragSample {
        qint64 ms;
        qreal position;
    };
    std::vector<DragSample> m_drag_samples;
    int m_hover_position = -1;
    bool m_hover_play = false;

signals:
    /// The game the backdrop should feature: the slide's destination once one is running, the
    /// settled center otherwise. Emitted on selection changes and relayouts.
    void CenteredGameChanged(u64 program_id);
    void FilterResultReady(int visible, int total);
    /// The centered card's play button was clicked, or a card double-clicked.
    void LaunchRequested(const QModelIndex& index);
    /// A click, not a drag, that landed on no card.
    void BackgroundClicked();
};
