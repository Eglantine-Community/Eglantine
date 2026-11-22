// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <cmath>
#include <numbers>
#include <vector>

#include <QApplication>
#include <QDir>
#include <QEasingCurve>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLocale>
#include <QMenu>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QScrollArea>
#include <QStackedWidget>
#include <QToolButton>
#include <QVBoxLayout>
#include <QVariantAnimation>

#include "yuzu/game/add_on_list.h"
#include "yuzu/game/carousel_hub.h"
#include "yuzu/game/game_artwork.h"

namespace {

constexpr int TileWidth = 96;
constexpr int TileHeight = 78;
constexpr int TileIconSize = 28;
constexpr int Spacing = 10;
constexpr int PanelMaxWidth = 720;
constexpr int PanelAnimationMs = 180;

enum class Glyph { Play, Star, StarFilled, Gear, AddOns, Shaders, Info };

/// Tile icons, drawn in the button text colour so they read on every style and theme; the
/// icon theme has no reliable set for these on Windows and macOS.
QIcon MakeGlyph(Glyph glyph, const QColor& color, qreal dpr) {
    constexpr int size = TileIconSize;
    QPixmap pixmap(QSize(size, size) * dpr);
    pixmap.setDevicePixelRatio(dpr);
    pixmap.fill(Qt::transparent);
    QPainter p(&pixmap);
    p.setRenderHint(QPainter::Antialiasing);
    p.scale(size / 20.0, size / 20.0);
    const QPen outline(color, 1.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);

    const auto star = [] {
        QPolygonF poly;
        for (int i = 0; i < 10; ++i) {
            const qreal radius = i % 2 == 0 ? 8.5 : 3.6;
            const qreal angle = -std::numbers::pi / 2 + i * std::numbers::pi / 5;
            poly << QPointF(10 + radius * std::cos(angle), 10.6 + radius * std::sin(angle));
        }
        return poly;
    };

    switch (glyph) {
    case Glyph::Play: {
        QPolygonF triangle;
        triangle << QPointF(6, 3.5) << QPointF(16.5, 10) << QPointF(6, 16.5);
        p.setPen(Qt::NoPen);
        p.setBrush(color);
        p.drawPolygon(triangle);
        break;
    }
    case Glyph::Star:
        p.setPen(outline);
        p.setBrush(Qt::NoBrush);
        p.drawPolygon(star());
        break;
    case Glyph::StarFilled:
        p.setPen(outline);
        p.setBrush(color);
        p.drawPolygon(star());
        break;
    case Glyph::Gear: {
        QPainterPath gear;
        gear.setFillRule(Qt::OddEvenFill);
        for (int i = 0; i < 8; ++i) {
            QTransform t;
            t.translate(10, 10);
            t.rotate(i * 45.0);
            QPainterPath tooth;
            tooth.addRoundedRect(QRectF(-1.6, -8.5, 3.2, 4.0), 0.8, 0.8);
            gear.addPath(t.map(tooth));
        }
        gear.addEllipse(QPointF(10, 10), 5.8, 5.8);
        gear = gear.simplified();
        gear.addEllipse(QPointF(10, 10), 2.4, 2.4);
        p.setPen(Qt::NoPen);
        p.setBrush(color);
        p.drawPath(gear);
        break;
    }
    case Glyph::AddOns:
        p.setPen(outline);
        p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(QRectF(3, 6.5, 10.5, 10.5), 2, 2);
        p.drawPolyline(QPolygonF{QPointF(6.5, 3), QPointF(17, 3), QPointF(17, 13.5)});
        break;
    case Glyph::Shaders: {
        // An isometric cube.
        p.setPen(outline);
        p.setBrush(Qt::NoBrush);
        const QPointF top(10, 2.5), right(17, 6.3), bottom(10, 10.1), left(3, 6.3);
        p.drawPolygon(QPolygonF{top, right, bottom, left});
        p.drawPolyline(QPolygonF{left, QPointF(3, 13.7), QPointF(10, 17.5), QPointF(17, 13.7), right});
        p.drawLine(bottom, QPointF(10, 17.5));
        break;
    }
    case Glyph::Info:
        p.setPen(outline);
        p.setBrush(Qt::NoBrush);
        p.drawEllipse(QPointF(10, 10), 8, 8);
        p.setPen(QPen(color, 1.8, Qt::SolidLine, Qt::RoundCap));
        p.drawLine(QPointF(10, 9), QPointF(10, 14.2));
        p.setPen(Qt::NoPen);
        p.setBrush(color);
        p.drawEllipse(QPointF(10, 6), 1.1, 1.1);
        break;
    }
    return QIcon(pixmap);
}

/// Every control in `root` that can take the keyboard focus, in focus chain order.
std::vector<QWidget*> FocusableIn(QWidget* root) {
    std::vector<QWidget*> out;
    for (QWidget* w = root->nextInFocusChain(); w && w != root; w = w->nextInFocusChain()) {
        if (root->isAncestorOf(w) && w->isVisible() && w->isEnabled() &&
            (w->focusPolicy() & Qt::TabFocus)) {
            out.push_back(w);
        }
        if (out.size() > 512) {
            break;
        }
    }
    return out;
}

} // namespace

