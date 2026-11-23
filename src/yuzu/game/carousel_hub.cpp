// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <cmath>
#include <numbers>
#include <vector>

#include <QApplication>
#include <QDir>
#include <QEasingCurve>
#include <QFileInfo>
#include <QFormLayout>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLocale>
#include <QMenu>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QScrollArea>
#include <QToolButton>
#include <QVBoxLayout>
#include <QVariantAnimation>

#include "qt_common/config/uisettings.h"
#include "yuzu/game/add_on_list.h"
#include "yuzu/game/carousel_hub.h"
#include "yuzu/game/game_artwork.h"

namespace {

constexpr int BandRadius = 14;
constexpr int BandPadding = 18;
constexpr int Spacing = 10;
constexpr int ActionHeight = 46;
constexpr int PlayWidth = 168;
constexpr int ActionIconSize = 22;
constexpr int TabIconSize = 16;
constexpr int TitlePixelSize = 24;
constexpr int HeightAnimationMs = 180;
constexpr int ChipPadX = 10;
constexpr int ChipPadY = 3;
constexpr int ChipGap = 6;

enum class Glyph { Play, Star, StarFilled, Gear, Overview, AddOns, Shaders, Info };

/// Action and tab icons, drawn in a palette colour so they read on every style and theme; the
/// icon theme has no reliable set for these on Windows and macOS.
QIcon MakeGlyph(Glyph glyph, const QColor& color, qreal dpr, int size) {
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
    case Glyph::Overview:
        p.setPen(outline);
        p.setBrush(Qt::NoBrush);
        for (const QPointF& corner :
             {QPointF(3, 3), QPointF(11, 3), QPointF(3, 11), QPointF(11, 11)}) {
            p.drawRoundedRect(QRectF(corner, QSizeF(6, 6)), 1.5, 1.5);
        }
        break;
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
        p.drawPolyline(
            QPolygonF{left, QPointF(3, 13.7), QPointF(10, 17.5), QPointF(17, 13.7), right});
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

void SetMuted(QWidget* widget, const QPalette& base) {
    QPalette pal = widget->palette();
    QColor color = base.color(QPalette::WindowText);
    color.setAlpha(160);
    pal.setColor(QPalette::WindowText, color);
    widget->setPalette(pal);
}

void SetPixelSize(QWidget* widget, int size, bool bold) {
    QFont font = widget->font();
    font.setPixelSize(size);
    font.setBold(bold);
    widget->setFont(font);
}

/// A one-line label that elides to the width the layout gives it instead of asking for its full
/// text: long game titles must not push the actions off the band.
class ElidedLabel : public QLabel {
public:
    explicit ElidedLabel(QWidget* parent) : QLabel{parent} {
        setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        setTextFormat(Qt::PlainText);
    }

    void SetFullText(const QString& text) {
        full_text = text;
        setToolTip(text);
        Elide();
    }

protected:
    void resizeEvent(QResizeEvent* event) override {
        QLabel::resizeEvent(event);
        Elide();
    }
    void changeEvent(QEvent* event) override {
        QLabel::changeEvent(event);
        if (event->type() == QEvent::FontChange) {
            Elide();
        }
    }

private:
    void Elide() {
        setText(fontMetrics().elidedText(full_text, Qt::ElideRight, std::max(0, width())));
    }

    QString full_text;
};

struct Chip {
    QString text;
    bool accent = false;
};

/// The game's facts as a row of pills. Pills that do not fit the width are left out rather than
/// squeezed.
class ChipRow : public QWidget {
public:
    explicit ChipRow(QWidget* parent) : QWidget{parent} {
        setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    }

    void SetChips(std::vector<Chip> chips_) {
        chips = std::move(chips_);
        update();
    }

    QSize sizeHint() const override {
        return {0, fontMetrics().height() + 2 * ChipPadY + 2};
    }
    QSize minimumSizeHint() const override {
        return sizeHint();
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const QFontMetrics fm = fontMetrics();
        const QPalette& pal = palette();
        const qreal h = height() - 1.0;
        qreal x = 0.5;
        for (const Chip& chip : chips) {
            const qreal w = fm.horizontalAdvance(chip.text) + 2 * ChipPadX;
            if (x + w > width()) {
                break;
            }
            const QRectF rect(x, 0.5, w, h);
            QColor fill = pal.color(chip.accent ? QPalette::Highlight : QPalette::Button);
            fill.setAlpha(chip.accent ? 70 : 160);
            QColor border = pal.color(chip.accent ? QPalette::Highlight : QPalette::Mid);
            border.setAlpha(chip.accent ? 170 : 120);
            p.setPen(QPen(border, 1.0));
            p.setBrush(fill);
            p.drawRoundedRect(rect, h / 2, h / 2);
            p.setPen(pal.color(QPalette::WindowText));
            p.drawText(rect, Qt::AlignCenter, chip.text);
            x += w + ChipGap;
        }
    }

private:
    std::vector<Chip> chips;
};

/// The band's surface: a rounded, nearly opaque panel in the window colour, so the details stay
/// readable over any backdrop or banner.
class BandFrame : public QFrame {
public:
    explicit BandFrame(QWidget* parent) : QFrame{parent} {}

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        QColor fill = palette().color(QPalette::Window);
        fill.setAlpha(235);
        QColor border = palette().color(QPalette::Mid);
        border.setAlpha(110);
        p.setPen(QPen(border, 1.0));
        p.setBrush(fill);
        p.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), BandRadius, BandRadius);
    }
};

