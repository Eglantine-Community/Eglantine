// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <vector>

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
class QToolButton;
class QVBoxLayout;
class QVariantAnimation;

/// The centered game's details band under the carousel strip: title, developer and a row of
/// facts, the Play / Favorite / Settings actions, and tabbed pages (Overview, Add-ons, Shaders,
/// Info). The carousel_details setting picks Full (every page, always open), Compact (the header
/// and tabs, a page opens on demand) or Off (nothing until Down or a click on the centered card
/// reveals the compact band).
///
/// Controller and keyboard: Down from the cards enters the actions, Down again the tabs, then the
/// page. Left/Right move along a row, Up goes back, L/R switch pages from anywhere, B/Escape
/// returns to the cards.
class CarouselHub : public QWidget {
    Q_OBJECT

public:
    enum class Style { Full, Compact, Off };
    enum class Page { Overview, AddOns, Shaders, Info };

    explicit CarouselHub(QWidget* parent);

    void SetGame(const GameDetailsInfo& info);
    /// Ignored unless it belongs to the game shown.
    void SetExtras(const GameDetailsExtras& extras);
    void SetFavorite(bool favorite);
    /// The centered game's place in the filtered list.
    void SetPosition(int position, int count);
    const GameDetailsInfo& Game() const {
        return game;
    }

    /// Re-reads the carousel_details setting.
    void ApplyStyle();
    /// The carousel draws the title and info line itself only while the band is off and hidden.
    bool CarouselDrawsTitle() const {
        return style == Style::Off && !revealed;
    }
    /// The carousel viewport's height, which the band's pages follow.
    void SetAvailableHeight(int height);
    /// The height the band takes under the strip, following its animations.
    int ReservedHeight() const;
    bool PanelOpen() const;
    /// Moves the focus onto Play, revealing the band when it is off.
    void FocusActions();
    /// Off style: shows the band without moving the focus, for a click on the centered card.
    void Reveal();
    /// Off style: hides a revealed band.
    void Conceal();
    void ClosePanel();
    /// L/R: the previous or next page.
    void CyclePage(int direction);

signals:
    void PlayRequested();
    void FavoriteToggled();
    void ConfigureRequested();
    void PrecompileShadersRequested();
    void OpenLocationRequested();
    void DisabledAddOnsChanged(const QStringList& disabled);
    void IconChanged(u64 program_id);
    void HeroChanged(u64 program_id);
    /// A page needs the game's disk details and they are not loaded.
    void ExtrasNeeded();
    /// Up or Escape/B: the focus goes back to the cards.
    void ExitRequested();
    /// ReservedHeight() changed.
    void HeightChanged();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    bool focusNextPrevChild(bool next) override;
    void changeEvent(QEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    enum class Row { None, Actions, Tabs, Content };
    static constexpr int PageCount = 4;

    QWidget* BuildHeader();
    QWidget* BuildTabs();
    QWidget* BuildOverview();
    QWidget* BuildAddOns();
    QWidget* BuildShaders();
    QWidget* BuildInfo();

    /// Off while revealed behaves as Compact.
    Style EffectiveStyle() const;
    std::vector<Page> VisiblePages() const;
    bool ContentShown() const;
    void ShowPage(Page which);
    void SelectPage(Page which, bool open);
    void TogglePage(Page which);
    QToolButton* TabFor(Page which) const;
    Row RowOf(QWidget* widget) const;
    std::vector<QWidget*> ActionButtons() const;
    /// Moves the focus within the open page; false when it would leave the page's top.
    bool MoveInPanel(bool forward);
    void FocusCurrentTab();
    void NeedExtras();

    int ChromeHeight() const;
    int ContentHeight() const;
    int TargetHeight() const;
    /// Animates the band toward its target height.
    void UpdateHeight(bool animate = true);
    void LayoutBand();

    void RefreshHeader();
    void RefreshActions();
    void RefreshTabs();
    void RefreshOverview();
    void RefreshPanels();
    void RefreshInfo();
    void RefreshPalette();
    void RetranslateUI();

    GameDetailsInfo game;
    GameDetailsExtras extras;
    bool extras_loaded = false;
    bool extras_requested = false;
    Style style = Style::Full;
    bool revealed = false;
    Page page = Page::Overview;
    /// Compact: whether the current page is open under the tabs.
    bool content_open = false;
    int available_height = -1;
    qreal shown_height = 0.0;
    QVariantAnimation* height_animation = nullptr;
    int position = 0;
    int count = 0;

    QFrame* band = nullptr;
    QVBoxLayout* band_layout = nullptr;
    QWidget* header = nullptr;
    QLabel* title_label = nullptr;
    QLabel* subtitle_label = nullptr;
    QWidget* chips = nullptr;
    QPushButton* play_button = nullptr;
    QToolButton* favorite_button = nullptr;
    QToolButton* settings_button = nullptr;

    QWidget* tab_row = nullptr;
    std::array<QToolButton*, PageCount> tabs{};
    QLabel* position_label = nullptr;

    QScrollArea* content_scroll = nullptr;
    QWidget* content_pages = nullptr;
    std::array<QWidget*, PageCount> pages{};

    std::array<QLabel*, 5> stat_captions{};
    std::array<QLabel*, 5> stat_values{};

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