CarouselHub::CarouselHub(QWidget* parent) : QWidget{parent} {
    // Presses between the tiles must not reach the carousel underneath and start a drag.
    setAttribute(Qt::WA_NoMousePropagation);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(Spacing);

    tile_row = new QWidget(this);
    auto* row = new QHBoxLayout(tile_row);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(Spacing);
    row->addStretch(1);
    for (int i = 0; i < TileCount; ++i) {
        auto* tile = new QToolButton(tile_row);
        tile->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
        tile->setIconSize(QSize(TileIconSize, TileIconSize));
        tile->setFixedSize(TileWidth, TileHeight);
        tile->setFocusPolicy(Qt::TabFocus);
        tile->setCursor(Qt::PointingHandCursor);
        tiles[i] = tile;
        row->addWidget(tile);
    }
    row->addStretch(1);
    layout->addWidget(tile_row);
    tiles[FavoriteTile]->setCheckable(true);
    for (const int i : {AddOnsTile, ShadersTile, InfoTile}) {
        tiles[i]->setCheckable(true);
    }

    connect(tiles[PlayTile], &QToolButton::clicked, this, &CarouselHub::PlayRequested);
    connect(tiles[FavoriteTile], &QToolButton::clicked, this, &CarouselHub::FavoriteToggled);
    connect(tiles[SettingsTile], &QToolButton::clicked, this, &CarouselHub::ConfigureRequested);
    connect(tiles[AddOnsTile], &QToolButton::clicked, this, [this] { TogglePanel(Panel::AddOns); });
    connect(tiles[ShadersTile], &QToolButton::clicked, this,
            [this] { TogglePanel(Panel::Shaders); });
    connect(tiles[InfoTile], &QToolButton::clicked, this, [this] { TogglePanel(Panel::Info); });

    // The panel: a plain, solid surface the platform style draws.
    panel_frame = new QFrame(this);
    panel_frame->setFrameShape(QFrame::StyledPanel);
    panel_frame->setAutoFillBackground(true);
    panel_frame->setBackgroundRole(QPalette::Window);
    panel_frame->setMaximumWidth(PanelMaxWidth);
    panel_frame->setFixedHeight(0);
    panel_frame->hide();
    auto* frame_layout = new QVBoxLayout(panel_frame);
    frame_layout->setContentsMargins(16, 12, 16, 12);
    panel_scroll = new QScrollArea(panel_frame);
    panel_scroll->setWidgetResizable(true);
    panel_scroll->setFrameShape(QFrame::NoFrame);
    panel_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    // The scroll area takes the arrow keys for scrolling; the hub wants them to move focus.
    panel_scroll->installEventFilter(this);
    frame_layout->addWidget(panel_scroll);
    panel_stack = new QStackedWidget(panel_scroll);
    add_ons_page = BuildAddOns();
    shaders_page = BuildShaders();
    info_page = BuildInfo();
    for (QWidget* page : {add_ons_page, shaders_page, info_page}) {
        panel_stack->addWidget(page);
    }
    panel_scroll->setWidget(panel_stack);
    layout->addWidget(panel_frame, 0, Qt::AlignHCenter);
    layout->addStretch(1);

    panel_animation = new QVariantAnimation(this);
    panel_animation->setDuration(PanelAnimationMs);
    panel_animation->setEasingCurve(QEasingCurve::OutCubic);
    connect(panel_animation, &QVariantAnimation::valueChanged, this, [this](const QVariant& v) {
        panel_progress = v.toReal();
        panel_frame->setFixedHeight(qRound(panel_progress * panel_height));
        panel_frame->setVisible(panel_progress > 0.0);
        emit HeightChanged();
    });

    artwork = new GameArtwork(this);
    connect(artwork, &GameArtwork::IconChanged, this, &CarouselHub::IconChanged);
    connect(artwork, &GameArtwork::HeroChanged, this, &CarouselHub::HeroChanged);
    connect(artwork, &GameArtwork::BusyChanged, this, [this](bool busy) {
        banner_button->setEnabled(!busy);
        icon_button->setEnabled(!busy);
    });

    RetranslateUI();
}