/// One fact on the Overview page, on a faint raised tile.
class StatTile : public QWidget {
public:
    explicit StatTile(QWidget* parent) : QWidget{parent} {}

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        QColor fill = palette().color(QPalette::Button);
        fill.setAlpha(120);
        p.setPen(Qt::NoPen);
        p.setBrush(fill);
        p.drawRoundedRect(QRectF(rect()), 10, 10);
    }
};

} // namespace

CarouselHub::CarouselHub(QWidget* parent) : QWidget{parent} {
    // Presses on the band must not reach the carousel underneath and start a drag.
    setAttribute(Qt::WA_NoMousePropagation);

    band = new BandFrame(this);
    band_layout = new QVBoxLayout(band);
    band_layout->setContentsMargins(BandPadding, BandPadding - 4, BandPadding, BandPadding - 4);
    band_layout->setSpacing(Spacing);
    band_layout->addWidget(BuildHeader());
    band_layout->addWidget(BuildTabs());

    content_scroll = new QScrollArea(band);
    content_scroll->setWidgetResizable(true);
    content_scroll->setFrameShape(QFrame::NoFrame);
    content_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    content_scroll->viewport()->setAutoFillBackground(false);
    // The scroll area takes the arrow keys for scrolling; the band wants them to move focus.
    content_scroll->installEventFilter(this);
    // The pages share one layout and only the current one is shown: a stacked widget would
    // size every page by the tallest, so the Overview would scroll by the Info page's height.
    content_pages = new QWidget(content_scroll);
    auto* pages_layout = new QVBoxLayout(content_pages);
    pages_layout->setContentsMargins(0, 0, 0, 0);
    pages[int(Page::Overview)] = BuildOverview();
    pages[int(Page::AddOns)] = BuildAddOns();
    pages[int(Page::Shaders)] = BuildShaders();
    pages[int(Page::Info)] = BuildInfo();
    for (QWidget* page_widget : pages) {
        pages_layout->addWidget(page_widget);
    }
    content_scroll->setWidget(content_pages);
    // setWidget turns the widget's background fill on; the band paints the surface.
    content_pages->setAutoFillBackground(false);
    ShowPage(page);
    band_layout->addWidget(content_scroll, 1);

    height_animation = new QVariantAnimation(this);
    height_animation->setDuration(HeightAnimationMs);
    height_animation->setEasingCurve(QEasingCurve::OutCubic);
    connect(height_animation, &QVariantAnimation::valueChanged, this, [this](const QVariant& v) {
        shown_height = v.toReal();
        LayoutBand();
        emit HeightChanged();
    });
    connect(height_animation, &QVariantAnimation::finished, this, [this] { LayoutBand(); });

    artwork = new GameArtwork(this);
    connect(artwork, &GameArtwork::IconChanged, this, &CarouselHub::IconChanged);
    connect(artwork, &GameArtwork::HeroChanged, this, &CarouselHub::HeroChanged);
    connect(artwork, &GameArtwork::BusyChanged, this, [this](bool busy) {
        banner_button->setEnabled(!busy);
        icon_button->setEnabled(!busy);
    });

    style = static_cast<Style>(
        std::min<int>(UISettings::values.carousel_details.GetValue(), int(Style::Off)));
    RetranslateUI();
    RefreshPalette();

    // Focused buttons turn Up and Down into Tab and Backtab, which loses the row the key meant;
    // whichever control in the band has the focus hands those keys to the band instead.
    connect(qApp, &QApplication::focusChanged, this, [this](QWidget*, QWidget* now) {
        if (now && isAncestorOf(now)) {
            now->installEventFilter(this);
        }
    });
}

