// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>

#include <QStringList>
#include <QWidget>

#include "yuzu/game/game_details_info.h"

class AddOnList;
class GameArtwork;
class QFormLayout;
class QFrame;
class QLabel;
class QMenu;
class QPushButton;
class QScrollArea;
class QStackedWidget;
class QToolButton;
class QVariantAnimation;

/// The row of action tiles under the carousel's centered card, and the panel a tile opens
/// beneath it. Always shows the centered game: nothing opens over the carousel. Down from the
/// cards enters the row, Left and Right move along it, Enter or A activates, Up or Escape/B
/// goes back. A panel follows the centered game while it is open.
class CarouselHub : public QWidget {
    Q_OBJECT

public:
    enum class Panel { None, AddOns, Shaders, Info };

    explicit CarouselHub(QWidget* parent);

    void SetGame(const GameDetailsInfo& info);
    /// Ignored unless it belongs to the game shown.
    void SetExtras(const GameDetailsExtras& extras);
    void SetFavorite(bool favorite);
    const GameDetailsInfo& Game() const {
        return game;
    }

    /// The carousel's height, which the open panel's height follows.
    void SetAvailableHeight(int height);
    /// The height the hub takes under the title, following the panel's open animation.
    int ReservedHeight() const;
    bool PanelOpen() const {
        return panel != Panel::None;
    }
    bool ExtrasLoaded() const {
        return extras_loaded;
    }
    /// Moves the focus onto the tile row, on Play.
    void FocusTiles();
    void ClosePanel();

signals:
    void PlayRequested();
    void FavoriteToggled();
    void ConfigureRequested();
    void PrecompileShadersRequested();
    void OpenLocationRequested();
    void DisabledAddOnsChanged(const QStringList& disabled);
    void IconChanged(u64 program_id);
    void HeroChanged(u64 program_id);
    /// A panel opened, or the game changed under an open one, and its details are not loaded.
    void ExtrasNeeded();
    /// Up or Escape/B on the tile row: the focus goes back to the cards.
    void ExitRequested();
    /// ReservedHeight() changed.
    void HeightChanged();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    bool focusNextPrevChild(bool next) override;
    void changeEvent(QEvent* event) override;

private:
    enum Tile { PlayTile, FavoriteTile, SettingsTile, AddOnsTile, ShadersTile, InfoTile, TileCount };

    QWidget* BuildAddOns();
    QWidget* BuildShaders();
    QWidget* BuildInfo();
    void TogglePanel(Panel which);
    void AnimatePanel(bool open);
    QToolButton* PanelTile(Panel which) const;
    /// Moves the focus within the open panel; false when it would leave the panel's top.
    bool MoveInPanel(bool forward);
    void RefreshTiles();
    void RefreshPanels();
    void RefreshInfo();
    void RetranslateUI();

    GameDetailsInfo game;
    GameDetailsExtras extras;
    bool extras_loaded = false;
    Panel panel = Panel::None;
    int panel_height = 240;
    qreal panel_progress = 0.0;
    QVariantAnimation* panel_animation = nullptr;

    QWidget* tile_row = nullptr;
    std::array<QToolButton*, TileCount> tiles{};

    QFrame* panel_frame = nullptr;
    QScrollArea* panel_scroll = nullptr;
    QStackedWidget* panel_stack = nullptr;
    QWidget* add_ons_page = nullptr;
    QWidget* shaders_page = nullptr;
    QWidget* info_page = nullptr;

    AddOnList* add_on_list = nullptr;
    QLabel* pipeline_cache = nullptr;
    QLabel* precompiled = nullptr;
    QPushButton* precompile_button = nullptr;
    QFormLayout* info_form = nullptr;
    QPushButton* location_button = nullptr;
    QPushButton* banner_button = nullptr;
    QPushButton* icon_button = nullptr;
    QMenu* banner_menu = nullptr;
    QMenu* icon_menu = nullptr;

    GameArtwork* artwork = nullptr;
};