QWidget* CarouselHub::BuildAddOns() {
    auto* page = new QWidget(panel_stack);
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    add_on_list = new AddOnList(page);
    connect(add_on_list, &AddOnList::DisabledChanged, this, [this](const QStringList& disabled) {
        extras.add_ons = add_on_list->AddOns();
        emit DisabledAddOnsChanged(disabled);
    });
    layout->addWidget(add_on_list);
    layout->addStretch(1);
    return page;
}

QWidget* CarouselHub::BuildShaders() {
    auto* page = new QWidget(panel_stack);
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    auto* form = new QFormLayout;
    pipeline_cache = new QLabel(page);
    precompiled = new QLabel(page);
    form->addRow(tr("Pipeline cache"), pipeline_cache);
    form->addRow(tr("Pre-compiled"), precompiled);
    precompile_button = new QPushButton(page);
    precompile_button->setCursor(Qt::PointingHandCursor);
    connect(precompile_button, &QPushButton::clicked, this,
            &CarouselHub::PrecompileShadersRequested);
    layout->addLayout(form);
    layout->addWidget(precompile_button, 0, Qt::AlignLeft);
    layout->addStretch(1);
    return page;
}

QWidget* CarouselHub::BuildInfo() {
    auto* page = new QWidget(panel_stack);
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    info_form = new QFormLayout;
    layout->addLayout(info_form);

    // Kept apart from the form, which is rebuilt per game, so the focus survives a refresh.
    auto* buttons = new QHBoxLayout;
    location_button = new QPushButton(page);
    banner_button = new QPushButton(page);
    icon_button = new QPushButton(page);
    banner_menu = new QMenu(this);
    icon_menu = new QMenu(this);
    for (QMenu* menu : {banner_menu, icon_menu}) {
        const bool hero = menu == banner_menu;
        connect(menu, &QMenu::aboutToShow, this, [this, menu, hero] {
            artwork->FillMenu(menu, hero, game.program_id, game.title);
        });
    }
    banner_button->setMenu(banner_menu);
    icon_button->setMenu(icon_menu);
    for (QPushButton* button : {location_button, banner_button, icon_button}) {
        button->setCursor(Qt::PointingHandCursor);
        buttons->addWidget(button);
    }
    buttons->addStretch(1);
    connect(location_button, &QPushButton::clicked, this, &CarouselHub::OpenLocationRequested);
    layout->addLayout(buttons);
    layout->addStretch(1);
    return page;
}