QWidget* CarouselHub::BuildHeader() {
    header = new QWidget(band);
    auto* row = new QHBoxLayout(header);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(16);

    auto* text = new QVBoxLayout;
    text->setSpacing(4);
    auto* title = new ElidedLabel(header);
    SetPixelSize(title, TitlePixelSize, true);
    title_label = title;
    subtitle_label = new ElidedLabel(header);
    chips = new ChipRow(header);
    text->addWidget(title_label);
    text->addWidget(subtitle_label);
    text->addSpacing(2);
    text->addWidget(chips);
    row->addLayout(text, 1);

    play_button = new QPushButton(header);
    play_button->setMinimumSize(PlayWidth, ActionHeight);
    play_button->setIconSize(QSize(ActionIconSize, ActionIconSize));
    SetPixelSize(play_button, 16, true);
    favorite_button = new QToolButton(header);
    favorite_button->setCheckable(true);
    settings_button = new QToolButton(header);
    for (QToolButton* button : {favorite_button, settings_button}) {
        button->setFixedSize(ActionHeight, ActionHeight);
        button->setIconSize(QSize(ActionIconSize, ActionIconSize));
    }
    for (QAbstractButton* button :
         std::initializer_list<QAbstractButton*>{play_button, favorite_button, settings_button}) {
        // Keyboard and controller reach the actions; a click leaves the focus on the cards.
        button->setFocusPolicy(Qt::TabFocus);
        button->setCursor(Qt::PointingHandCursor);
        row->addWidget(button, 0, Qt::AlignVCenter);
    }
    connect(play_button, &QPushButton::clicked, this, &CarouselHub::PlayRequested);
    connect(favorite_button, &QToolButton::clicked, this, &CarouselHub::FavoriteToggled);
    connect(settings_button, &QToolButton::clicked, this, &CarouselHub::ConfigureRequested);
    return header;
}

QWidget* CarouselHub::BuildTabs() {
    tab_row = new QWidget(band);
    auto* row = new QHBoxLayout(tab_row);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(4);
    for (int i = 0; i < PageCount; ++i) {
        auto* tab = new QToolButton(tab_row);
        tab->setCheckable(true);
        tab->setAutoRaise(true);
        tab->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        tab->setIconSize(QSize(TabIconSize, TabIconSize));
        tab->setFocusPolicy(Qt::TabFocus);
        tab->setCursor(Qt::PointingHandCursor);
        tab->installEventFilter(this);
        connect(tab, &QToolButton::clicked, this, [this, i] { TogglePage(Page(i)); });
        tabs[i] = tab;
        row->addWidget(tab);
    }
    row->addStretch(1);
    position_label = new QLabel(tab_row);
    row->addWidget(position_label);
    return tab_row;
}

QWidget* CarouselHub::BuildOverview() {
    auto* page_widget = new QWidget(content_pages);
    auto* layout = new QVBoxLayout(page_widget);
    layout->setContentsMargins(0, 0, 0, 0);
    auto* row = new QHBoxLayout;
    row->setSpacing(Spacing);
    for (size_t i = 0; i < stat_values.size(); ++i) {
        auto* tile = new StatTile(page_widget);
        auto* tile_layout = new QVBoxLayout(tile);
        tile_layout->setContentsMargins(12, 8, 12, 10);
        tile_layout->setSpacing(2);
        stat_captions[i] = new QLabel(tile);
        SetPixelSize(stat_captions[i], 12, false);
        stat_values[i] = new QLabel(tile);
        stat_values[i]->setWordWrap(true);
        SetPixelSize(stat_values[i], 15, true);
        tile_layout->addWidget(stat_captions[i]);
        tile_layout->addWidget(stat_values[i]);
        tile_layout->addStretch(1);
        row->addWidget(tile, 1);
    }
    layout->addLayout(row, 1);
    return page_widget;
}

QWidget* CarouselHub::BuildAddOns() {
    auto* page_widget = new QWidget(content_pages);
    auto* layout = new QVBoxLayout(page_widget);
    layout->setContentsMargins(0, 0, 0, 0);
    add_on_list = new AddOnList(page_widget);
    connect(add_on_list, &AddOnList::DisabledChanged, this, [this](const QStringList& disabled) {
        extras.add_ons = add_on_list->AddOns();
        RefreshOverview();
        emit DisabledAddOnsChanged(disabled);
    });
    layout->addWidget(add_on_list);
    layout->addStretch(1);
    return page_widget;
}

QWidget* CarouselHub::BuildShaders() {
    auto* page_widget = new QWidget(content_pages);
    auto* layout = new QVBoxLayout(page_widget);
    layout->setContentsMargins(0, 0, 0, 0);
    auto* form = new QFormLayout;
    pipeline_cache = new QLabel(page_widget);
    precompiled = new QLabel(page_widget);
    form->addRow(tr("Pipeline cache"), pipeline_cache);
    form->addRow(tr("Pre-compiled"), precompiled);
    precompile_button = new QPushButton(page_widget);
    precompile_button->setCursor(Qt::PointingHandCursor);
    connect(precompile_button, &QPushButton::clicked, this,
            &CarouselHub::PrecompileShadersRequested);
    layout->addLayout(form);
    layout->addWidget(precompile_button, 0, Qt::AlignLeft);
    layout->addStretch(1);
    return page_widget;
}

QWidget* CarouselHub::BuildInfo() {
    auto* page_widget = new QWidget(content_pages);
    auto* layout = new QVBoxLayout(page_widget);
    layout->setContentsMargins(0, 0, 0, 0);
    info_form = new QFormLayout;
    layout->addLayout(info_form);

    // Kept apart from the form, which is rebuilt per game, so the focus survives a refresh.
    auto* buttons = new QHBoxLayout;
    location_button = new QPushButton(page_widget);
    banner_button = new QPushButton(page_widget);
    icon_button = new QPushButton(page_widget);
    banner_menu = new QMenu(this);
    icon_menu = new QMenu(this);
    for (QMenu* menu : {banner_menu, icon_menu}) {
        const bool hero = menu == banner_menu;
        connect(menu, &QMenu::aboutToShow, this,
                [this, menu, hero] { artwork->FillMenu(menu, hero, game.program_id, game.title); });
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
    return page_widget;
}

void CarouselHub::SetGame(const GameDetailsInfo& info) {
    const bool changed = info.path != game.path || info.program_id != game.program_id;
    game = info;
    if (changed) {
        extras = GameDetailsExtras{.program_id = info.program_id, .path = info.path};
        extras_loaded = false;
        extras_requested = false;
        RefreshInfo();
        // Add-ons and shaders belong to a title ID.
        if (game.program_id == 0 && (page == Page::AddOns || page == Page::Shaders)) {
            page = Page::Overview;
            content_open = false;
            ShowPage(page);
            UpdateHeight();
        }
        // Browsing with the mouse hides a band revealed by a click.
        if (revealed && !isAncestorOf(QApplication::focusWidget())) {
            Conceal();
        }
    }
    RefreshHeader();
    RefreshActions();
    RefreshTabs();
    RefreshOverview();
    RefreshPanels();
    if (changed && ContentShown()) {
        NeedExtras();
    }
}

void CarouselHub::SetExtras(const GameDetailsExtras& extras_) {
    if (extras_.program_id != game.program_id || extras_.path != game.path) {
        return;
    }
    const bool focus_in_list = add_on_list->isAncestorOf(QApplication::focusWidget());
    extras = extras_;
    extras_loaded = true;
    RefreshOverview();
    RefreshPanels();
    // The add-on list's length decides an open compact page's height.
    UpdateHeight();
    // A rebuilt add-on list drops the focus it held; keep it in the page.
    if (focus_in_list) {
        MoveInPanel(true);
    }
}

void CarouselHub::SetFavorite(bool favorite) {
    game.favorite = favorite;
    RefreshHeader();
    RefreshActions();
}

void CarouselHub::SetPosition(int position_, int count_) {
    if (position_ == position && count_ == count) {
        return;
    }
    position = position_;
    count = count_;
    position_label->setText(tr("%1 of %2").arg(position + 1).arg(count));
}

void CarouselHub::ApplyStyle() {
    const auto next = static_cast<Style>(
        std::min<int>(UISettings::values.carousel_details.GetValue(), int(Style::Off)));
    if (next == style) {
        return;
    }
    style = next;
    revealed = false;
    if (style == Style::Compact && page == Page::Overview) {
        content_open = false;
    }
    RefreshTabs();
    UpdateHeight();
    if (ContentShown()) {
        NeedExtras();
    }
}

CarouselHub::Style CarouselHub::EffectiveStyle() const {
    return style == Style::Off && revealed ? Style::Compact : style;
}

std::vector<CarouselHub::Page> CarouselHub::VisiblePages() const {
    std::vector<Page> out;
    if (EffectiveStyle() == Style::Full) {
        out.push_back(Page::Overview);
    }
    if (game.program_id != 0) {
        out.push_back(Page::AddOns);
        out.push_back(Page::Shaders);
    }
    out.push_back(Page::Info);
    return out;
}

bool CarouselHub::ContentShown() const {
    switch (EffectiveStyle()) {
    case Style::Full:
        return true;
    case Style::Compact:
        return content_open && page != Page::Overview;
    case Style::Off:
        break;
    }
    return false;
}

bool CarouselHub::PanelOpen() const {
    return EffectiveStyle() == Style::Compact && ContentShown();
}

void CarouselHub::SetAvailableHeight(int height) {
    if (height == available_height) {
        return;
    }
    available_height = height;
    UpdateHeight(height_animation->state() == QAbstractAnimation::Running);
}

int CarouselHub::ChromeHeight() const {
    const QMargins margins = band_layout->contentsMargins();
    return margins.top() + margins.bottom() + header->sizeHint().height() + Spacing +
           tab_row->sizeHint().height();
}

int CarouselHub::ContentHeight() const {
    if (!ContentShown()) {
        return 0;
    }
    const int available = std::max(0, available_height);
    if (EffectiveStyle() == Style::Full) {
        return std::clamp(available * 12 / 100, 76, 180);
    }
    // A compact page opens only as far as its content needs.
    const int wanted = pages[int(page)]->sizeHint().height();
    return std::clamp(wanted, 72, std::clamp(available * 30 / 100, 150, 320));
}

int CarouselHub::TargetHeight() const {
    if (style == Style::Off && !revealed) {
        return 0;
    }
    const int content = ContentHeight();
    return ChromeHeight() + (content > 0 ? Spacing + content : 0);
}

int CarouselHub::ReservedHeight() const {
    return qRound(shown_height);
}

void CarouselHub::UpdateHeight(bool animate) {
    const int target = TargetHeight();
    height_animation->stop();
    if (!animate || !isVisible() || available_height < 0) {
        shown_height = target;
        LayoutBand();
        emit HeightChanged();
        return;
    }
    if (qRound(shown_height) == target) {
        LayoutBand();
        return;
    }
    height_animation->setStartValue(shown_height);
    height_animation->setEndValue(qreal(target));
    height_animation->start();
}

void CarouselHub::LayoutBand() {
    // The band is laid out at its full height and the hub clips it. The hub is pinned to the
    // bottom of the carousel, so the band is anchored to the hub's bottom and slides up into
    // view instead of being squeezed.
    const int height = std::max(TargetHeight(), qRound(shown_height));
    content_scroll->setVisible(height > ChromeHeight() + Spacing);
    const QRect geometry(0, this->height() - height, width(), height);
    if (band->geometry() != geometry) {
        band->setGeometry(geometry);
    }
}

void CarouselHub::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    LayoutBand();
}