void CarouselHub::SetGame(const GameDetailsInfo& info) {
    const bool changed = info.path != game.path || info.program_id != game.program_id;
    game = info;
    if (changed) {
        extras = GameDetailsExtras{.program_id = info.program_id, .path = info.path};
        extras_loaded = false;
        RefreshInfo();
    }
    RefreshTiles();
    RefreshPanels();
    if (changed && PanelOpen()) {
        emit ExtrasNeeded();
    }
}

void CarouselHub::SetExtras(const GameDetailsExtras& extras_) {
    if (extras_.program_id != game.program_id || extras_.path != game.path) {
        return;
    }
    const bool focus_in_list = add_on_list->isAncestorOf(QApplication::focusWidget());
    extras = extras_;
    extras_loaded = true;
    RefreshPanels();
    // A rebuilt add-on list drops the focus it held; keep it in the panel.
    if (focus_in_list) {
        MoveInPanel(true);
    }
}

void CarouselHub::SetFavorite(bool favorite) {
    game.favorite = favorite;
    RefreshTiles();
}

void CarouselHub::SetAvailableHeight(int height) {
    const int wanted = std::clamp(height * 36 / 100, 180, 340);
    if (wanted == panel_height) {
        return;
    }
    panel_height = wanted;
    if (panel_progress > 0.0) {
        panel_frame->setFixedHeight(qRound(panel_progress * panel_height));
        emit HeightChanged();
    }
}

int CarouselHub::ReservedHeight() const {
    return TileHeight + (panel_progress > 0.0 ? Spacing + qRound(panel_progress * panel_height) : 0);
}

void CarouselHub::FocusTiles() {
    tiles[PlayTile]->setFocus(Qt::TabFocusReason);
}

void CarouselHub::ClosePanel() {
    if (!PanelOpen()) {
        return;
    }
    QToolButton* tile = PanelTile(panel);
    const bool focus_in_panel = panel_frame->isAncestorOf(QApplication::focusWidget());
    panel = Panel::None;
    RefreshTiles();
    AnimatePanel(false);
    if (focus_in_panel) {
        tile->setFocus(Qt::TabFocusReason);
    }
}

void CarouselHub::TogglePanel(Panel which) {
    if (panel == which) {
        ClosePanel();
        return;
    }
    const bool was_open = PanelOpen();
    panel = which;
    panel_stack->setCurrentWidget(which == Panel::AddOns   ? add_ons_page
                                  : which == Panel::Shaders ? shaders_page
                                                            : info_page);
    RefreshTiles();
    if (!was_open) {
        AnimatePanel(true);
    }
    if (!extras_loaded) {
        emit ExtrasNeeded();
    }
}

void CarouselHub::AnimatePanel(bool open) {
    panel_animation->stop();
    panel_animation->setStartValue(panel_progress);
    panel_animation->setEndValue(open ? 1.0 : 0.0);
    panel_animation->start();
}

QToolButton* CarouselHub::PanelTile(Panel which) const {
    switch (which) {
    case Panel::AddOns:
        return tiles[AddOnsTile];
    case Panel::Shaders:
        return tiles[ShadersTile];
    case Panel::Info:
        return tiles[InfoTile];
    case Panel::None:
        break;
    }
    return tiles[PlayTile];
}

bool CarouselHub::MoveInPanel(bool forward) {
    const std::vector<QWidget*> controls = FocusableIn(panel_stack->currentWidget());
    if (controls.empty()) {
        return false;
    }
    const auto it = std::find(controls.begin(), controls.end(), QApplication::focusWidget());
    int index;
    if (it == controls.end()) {
        index = forward ? 0 : int(controls.size()) - 1;
    } else {
        index = int(it - controls.begin()) + (forward ? 1 : -1);
    }
    if (index < 0 || index >= int(controls.size())) {
        return false;
    }
    controls[index]->setFocus(forward ? Qt::TabFocusReason : Qt::BacktabFocusReason);
    panel_scroll->ensureWidgetVisible(controls[index], 0, 24);
    return true;
}

bool CarouselHub::focusNextPrevChild(bool next) {
    QWidget* focus = QApplication::focusWidget();
    if (tile_row->isAncestorOf(focus)) {
        // Left and Right walk the row and stop at its ends.
        const auto it = std::find(tiles.begin(), tiles.end(), focus);
        const int index = std::clamp(int(it - tiles.begin()) + (next ? 1 : -1), 0, TileCount - 1);
        tiles[index]->setFocus(next ? Qt::TabFocusReason : Qt::BacktabFocusReason);
        return true;
    }
    if (panel_frame->isAncestorOf(focus)) {
        MoveInPanel(next);
        return true;
    }
    return QWidget::focusNextPrevChild(next);
}

void CarouselHub::keyPressEvent(QKeyEvent* event) {
    QWidget* focus = QApplication::focusWidget();
    const bool in_tiles = tile_row->isAncestorOf(focus);
    const bool in_panel = panel_frame->isAncestorOf(focus);
    switch (event->key()) {
    case Qt::Key_Up:
        if (in_panel) {
            if (!MoveInPanel(false)) {
                PanelTile(panel)->setFocus(Qt::BacktabFocusReason);
            }
        } else {
            emit ExitRequested();
        }
        return;
    case Qt::Key_Down:
        if ((in_tiles && PanelOpen()) || in_panel) {
            MoveInPanel(true);
        }
        return;
    case Qt::Key_Escape:
        // Closing moves a focus that was inside the panel onto its tile.
        if (PanelOpen()) {
            ClosePanel();
        } else {
            emit ExitRequested();
        }
        return;
    case Qt::Key_Enter:
    case Qt::Key_Return: {
        // Buttons outside a dialog ignore Enter, and a controller's A arrives as Enter.
        auto* button = qobject_cast<QAbstractButton*>(focus);
        if (button && isAncestorOf(button)) {
            if (auto* push = qobject_cast<QPushButton*>(button); push && push->menu()) {
                push->showMenu();
            } else {
                button->click();
            }
        }
        return;
    }
    // Sent by controller Y and X.
    case Qt::Key_Favorites:
        emit FavoriteToggled();
        return;
    case Qt::Key_Settings:
        emit ConfigureRequested();
        return;
    case Qt::Key_Menu:
        emit ExitRequested();
        return;
    default:
        QWidget::keyPressEvent(event);
    }
}