void CarouselHub::FocusActions() {
    Reveal();
    play_button->setFocus(Qt::TabFocusReason);
}

void CarouselHub::Reveal() {
    if (style != Style::Off || revealed) {
        return;
    }
    revealed = true;
    show();
    RefreshTabs();
    UpdateHeight();
}

void CarouselHub::Conceal() {
    if (style != Style::Off || !revealed) {
        return;
    }
    revealed = false;
    content_open = false;
    RefreshTabs();
    UpdateHeight();
}

void CarouselHub::ClosePanel() {
    if (!PanelOpen()) {
        return;
    }
    const bool focus_in_panel = content_scroll->isAncestorOf(QApplication::focusWidget());
    content_open = false;
    RefreshTabs();
    UpdateHeight();
    if (focus_in_panel) {
        TabFor(page)->setFocus(Qt::BacktabFocusReason);
    }
}

void CarouselHub::ShowPage(Page which) {
    for (int i = 0; i < PageCount; ++i) {
        pages[i]->setVisible(i == int(which));
    }
}

void CarouselHub::SelectPage(Page which, bool open) {
    page = which;
    ShowPage(which);
    if (EffectiveStyle() == Style::Compact) {
        content_open = open && which != Page::Overview;
    }
    RefreshTabs();
    UpdateHeight();
    if (ContentShown()) {
        NeedExtras();
    }
}

void CarouselHub::TogglePage(Page which) {
    if (EffectiveStyle() == Style::Compact && content_open && page == which) {
        ClosePanel();
        return;
    }
    SelectPage(which, true);
}

void CarouselHub::CyclePage(int direction) {
    Reveal();
    const std::vector<Page> visible = VisiblePages();
    const int n = int(visible.size());
    const auto it = std::find(visible.begin(), visible.end(), page);
    Page next;
    if (!ContentShown() || it == visible.end()) {
        next = direction > 0 ? visible.front() : visible.back();
    } else {
        next = visible[((it - visible.begin()) + direction + n) % n];
    }
    const Row row = RowOf(QApplication::focusWidget());
    SelectPage(next, true);
    if (row == Row::Tabs || row == Row::Content) {
        TabFor(next)->setFocus(Qt::TabFocusReason);
    }
}

QToolButton* CarouselHub::TabFor(Page which) const {
    return tabs[int(which)];
}

void CarouselHub::FocusCurrentTab() {
    const std::vector<Page> visible = VisiblePages();
    const bool current_visible = std::find(visible.begin(), visible.end(), page) != visible.end();
    TabFor(current_visible ? page : visible.front())->setFocus(Qt::TabFocusReason);
}

void CarouselHub::NeedExtras() {
    if (extras_loaded || extras_requested) {
        return;
    }
    extras_requested = true;
    emit ExtrasNeeded();
}

CarouselHub::Row CarouselHub::RowOf(QWidget* widget) const {
    if (!widget || !isAncestorOf(widget)) {
        return Row::None;
    }
    if (header->isAncestorOf(widget)) {
        return Row::Actions;
    }
    if (tab_row->isAncestorOf(widget)) {
        return Row::Tabs;
    }
    if (content_scroll->isAncestorOf(widget)) {
        return Row::Content;
    }
    return Row::None;
}

std::vector<QWidget*> CarouselHub::ActionButtons() const {
    std::vector<QWidget*> out;
    for (QWidget* button :
         std::initializer_list<QWidget*>{play_button, favorite_button, settings_button}) {
        if (button->isEnabled()) {
            out.push_back(button);
        }
    }
    return out;
}

bool CarouselHub::MoveInPanel(bool forward) {
    if (!ContentShown()) {
        return false;
    }
    const std::vector<QWidget*> controls = FocusableIn(pages[int(page)]);
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
    content_scroll->ensureWidgetVisible(controls[index], 0, 24);
    return true;
}

bool CarouselHub::focusNextPrevChild(bool next) {
    QWidget* focus = QApplication::focusWidget();
    const auto step = [&](const std::vector<QWidget*>& row) {
        const auto it = std::find(row.begin(), row.end(), focus);
        if (it == row.end() || row.empty()) {
            return;
        }
        const int index =
            std::clamp(int(it - row.begin()) + (next ? 1 : -1), 0, int(row.size()) - 1);
        row[index]->setFocus(next ? Qt::TabFocusReason : Qt::BacktabFocusReason);
    };
    switch (RowOf(focus)) {
    case Row::Actions:
        // Left and Right walk a row and stop at its ends.
        step(ActionButtons());
        return true;
    case Row::Tabs: {
        std::vector<QWidget*> row;
        for (const Page visible : VisiblePages()) {
            row.push_back(TabFor(visible));
        }
        step(row);
        return true;
    }
    case Row::Content:
        MoveInPanel(next);
        return true;
    case Row::None:
        break;
    }
    return QWidget::focusNextPrevChild(next);
}