bool CarouselHub::eventFilter(QObject* watched, QEvent* event) {
    if (watched == panel_scroll && event->type() == QEvent::KeyPress) {
        const int key = static_cast<QKeyEvent*>(event)->key();
        if (key == Qt::Key_Up || key == Qt::Key_Down) {
            QKeyEvent forwarded(QEvent::KeyPress, key, Qt::NoModifier);
            keyPressEvent(&forwarded);
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

void CarouselHub::RefreshTiles() {
    const QColor ink = palette().color(QPalette::ButtonText);
    const qreal dpr = devicePixelRatioF();
    tiles[PlayTile]->setIcon(MakeGlyph(Glyph::Play, ink, dpr));
    tiles[FavoriteTile]->setIcon(
        MakeGlyph(game.favorite ? Glyph::StarFilled : Glyph::Star, ink, dpr));
    tiles[FavoriteTile]->setChecked(game.favorite);
    tiles[FavoriteTile]->setToolTip(game.favorite ? tr("Remove from Favorites")
                                                  : tr("Add to Favorites"));
    tiles[SettingsTile]->setIcon(MakeGlyph(Glyph::Gear, ink, dpr));
    tiles[AddOnsTile]->setIcon(MakeGlyph(Glyph::AddOns, ink, dpr));
    tiles[ShadersTile]->setIcon(MakeGlyph(Glyph::Shaders, ink, dpr));
    tiles[InfoTile]->setIcon(MakeGlyph(Glyph::Info, ink, dpr));
    tiles[AddOnsTile]->setChecked(panel == Panel::AddOns);
    tiles[ShadersTile]->setChecked(panel == Panel::Shaders);
    tiles[InfoTile]->setChecked(panel == Panel::Info);
    // Add-ons and shaders belong to a title ID.
    const bool has_id = game.program_id != 0;
    tiles[AddOnsTile]->setEnabled(has_id);
    tiles[ShadersTile]->setEnabled(has_id);
    tiles[FavoriteTile]->setEnabled(has_id);
}

void CarouselHub::RefreshPanels() {
    add_on_list->SetAddOns(extras.add_ons, extras_loaded);
    if (!extras_loaded) {
        pipeline_cache->setText(tr("..."));
        precompiled->setText(tr("..."));
        precompile_button->setText(tr("Pre-compile Shaders"));
        return;
    }
    pipeline_cache->setText(
        extras.pipeline_cache_bytes == 0
            ? tr("None yet")
            : QLocale().formattedDataSize(static_cast<qint64>(extras.pipeline_cache_bytes)));
    precompiled->setText(extras.shaders_precompiled ? tr("Yes") : tr("No"));
    precompile_button->setText(extras.shaders_precompiled ? tr("Pre-compile Again")
                                                          : tr("Pre-compile Shaders"));
}

void CarouselHub::RefreshInfo() {
    while (info_form->rowCount() > 0) {
        info_form->removeRow(0);
    }
    QWidget* page = info_page;
    const auto add_text = [&](const QString& key, const QString& value) {
        if (value.trimmed().isEmpty()) {
            return;
        }
        auto* label = new QLabel(value, page);
        label->setTextInteractionFlags(Qt::TextSelectableByMouse);
        label->setWordWrap(true);
        info_form->addRow(key, label);
    };
    add_text(tr("Version"), game.version);
    if (game.program_id != 0) {
        add_text(tr("Title ID"),
                 QStringLiteral("%1").arg(game.program_id, 16, 16, QLatin1Char('0')).toUpper());
    }
    add_text(tr("Format"), game.file_type);
    add_text(tr("Size"), game.size);
    add_text(tr("Developer"), game.developer);
    location_button->setToolTip(QDir::toNativeSeparators(game.path));
    location_button->setVisible(!game.path.isEmpty());
    banner_button->setVisible(game.program_id != 0);
    icon_button->setVisible(game.program_id != 0);
}

void CarouselHub::RetranslateUI() {
    tiles[PlayTile]->setText(tr("Play"));
    tiles[FavoriteTile]->setText(tr("Favorite"));
    tiles[SettingsTile]->setText(tr("Settings"));
    tiles[SettingsTile]->setToolTip(tr("Game settings"));
    tiles[AddOnsTile]->setText(tr("Add-ons"));
    tiles[AddOnsTile]->setToolTip(tr("Updates, DLC and mods"));
    tiles[ShadersTile]->setText(tr("Shaders"));
    tiles[InfoTile]->setText(tr("Info"));
    tiles[InfoTile]->setToolTip(tr("Details and artwork"));
    precompile_button->setToolTip(
        tr("Compile this game's shaders ahead of time to reduce stutter while playing"));
    location_button->setText(tr("Open Game Folder"));
    banner_button->setText(tr("Change Banner"));
    icon_button->setText(tr("Change Icon"));
    RefreshTiles();
    RefreshPanels();
    RefreshInfo();
}

void CarouselHub::changeEvent(QEvent* event) {
    if (event->type() == QEvent::LanguageChange) {
        RetranslateUI();
    } else if (event->type() == QEvent::PaletteChange) {
        // The tile glyphs are drawn in the palette's button text colour.
        QMetaObject::invokeMethod(this, [this] { RefreshTiles(); }, Qt::QueuedConnection);
    }
    QWidget::changeEvent(event);
}