void CarouselHub::keyPressEvent(QKeyEvent* event) {
    QWidget* focus = QApplication::focusWidget();
    const Row row = RowOf(focus);
    switch (event->key()) {
    case Qt::Key_Up:
        if (row == Row::Content) {
            if (!MoveInPanel(false)) {
                FocusCurrentTab();
            }
        } else if (row == Row::Tabs) {
            play_button->setFocus(Qt::BacktabFocusReason);
        } else {
            emit ExitRequested();
        }
        return;
    case Qt::Key_Down:
        if (row == Row::Actions) {
            FocusCurrentTab();
        } else if (row == Row::Tabs || row == Row::Content) {
            MoveInPanel(true);
        }
        return;
    case Qt::Key_Escape:
        // Closing moves a focus that was inside the page onto its tab.
        if (PanelOpen() && row != Row::Actions) {
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
    // Sent by controller L and R.
    case Qt::Key_Back:
        CyclePage(-1);
        return;
    case Qt::Key_Forward:
        CyclePage(1);
        return;
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
    // The window's Escape shortcut (Exit Fullscreen) would take the key before the band sees
    // it; inside the band Escape means back.
    if (event->type() == QEvent::ShortcutOverride && watched->isWidgetType() &&
        isAncestorOf(static_cast<QWidget*>(watched)) &&
        static_cast<QKeyEvent*>(event)->key() == Qt::Key_Escape) {
        event->accept();
        return true;
    }
    if (event->type() == QEvent::KeyPress && watched->isWidgetType() &&
        isAncestorOf(static_cast<QWidget*>(watched))) {
        const int key = static_cast<QKeyEvent*>(event)->key();
        if (key == Qt::Key_Up || key == Qt::Key_Down) {
            QKeyEvent forwarded(QEvent::KeyPress, key, Qt::NoModifier);
            keyPressEvent(&forwarded);
            return true;
        }
    }
    // Walking the tabs with the keyboard or a controller shows each page, unless a compact
    // band's page is closed: then Enter opens it.
    if (event->type() == QEvent::FocusIn) {
        for (int i = 0; i < PageCount; ++i) {
            if (watched == tabs[i] && page != Page(i) &&
                (EffectiveStyle() == Style::Full || PanelOpen())) {
                SelectPage(Page(i), true);
                break;
            }
        }
    }
    return QWidget::eventFilter(watched, event);
}

void CarouselHub::RefreshHeader() {
    static_cast<ElidedLabel*>(title_label)->SetFullText(game.title);
    static_cast<ElidedLabel*>(subtitle_label)
        ->SetFullText(game.developer.isEmpty() ? QFileInfo(game.path).fileName() : game.developer);

    std::vector<Chip> facts;
    if (game.program_id != 0) {
        facts.push_back({game.switch2 ? tr("Switch 2") : tr("Switch"), game.switch2});
    }
    if (!game.version.trimmed().isEmpty()) {
        facts.push_back({tr("v%1").arg(game.version.trimmed())});
    }
    if (!game.file_type.isEmpty()) {
        facts.push_back({game.file_type});
    }
    if (!game.size.isEmpty()) {
        facts.push_back({game.size});
    }
    if (game.favorite) {
        facts.push_back({tr("Favorite")});
    }
    static_cast<ChipRow*>(chips)->SetChips(std::move(facts));
}

void CarouselHub::RefreshActions() {
    const qreal dpr = devicePixelRatioF();
    const QColor ink = palette().color(QPalette::ButtonText);
    play_button->setIcon(MakeGlyph(Glyph::Play, play_button->palette().color(QPalette::ButtonText),
                                   dpr, ActionIconSize));
    favorite_button->setIcon(
        MakeGlyph(game.favorite ? Glyph::StarFilled : Glyph::Star, ink, dpr, ActionIconSize));
    favorite_button->setChecked(game.favorite);
    favorite_button->setToolTip(game.favorite ? tr("Remove from Favorites")
                                              : tr("Add to Favorites"));
    favorite_button->setEnabled(game.program_id != 0);
    settings_button->setIcon(MakeGlyph(Glyph::Gear, ink, dpr, ActionIconSize));
}

void CarouselHub::RefreshTabs() {
    const qreal dpr = devicePixelRatioF();
    const QColor ink = palette().color(QPalette::ButtonText);
    constexpr std::array<Glyph, PageCount> glyphs{Glyph::Overview, Glyph::AddOns, Glyph::Shaders,
                                                  Glyph::Info};
    const std::vector<Page> visible = VisiblePages();
    for (int i = 0; i < PageCount; ++i) {
        tabs[i]->setIcon(MakeGlyph(glyphs[i], ink, dpr, TabIconSize));
        tabs[i]->setVisible(std::find(visible.begin(), visible.end(), Page(i)) != visible.end());
        tabs[i]->setChecked(ContentShown() && page == Page(i));
    }
}

void CarouselHub::RefreshOverview() {
    enum { PlayTime, Update, AddOns, Shaders, Settings };
    stat_values[PlayTime]->setText(game.play_time.isEmpty() ? tr("Not played yet")
                                                            : game.play_time);
    if (!extras_loaded) {
        for (const int i : {int(Update), int(AddOns), int(Shaders), int(Settings)}) {
            stat_values[i]->setText(tr("..."));
        }
        return;
    }

    const GameAddOn* update = nullptr;
    bool has_updates = false;
    int add_ons = 0;
    int enabled = 0;
    for (const GameAddOn& add_on : extras.add_ons) {
        if (add_on.is_update) {
            has_updates = true;
            if (add_on.enabled) {
                update = &add_on;
            }
        } else {
            ++add_ons;
            enabled += add_on.enabled ? 1 : 0;
        }
    }
    if (update) {
        stat_values[Update]->setText(update->version.isEmpty() ? update->name
                                                               : tr("v%1").arg(update->version));
    } else {
        stat_values[Update]->setText(has_updates ? tr("Base game") : tr("None"));
    }
    stat_values[AddOns]->setText(add_ons == 0 ? tr("None")
                                              : tr("%1 of %2 on").arg(enabled).arg(add_ons));

    const QString cache =
        QLocale().formattedDataSize(static_cast<qint64>(extras.pipeline_cache_bytes));
    if (extras.shaders_precompiled) {
        stat_values[Shaders]->setText(extras.pipeline_cache_bytes == 0
                                          ? tr("Pre-compiled")
                                          : tr("Pre-compiled, %1").arg(cache));
    } else {
        stat_values[Shaders]->setText(
            extras.pipeline_cache_bytes == 0 ? tr("None yet") : tr("%1 cached").arg(cache));
    }
    stat_values[Settings]->setText(extras.custom_config ? tr("Custom") : tr("Global"));
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
    QWidget* page_widget = pages[int(Page::Info)];
    const auto add_text = [&](const QString& key, const QString& value) {
        if (value.trimmed().isEmpty()) {
            return;
        }
        auto* label = new QLabel(value, page_widget);
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
    add_text(tr("Location"), QDir::toNativeSeparators(game.path));
    location_button->setVisible(!game.path.isEmpty());
    banner_button->setVisible(game.program_id != 0);
    icon_button->setVisible(game.program_id != 0);
}

void CarouselHub::RefreshPalette() {
    const QPalette& base = palette();
    SetMuted(subtitle_label, base);
    SetMuted(position_label, base);
    for (QLabel* caption : stat_captions) {
        SetMuted(caption, base);
    }
    // Play is the band's primary action and wears the accent colour, unless a theme style sheet
    // draws the buttons and would ignore it.
    QPalette accent = base;
    if (qApp->styleSheet().isEmpty()) {
        accent.setColor(QPalette::Button, base.color(QPalette::Highlight));
        accent.setColor(QPalette::ButtonText, base.color(QPalette::HighlightedText));
    }
    play_button->setPalette(accent);
    RefreshActions();
    RefreshTabs();
}

void CarouselHub::RetranslateUI() {
    play_button->setText(tr("Play"));
    settings_button->setToolTip(tr("Game settings"));
    tabs[int(Page::Overview)]->setText(tr("Overview"));
    tabs[int(Page::AddOns)]->setText(tr("Add-ons"));
    tabs[int(Page::AddOns)]->setToolTip(tr("Updates, DLC and mods"));
    tabs[int(Page::Shaders)]->setText(tr("Shaders"));
    tabs[int(Page::Info)]->setText(tr("Info"));
    tabs[int(Page::Info)]->setToolTip(tr("Details and artwork"));
    stat_captions[0]->setText(tr("Play time"));
    stat_captions[1]->setText(tr("Update"));
    stat_captions[2]->setText(tr("DLC and mods"));
    stat_captions[3]->setText(tr("Shaders"));
    stat_captions[4]->setText(tr("Settings"));
    precompile_button->setToolTip(
        tr("Compile this game's shaders ahead of time to reduce stutter while playing"));
    location_button->setText(tr("Open Game Folder"));
    banner_button->setText(tr("Change Banner"));
    icon_button->setText(tr("Change Icon"));
    if (count > 0) {
        position_label->setText(tr("%1 of %2").arg(position + 1).arg(count));
    }
    RefreshHeader();
    RefreshActions();
    RefreshTabs();
    RefreshOverview();
    RefreshPanels();
    RefreshInfo();
}

void CarouselHub::changeEvent(QEvent* event) {
    if (event->type() == QEvent::LanguageChange) {
        RetranslateUI();
    } else if (event->type() == QEvent::PaletteChange) {
        // The glyphs and muted labels are derived from the palette.
        QMetaObject::invokeMethod(this, [this] { RefreshPalette(); }, Qt::QueuedConnection);
    }
    QWidget::changeEvent(event);
}
