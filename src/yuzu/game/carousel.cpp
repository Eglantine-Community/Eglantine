// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "yuzu/game/carousel.h"

#include <QEasingCurve>
#include <QAccessible>
#include <QApplication>
#include <QImage>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPixmapCache>
#include <QPolygonF>
#include <QScrollBar>
#include <QTimer>
#include <QTransform>
#include <QVariantAnimation>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <vector>

#include "qt_common/config/uisettings.h"
#include "qt_common/game_list/game_list_p.h"
#include "qt_common/game_list/model.h"
#include "qt_common/util/game_art.h"
#include "yuzu/game/common.h"
#include "yuzu/game/game_card.h"

namespace {

constexpr int SideGap = 16;
/// Room under the centered card for its title and detail rows, when game names are shown.
constexpr int TitleReserve = 64;
constexpr int MinCenterSize = 160;
/// How far each stacked card peeks past the card in front of it, as a share of the center card.
constexpr qreal PeekFraction = 0.12;
/// Size falloff per step into the stack, so the strip reads as receding.
constexpr qreal ScaleStep = 0.90;
constexpr qreal MinScale = 0.40;
/// Trapezoid tilt of the stacked cards: their far edge is this much narrower, and this much
/// shorter, than the edge facing the center.
constexpr qreal TiltSqueeze = 0.14;
constexpr qreal TiltHeight = 0.95;
/// Depth fog: the panel's Window colour at this alpha per step into the stack, the same wash the
/// details panel uses to fade banners into the surface.
constexpr int FogPerStep = 55;
constexpr int FogMax = 170;
/// The deepest stack the setting may ask for, whatever ends up in the config file.
constexpr int MaxSideCount = 12;

/// How opaque a stacked card is at `depth` with `count` cards a side shown: solid up close,
/// dissolving to nothing at the stack's edge. Cards fade out of existence as they recede and
/// fade back in at the back of the stack as they return.
qreal StackAlpha(qreal depth, int count) {
    const qreal d = std::abs(depth);
    const qreal solid = std::max<qreal>(1.0, count - 2.0);
    if (d <= solid) {
        return 1.0;
    }
    const qreal t = std::min<qreal>(1.0, (d - solid) / std::max<qreal>(1.0, count - solid));
    return 1.0 - t * t * (3.0 - 2.0 * t);
}
constexpr int FocusRingWidth = 2;
constexpr int AnimationMs = 220;
/// A hovered stacked card keeps this share of its depth fog, so it brightens under the mouse.
constexpr qreal HoverFogScale = 0.35;
/// A pressed card shrinks to this, so the press shows before anything else happens.
constexpr qreal PressScale = 0.96;
/// Pointer travel to card travel while dragging: 1 keeps the grabbed card under the pointer.
constexpr qreal DragGain = 1.0;
constexpr int ArrowDiameter = 40;
constexpr int ArrowMargin = 16;
/// The backdrop and the screen reader wait this long for the strip to stop on a game.
constexpr int SettleMs = 120;
/// How far a flick carries on after release: seconds of the release velocity.
constexpr qreal FlingSeconds = 0.22;
constexpr int MaxFlingCards = 12;
/// Icon sizes are quantized to this many pixels so a live window resize hits the scaling cache
/// instead of re-filtering every card at every pixel step.
constexpr int SizeBucket = 32;
/// Cached icon sources are capped in physical pixels: full-card-height entries would crowd Qt's
/// small default pixmap cache, and every eviction re-filters mid-animation. A card never needs
/// more detail than this; the last stretch to the card's size is a cheap per-frame smooth blit.
constexpr int MaxCenterSourcePx = 768;
constexpr int MaxStackSourcePx = 512;

QColor WithAlpha(QColor color, int alpha) {
    color.setAlpha(alpha);
    return color;
}

int SizeBucketOf(qreal side) {
    return std::max(SizeBucket * 2, int(side) / SizeBucket * SizeBucket);
}

/// Mitchell-Netravali bicubic filter: crisper than the painter's bilinear when a small icon is
/// enlarged onto a full-height card, without the ringing sharper windows produce on hard edges.
qreal MitchellWeight(qreal x) {
    constexpr qreal b = 1.0 / 3.0;
    constexpr qreal c = 1.0 / 3.0;
    x = std::abs(x);
    if (x < 1.0) {
        return ((12 - 9 * b - 6 * c) * x * x * x + (-18 + 12 * b + 6 * c) * x * x +
                (6 - 2 * b)) / 6.0;
    }
    if (x < 2.0) {
        return ((-b - 6 * c) * x * x * x + (6 * b + 30 * c) * x * x + (-12 * b - 48 * c) * x +
                (8 * b + 24 * c)) / 6.0;
    }
    return 0.0;
}

struct FilterTaps {
    int start = 0;
    std::vector<qreal> weights;
};

std::vector<FilterTaps> BuildTaps(int in_count, int out_count) {
    const qreal ratio = qreal(in_count) / qreal(out_count);
    const qreal scale = std::max<qreal>(1.0, ratio);
    const int half = int(std::ceil(2.0 * scale));
    std::vector<FilterTaps> taps(out_count);
    for (int out = 0; out < out_count; ++out) {
        const qreal center = (out + 0.5) * ratio - 0.5;
        FilterTaps& t = taps[out];
        t.start = int(std::floor(center)) - half + 1;
        qreal sum = 0.0;
        for (int i = t.start; i < t.start + 2 * half; ++i) {
            // Negative lobes ring around sharp text and logo edges when a small icon is
            // enlarged, so the kernel is kept non-negative: a touch softer, but artifact-free.
            const qreal weight = std::max<qreal>(0.0, MitchellWeight((i - center) / scale));
            t.weights.push_back(weight);
            sum += weight;
        }
        if (sum > 0.0) {
            for (qreal& weight : t.weights) {
                weight /= sum;
            }
        }
    }
    return taps;
}

/// Two separable passes with edge clamping; runs once per icon and size, never per frame.
QImage MitchellResample(const QImage& source, const QSize& target) {
    if (source.size() == target) {
        return source.convertToFormat(QImage::Format_ARGB32);
    }
    const QImage src = source.convertToFormat(QImage::Format_ARGB32);

    QImage temp(target.width(), src.height(), QImage::Format_ARGB32);
    const std::vector<FilterTaps> h_taps = BuildTaps(src.width(), target.width());
    for (int y = 0; y < src.height(); ++y) {
        const auto* row_in = reinterpret_cast<const QRgb*>(src.scanLine(y));
        auto* row_out = reinterpret_cast<QRgb*>(temp.scanLine(y));
        for (int x = 0; x < target.width(); ++x) {
            const FilterTaps& t = h_taps[x];
            qreal r = 0.0, g = 0.0, b = 0.0, a = 0.0;
            for (size_t k = 0; k < t.weights.size(); ++k) {
                const int sx = std::clamp(t.start + int(k), 0, src.width() - 1);
                const qreal w = t.weights[k];
                const QRgb color = row_in[sx];
                r += w * qRed(color);
                g += w * qGreen(color);
                b += w * qBlue(color);
                a += w * qAlpha(color);
            }
            row_out[x] = qRgba(qRound(r), qRound(g), qRound(b), qRound(a));
        }
    }

    QImage out(target.width(), target.height(), QImage::Format_ARGB32);
    const std::vector<FilterTaps> v_taps = BuildTaps(src.height(), target.height());
    for (int y = 0; y < target.height(); ++y) {
        const FilterTaps& t = v_taps[y];
        auto* row_out = reinterpret_cast<QRgb*>(out.scanLine(y));
        for (int x = 0; x < target.width(); ++x) {
            qreal r = 0.0, g = 0.0, b = 0.0, a = 0.0;
            for (size_t k = 0; k < t.weights.size(); ++k) {
                const int sy = std::clamp(t.start + int(k), 0, temp.height() - 1);
                const qreal w = t.weights[k];
                const QRgb color = reinterpret_cast<const QRgb*>(temp.scanLine(sy))[x];
                r += w * qRed(color);
                g += w * qGreen(color);
                b += w * qBlue(color);
                a += w * qAlpha(color);
            }
            row_out[x] = qRgba(qRound(r), qRound(g), qRound(b), qRound(a));
        }
    }
    return out;
}

/// The card's icon at `bucket` logical pixels square, resampled once and cached: the model's
/// decoration is pre-scaled to the icon-size setting, so enlarging it per frame with the
/// painter's filter both looked soft and cost a smooth stretch per card per frame.
QPixmap ScaledCardIcon(const QModelIndex& index, int bucket, qreal dpr, int cap_px) {
    const int px = std::min(cap_px, std::max(1, qRound(bucket * dpr)));
    // The icon the user chose through the artwork feature overrides the embedded one, as in
    // every other view; otherwise the embedded original beats the pre-scaled decoration.
    const u64 program_id = index.data(GameListItemPath::ProgramIdRole).toULongLong();
    const bool custom_icon = QtCommon::GameArt::HasIcon(program_id);
    QPixmap source;
    if (custom_icon) {
        source = index.data(Qt::DecorationRole).value<QPixmap>();
    }
    if (source.isNull()) {
        source = index.data(GameListItemPath::OriginalIconRole).value<QPixmap>();
    }
    if (source.isNull()) {
        source = index.data(Qt::DecorationRole).value<QPixmap>();
    }
    if (source.isNull()) {
        return {};
    }
    // Keyed by the pixmap itself: two rows of the same title (two dumps of one game) must not
    // share an entry, and neither must a placeholder stand in for a real icon.
    const QString key = QStringLiteral("eglantine-carousel-%1-%2-%3")
                            .arg(source.cacheKey())
                            .arg(px)
                            .arg(dpr);
    QPixmap cached;
    if (QPixmapCache::find(key, &cached)) {
        return cached;
    }
    // A custom icon's decoration is scaled down to the icon-size setting; resample the file
    // instead. The key stays the decoration's, which changes whenever the custom icon does.
    QImage full = source.toImage();
    if (custom_icon) {
        QImage original(QtCommon::GameArt::IconPath(program_id));
        if (!original.isNull()) {
            full = std::move(original);
        }
    }
    const QSize physical(px, px);
    const QSize fitted = full.size().scaled(physical, Qt::KeepAspectRatio);
    QImage square(physical, QImage::Format_ARGB32);
    square.fill(Qt::transparent);
    QPainter sq(&square);
    sq.drawImage((physical.width() - fitted.width()) / 2, (physical.height() - fitted.height()) / 2,
                 MitchellResample(full, fitted));
    QPixmap out = QPixmap::fromImage(square);
    out.setDevicePixelRatio(dpr);
    QPixmapCache::insert(key, out);
    return out;
}

struct CarouselMetrics {
    QPointF center;
    qreal card_w = 0.0;
    qreal card_h = 0.0;
    qreal peek = 0.0;
    bool names = false;
};

CarouselMetrics MetricsFor(const QSize& area) {
    CarouselMetrics m;
    m.names = UISettings::values.show_game_name.GetValue();
    const qreal title = m.names ? TitleReserve : 0.0;
    // The stored index picks a share of the available height; anything out of range reads as
    // full, so a stale value can never shrink the card away.
    const auto fractions = GameCarousel::CardSizeFractions;
    size_t size_index = UISettings::values.carousel_card_size.GetValue();
    if (size_index >= fractions.size()) {
        size_index = 0;
    }
    const qreal available = std::max<qreal>(0.0, area.height() - 2 * SideGap - title);
    m.card_h = std::max<qreal>(MinCenterSize, available * fractions[size_index]);
    // The cartridge frame is portrait, and its slot is too: the stack, the peeks and the title
    // all space a rectangle instead of a square with a rectangle painted inside it.
    const bool cartridge = UISettings::values.game_icon_borders.GetValue() &&
                           UISettings::values.game_cartridge_kind.GetValue() == 1;
    m.card_w = cartridge ? m.card_h * GameCard::CartridgeAspect : m.card_h;
    // The card floats centered in the available height at every size.
    m.center = QPointF(area.width() / 2.0, SideGap + available / 2.0);
    m.peek = std::clamp(m.card_w * PeekFraction, qreal(40), qreal(96));
    return m;
}

/// The card's untilted rectangle at `depth`: 0 is the centered card, and the card `k` steps away
/// tucks under the one in front of it, peeking `k * peek` past its edge. The tilt foreshortens a
/// stacked card, so its slot is laid out on the foreshortened width and the peek stays exact.
/// Fractional depths interpolate, so the strip slides as one piece while the position animates.
QRectF CardRect(qreal depth, const CarouselMetrics& m) {
    const qreal d = std::abs(depth);
    const qreal fade = std::min(d, 1.0);
    const qreal scale = std::max(MinScale, std::pow(ScaleStep, d));
    const qreal w = m.card_w * scale * (1.0 - TiltSqueeze * fade);
    const qreal h = m.card_h * scale;
    const qreal y = m.center.y() - h / 2.0;
    if (depth > 0.0) {
        const qreal centered = m.center.x() - m.card_w / 2.0;
        const qreal stacked = m.center.x() + m.card_w / 2.0 + m.peek - w;
        const qreal left = depth < 1.0 ? centered + (stacked - centered) * depth
                                       : stacked + (depth - 1.0) * m.peek;
        return {left, y, w, h};
    }
    if (depth < 0.0) {
        const qreal centered = m.center.x() - m.card_w / 2.0;
        const qreal stacked = m.center.x() - m.card_w / 2.0 - m.peek;
        const qreal left = depth > -1.0 ? centered + (stacked - centered) * (-depth)
                                       : stacked + (depth + 1.0) * m.peek;
        return {left, y, w, h};
    }
    return {m.center.x() - w / 2.0, y, w, h};
}

/// Where the center of a card resting at `depth` sits, joined linearly between whole depths.
/// The painted center is not monotonic just off the center slot (the card shrinks faster than it
/// moves), so a drag follows this rail instead.
qreal SlotCenterX(qreal depth, const CarouselMetrics& m) {
    const qreal lower = std::floor(depth);
    const qreal t = depth - lower;
    const qreal a = CardRect(lower, m).center().x();
    if (t < 1e-6) {
        return a;
    }
    return a + (CardRect(lower + 1.0, m).center().x() - a) * t;
}

/// The depth at which a card's rail center lands on `x`.
qreal DepthForSlotX(qreal x, const CarouselMetrics& m) {
    qreal lo = -64.0;
    qreal hi = 64.0;
    for (int i = 0; i < 48; ++i) {
        const qreal mid = (lo + hi) / 2.0;
        if (SlotCenterX(mid, m) < x) {
            lo = mid;
        } else {
            hi = mid;
        }
    }
    return (lo + hi) / 2.0;
}

void DrawArrowButton(QPainter& p, const QRectF& rect, int direction, const QPalette& pal,
                     bool hot) {
    p.save();
    p.setPen(Qt::NoPen);
    for (int i = 3; i >= 1; --i) {
        p.setBrush(QColor(0, 0, 0, 16));
        p.drawEllipse(rect.adjusted(-i, -i + 2, i, i + 2));
    }
    QColor fill = hot ? pal.color(QPalette::Highlight) : pal.color(QPalette::Window);
    fill.setAlpha(hot ? 240 : 210);
    p.setBrush(fill);
    QColor edge = pal.color(QPalette::WindowText);
    edge.setAlpha(60);
    p.setPen(QPen(edge, 1));
    p.drawEllipse(rect);
    const QColor ink = hot ? pal.color(QPalette::HighlightedText) : pal.color(QPalette::WindowText);
    p.setPen(QPen(ink, 2.2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.setBrush(Qt::NoBrush);
    const qreal s = rect.width() * 0.16;
    const QPointF c = rect.center() + QPointF(-direction * s * 0.3, 0);
    QPolygonF chevron;
    chevron << QPointF(c.x() - direction * s, c.y() - s * 1.6) << QPointF(c.x() + direction * s, c.y())
            << QPointF(c.x() - direction * s, c.y() + s * 1.6);
    p.drawPolyline(chevron);
    p.restore();
}

void DrawCardShadow(QPainter& p, const QRectF& rect, int alpha) {
    if (alpha <= 0) {
        return;
    }
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0, 0, 0, alpha));
    const qreal radius = rect.width() * 0.08;
    for (int spread = 1; spread <= 5; ++spread) {
        p.drawRoundedRect(rect.adjusted(-spread, -spread + 2, spread, spread + 2),
                          radius + spread, radius + spread);
    }
}

/// The title of the card taking the center slot, fading in as the card settles, and under it
/// the play time and where the card sits in the list.
void DrawTitle(QPainter& p, const QModelIndex& index, const CarouselMetrics& m,
               const QPalette& pal, int position, int count) {
    const QString title =
        index.data(Qt::DisplayRole).toString().split(QLatin1Char('\n')).first();
    const QFont base_font = p.font();
    QFont font = base_font;
    font.setBold(true);
    const int title_px = qRound(std::clamp(m.card_w * 0.045, qreal(13), qreal(24)));
    font.setPixelSize(title_px);
    const int text_width = qRound(m.card_w * 1.4);
    const QString line = QFontMetrics(font).elidedText(title, Qt::ElideRight, text_width);
    p.setFont(font);
    p.setPen(pal.color(QPalette::Text));
    const QRectF slot = CardRect(0.0, m);
    const qreal left = slot.center().x() - text_width / 2.0;
    const qreal title_h = QFontMetrics(font).height();
    p.drawText(QRectF(left, slot.bottom() + 6, text_width, title_h),
               Qt::AlignHCenter | Qt::AlignTop, line);

    QStringList details;
    const QString play_time = index.sibling(index.row(), GameListModel::COLUMN_PLAY_TIME)
                                  .data(Qt::DisplayRole)
                                  .toString()
                                  .trimmed();
    if (!play_time.isEmpty()) {
        details << play_time;
    }
    details << QStringLiteral("%1 / %2").arg(position + 1).arg(count);
    QFont detail_font = base_font;
    detail_font.setPixelSize(std::max(11, title_px * 2 / 3));
    p.setFont(detail_font);
    QColor muted = pal.color(QPalette::Text);
    muted.setAlpha(170);
    p.setPen(muted);
    const qreal detail_top = slot.bottom() + 6 + title_h + 2;
    p.drawText(QRectF(left, detail_top, text_width, TitleReserve - (detail_top - slot.bottom())),
               Qt::AlignHCenter | Qt::AlignTop,
               details.join(QStringLiteral("  \u00B7  ")));
    p.setFont(base_font);
}

/// Round play button on the centered card, shown while the mouse is over the card.
void DrawPlayButton(QPainter& p, const QRectF& rect, const QPalette& pal, bool hot) {
    p.save();
    p.setPen(Qt::NoPen);
    for (int i = 3; i >= 1; --i) {
        p.setBrush(QColor(0, 0, 0, 22));
        p.drawEllipse(rect.adjusted(-i, -i + 2, i, i + 2));
    }
    QColor fill = pal.color(QPalette::Highlight);
    fill = hot ? fill.lighter(115) : fill;
    fill.setAlpha(hot ? 255 : 225);
    p.setBrush(fill);
    p.drawEllipse(rect);
    // The triangle sits a little right of center so it looks centered.
    const qreal s = rect.width() * 0.36;
    const QPointF c = rect.center() + QPointF(s * 0.12, 0);
    QPolygonF triangle;
    triangle << QPointF(c.x() - s * 0.42, c.y() - s * 0.5) << QPointF(c.x() + s * 0.5, c.y())
             << QPointF(c.x() - s * 0.42, c.y() + s * 0.5);
    p.setBrush(pal.color(QPalette::HighlightedText));
    p.drawPolygon(triangle);
    p.restore();
}

/// A stacked cartridge card: the label art and the frame under one tilt, drawn full body. The
/// frame is cached per bucket, so the extra blit area over a strip costs nothing.
void DrawStackedCartridge(QPainter& p, const QPixmap& icon, const QRectF& slot, qreal depth,
                          qreal dpr, const QPalette& pal, qreal fog_scale) {
    const qreal d = std::abs(depth);
    const QRectF& art = slot;
    const qreal fade = std::min(d, 1.0);
    const qreal far_h = art.height() * (1.0 + (TiltHeight - 1.0) * fade);
    const qreal yc = art.center().y();
    const qreal aw = art.width();
    const qreal ah = art.height();
    QPolygonF one{QPointF(0, 0), QPointF(aw, 0), QPointF(aw, ah), QPointF(0, ah)};
    QPolygonF target;
    // The whole cartridge maps across its slot: the edge facing the center keeps its height,
    // the far edge is shorter and reads as receding.
    if (depth > 0.0) {
        target << QPointF(art.left(), art.top()) << QPointF(art.right(), yc - far_h / 2)
               << QPointF(art.right(), yc + far_h / 2) << QPointF(art.left(), art.bottom());
    } else {
        target << QPointF(art.left(), yc - far_h / 2) << QPointF(art.right(), art.top())
               << QPointF(art.right(), art.bottom()) << QPointF(art.left(), yc + far_h / 2);
    }
    QTransform tilt;
    if (!QTransform::quadToQuad(one, target, tilt)) {
        return;
    }
    const QPixmap frame = GameCard::CartridgePixmap(qRound(ah), dpr);
    if (frame.isNull()) {
        return;
    }
    p.save();
    p.setTransform(tilt);
    // The frame's slot carries a transparent margin and a faint baked shadow around the
    // solid body. Clip and fog the body itself: washing the whole slot paints a translucent
    // border around every stacked card, and a clip wider than the frame's own ~8%-width
    // corners trims them.
    const QRectF body = GameCard::CartridgeBodyRect(QRectF(0, 0, aw, ah));
    const qreal body_radius =
        GameCard::CartridgeBodyRadius * aw / GameCard::CartridgeBase.width();
    QPainterPath clip;
    clip.addRoundedRect(body, body_radius, body_radius);
    p.setClipPath(clip);
    p.drawPixmap(QRectF(0, 0, aw, ah), frame,
                 QRectF(0, 0, frame.width(), frame.height()));
    const QRectF label = GameCard::CartridgeLabelRect(QRectF(0, 0, aw, ah));
    p.save();
    QPainterPath label_clip;
    label_clip.addRoundedRect(label, label.width() * 0.04, label.width() * 0.04);
    p.setClipPath(label_clip);
    GameCard::DrawCoverCropped(p, icon, label);
    p.restore();
    const int fog = qRound(std::min(FogMax, qRound(d * FogPerStep)) * fog_scale);
    p.fillRect(body, WithAlpha(pal.color(QPalette::Window), fog));
    p.restore();
}

/// One card at `depth`. Every visual parameter is continuous in the depth, so a card sliding out
/// of the center shrinks, tilts and fogs in instead of snapping between two shapes.
void DrawCard(QPainter& p, const QModelIndex& index, const QRectF& rect, qreal depth,
              const QPalette& pal, bool focused, bool borders, qreal dpr, qreal peek,
              int center_bucket, int stack_bucket, qreal alpha, qreal fog_scale) {
    const qreal d = std::abs(depth);
    const bool centered = d < 0.01;
    const QPixmap icon =
        ScaledCardIcon(index, centered ? center_bucket : stack_bucket, dpr,
                       centered ? MaxCenterSourcePx : MaxStackSourcePx);
    if (icon.isNull()) {
        return;
    }
    const bool cartridge = borders && UISettings::values.game_cartridge_kind.GetValue() == 1;
    // The cartridge's own solid frame is its outline: a translucent shadow halo or a
    // selection ring drawn around it reads as an ugly transparent border, so neither is
    // drawn in cartridge mode.
    if (d < 1.0 && !cartridge) {
        // Only the card taking the center slot casts the full shadow.
        DrawCardShadow(p, rect, int((1.0 - d) * 13));
    }

    p.save();
    if (alpha < 1.0) {
        p.setOpacity(alpha);
    }
    const int side = icon.width();

    if (centered) {
        if (cartridge) {
            const QRectF& cart = rect;
            const QPixmap frame = GameCard::CartridgePixmap(qRound(cart.height()), dpr);
            if (!frame.isNull()) {
                // The frame's slot carries a transparent margin and a faint baked shadow
                // around the solid body; clip to the body so the silhouette matches the
                // stacked cards and nothing translucent hugs the card. The frame goes down
                // first and the game art sits on its label, cover-cropped.
                const QRectF body = GameCard::CartridgeBodyRect(cart);
                const qreal body_radius =
                    GameCard::CartridgeBodyRadius * cart.width() /
                    GameCard::CartridgeBase.width();
                QPainterPath body_clip;
                body_clip.addRoundedRect(body, body_radius, body_radius);
                p.setClipPath(body_clip);
                // The frame first, the game art on its label: the label design shows through
                // the art's transparent regions.
                p.drawPixmap(cart, frame, QRectF(0, 0, frame.width(), frame.height()));
                const QRectF label = GameCard::CartridgeLabelRect(cart);
                p.save();
                QPainterPath label_clip;
                label_clip.addRoundedRect(label, label.width() * 0.04, label.width() * 0.04);
                p.setClipPath(label_clip);
                GameCard::DrawCoverCropped(p, icon, label);
                p.restore();
            }
        } else {
            QPainterPath clip;
            clip.addRoundedRect(rect, rect.width() * 0.08, rect.width() * 0.08);
            p.setClipPath(clip);
            p.drawPixmap(rect.toRect(), icon);
            if (borders) {
                p.drawPixmap(rect.toRect(),
                             GameCard::PlatformBorderPixmap(
                                 index.data(GameListItemPath::Switch2Role).toBool(),
                                 qRound(rect.width()), dpr));
            }
        }
    } else if (cartridge) {
        DrawStackedCartridge(p, icon, rect, depth, dpr, pal, fog_scale);
    } else {
        const qreal fade = std::min(d, 1.0);
        const qreal far_h = rect.height() * (1.0 + (TiltHeight - 1.0) * fade);
        const qreal yc = rect.center().y();
        QPolygonF one{QPointF(0, 0), QPointF(side, 0), QPointF(side, side), QPointF(0, side)};
        QPolygonF target;
        // The whole icon maps across the card's slot: the edge facing the center keeps its
        // height, the far edge is shorter and reads as receding.
        if (depth > 0.0) {
            target << QPointF(rect.left(), rect.top()) << QPointF(rect.right(), yc - far_h / 2)
                   << QPointF(rect.right(), yc + far_h / 2) << QPointF(rect.left(), rect.bottom());
        } else {
            target << QPointF(rect.left(), yc - far_h / 2) << QPointF(rect.right(), rect.top())
                   << QPointF(rect.right(), rect.bottom())
                   << QPointF(rect.left(), yc + far_h / 2);
        }
        QTransform tilt;
        if (QTransform::quadToQuad(one, target, tilt)) {
            p.setTransform(tilt);
            QPainterPath clip;
            clip.addRoundedRect(QRectF(0, 0, side, side), side * 0.08, side * 0.08);
            p.setClipPath(clip);
            // Only the far-end sliver of a stacked card can be seen; the rest sits under the
            // card in front, so drawing that strip alone keeps the resampling cheap. A card
            // crossing the center slot has nothing in front of it and needs its whole body;
            // a settled card needs just its peek, with the wider margin covering the gap a
            // slide opens between stack neighbours.
            qreal margin;
            if (d < 1.0) {
                margin = rect.width();
            } else {
                const qreal frac = std::abs(d - std::round(d));
                margin = frac < 0.02 ? peek + 4.0 : 2.5 * peek + 8.0;
            }
            const qreal strip = std::min(qreal(side), margin / rect.width() * side);
            const QRectF src = depth > 0.0 ? QRectF(side - strip, 0.0, strip, side)
                                           : QRectF(0.0, 0.0, strip, side);
            p.drawPixmap(src, icon, src);
            if (borders) {
                p.drawPixmap(src,
                             GameCard::PlatformBorderPixmap(
                                 index.data(GameListItemPath::Switch2Role).toBool(), side, dpr),
                             src);
            }
            const int fog = qRound(std::min(FogMax, qRound(d * FogPerStep)) * fog_scale);
            p.fillRect(src, WithAlpha(pal.color(QPalette::Window), fog));
        }
    }
    p.restore();

    if (centered && focused && !cartridge) {
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(pal.color(QPalette::Highlight), FocusRingWidth));
        p.drawRoundedRect(rect.adjusted(-1, -1, 1, 1), rect.width() * 0.08 + 1,
                          rect.width() * 0.08 + 1);
    }
}

} // namespace

GameCarousel::GameCarousel(QWidget* parent) : QListView{parent} {
    setSelectionMode(QAbstractItemView::SingleSelection);
    setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setEditTriggers(QAbstractItemView::NoEditTriggers);
    setContextMenuPolicy(Qt::CustomContextMenu);
    setFocusPolicy(Qt::StrongFocus);

    // Full-height cards mean large icon entries; Qt's small default pixmap cache would evict
    // them mid-animation and re-filter every frame. Raise it once, never lower it.
    if (QPixmapCache::cacheLimit() < 65536) {
        QPixmapCache::setCacheLimit(65536);
    }

    m_position_anim = new QVariantAnimation(this);
    m_position_anim->setDuration(AnimationMs);
    m_position_anim->setEasingCurve(QEasingCurve::OutCubic);
    connect(m_position_anim, &QVariantAnimation::valueChanged, this,
            [this](const QVariant& value) {
                m_position = value.toReal();
                viewport()->update();
            });

    viewport()->setMouseTracking(true);

    m_settle_timer = new QTimer(this);
    m_settle_timer->setSingleShot(true);
    m_settle_timer->setInterval(SettleMs);
    connect(m_settle_timer, &QTimer::timeout, this, [this] {
        emit CenteredGameChanged(CenteredProgramId());
        Announce();
    });
    setAccessibleName(tr("Games"));
}

void GameCarousel::SetModel(GameListModel* model) {
    QListView::setModel(model);
    // A repopulate replaces the rows, so the stored filter has to run over the new ones. The
    // centered game is remembered before its row goes, so the reload does not land elsewhere.
    const auto remember_center = [this] {
        if (m_restore_path.isEmpty() && !m_visible.empty() && this->model()) {
            m_restore_path = this->model()
                                 ->index(m_visible[CurrentPosition()], 0)
                                 .data(GameListItemPath::FullPathRole)
                                 .toString();
        }
    };
    connect(model, &QAbstractItemModel::rowsAboutToBeRemoved, this, remember_center);
    connect(model, &QAbstractItemModel::modelAboutToBeReset, this, remember_center);
    connect(model, &GameListModel::PopulatingStarted, this, [this] { m_populating = true; });
    connect(model, &GameListModel::PopulatingCompleted, this, [this] {
        m_populating = false;
        ScheduleRelayout();
    });
    connect(model, &QAbstractItemModel::rowsInserted, this, [this] { ScheduleRelayout(); });
    connect(model, &QAbstractItemModel::rowsRemoved, this, [this] { ScheduleRelayout(); });
    connect(model, &QAbstractItemModel::modelReset, this, [this] { ScheduleRelayout(); });
    ApplyFilter({}, model);
}

void GameCarousel::ApplyFilter(const QString& edit_filter_text, GameListModel* model) {
    m_filter_text = edit_filter_text;
    m_visible.clear();
    for (int row = 0; row < model->rowCount(); ++row) {
        QStandardItem* item = model->item(row, 0);
        if (!item || Yuzu::FilterMatches(edit_filter_text, item)) {
            m_visible.push_back(row);
        }
    }
    emit FilterResultReady(int(m_visible.size()), model->rowCount());

    int restored = -1;
    if (!m_restore_path.isEmpty()) {
        for (int position = 0; position < int(m_visible.size()); ++position) {
            if (model->index(m_visible[position], 0).data(GameListItemPath::FullPathRole) ==
                m_restore_path) {
                restored = position;
                break;
            }
        }
        if (restored < 0 && m_populating) {
            // Still loading: leave the position, selection and backdrop until the game returns.
            viewport()->update();
            return;
        }
        m_restore_path.clear();
    }
    m_position_anim->stop();
    m_pending_center = -1;
    if (m_visible.empty()) {
        m_position = 0.0;
        viewport()->update();
        emit CenteredGameChanged(0);
        return;
    }
    // The selected game stays centered when the filter keeps it. Otherwise the game left in the
    // center becomes the selection, so Enter and the sidebar act on the card on screen.
    const QModelIndex current = currentIndex();
    const int kept = restored >= 0 ? -1 : current.isValid() ? PositionOf(current.row()) : -1;
    const int position =
        restored >= 0 ? restored
        : kept >= 0   ? kept
                      : std::clamp(int(std::lround(m_position)), 0, int(m_visible.size()) - 1);
    m_position = position;
    if (kept < 0) {
        setCurrentIndex(model->index(m_visible[position], 0));
    }
    viewport()->update();
    emit CenteredGameChanged(CenteredProgramId());
}

u64 GameCarousel::CenteredProgramId() const {
    if (m_visible.empty() || !model()) {
        return 0;
    }
    const int index = m_pending_center >= 0 && m_pending_center < int(m_visible.size())
                          ? m_pending_center
                          : std::clamp(int(std::lround(m_position)), 0,
                                       int(m_visible.size()) - 1);
    return model()->index(m_visible[index], 0)
        .data(GameListItemPath::ProgramIdRole)
        .toULongLong();
}

void GameCarousel::UpdateIconSize() {
    // The card sizes follow the viewport, so there is nothing to recompute.
    viewport()->update();
}

int GameCarousel::PositionOf(int row) const {
    const auto it = std::find(m_visible.begin(), m_visible.end(), row);
    return it == m_visible.end() ? -1 : int(it - m_visible.begin());
}

int GameCarousel::CurrentPosition() const {
    const QModelIndex current = currentIndex();
    if (current.isValid()) {
        if (const int position = PositionOf(current.row()); position >= 0) {
            return position;
        }
    }
    if (m_pending_center >= 0 && m_pending_center < int(m_visible.size())) {
        return m_pending_center;
    }
    return std::clamp(int(std::lround(m_position)), 0, std::max(0, int(m_visible.size()) - 1));
}

void GameCarousel::MoveTo(int position, int duration_ms) {
    if (m_visible.empty()) {
        m_position = 0.0;
        viewport()->update();
        return;
    }
    position = std::clamp(position, 0, int(m_visible.size()) - 1);
    if (qFuzzyCompare(m_position, qreal(position))) {
        m_position = position;
        viewport()->update();
        return;
    }
    m_position_anim->stop();
    m_position_anim->setDuration(duration_ms < 0 ? AnimationMs : duration_ms);
    m_position_anim->setEasingCurve(QEasingCurve::OutCubic);
    m_position_anim->setKeyValues({{0.0, m_position}, {1.0, qreal(position)}});
    // The backdrop follows the slide's destination, not the games it passes, and only once
    // key repeats or wheel steps stop picking a new one.
    m_pending_center = position;
    m_settle_timer->start();
    // The card settling into the center slot swaps to the center-sized icon; warm it now so
    // the last frame of the slide does not hitch on its first resample.
    if (model() && position >= 0 && position < int(m_visible.size())) {
        const CarouselMetrics metrics = MetricsFor(viewport()->size());
        ScaledCardIcon(model()->index(m_visible[position], 0), SizeBucketOf(metrics.card_w),
                       devicePixelRatioF(), MaxCenterSourcePx);
    }
    m_position_anim->start();
}

void GameCarousel::Bounce(int direction) {
    if (m_visible.empty() || m_dragging ||
        m_position_anim->state() == QAbstractAnimation::Running) {
        return;
    }
    const qreal rest = std::round(m_position);
    m_position_anim->setDuration(260);
    m_position_anim->setEasingCurve(QEasingCurve::InOutSine);
    m_position_anim->setKeyValues(
        {{0.0, rest}, {0.35, rest + 0.15 * direction}, {1.0, rest}});
    m_position_anim->start();
}

void GameCarousel::ScheduleRelayout() {
    if (m_relayout_queued) {
        return;
    }
    m_relayout_queued = true;
    QMetaObject::invokeMethod(
        this,
        [this] {
            m_relayout_queued = false;
            m_pending_center = -1;
            if (auto* model = qobject_cast<GameListModel*>(this->model())) {
                ApplyFilter(m_filter_text, model);
            }
        },
        Qt::QueuedConnection);
}

void GameCarousel::paintEvent(QPaintEvent*) {
    QPainter p(viewport());
    if (m_visible.empty() || !model()) {
        return;
    }
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::SmoothPixmapTransform);

    const CarouselMetrics m = MetricsFor(viewport()->size());
    const bool borders = UISettings::values.game_icon_borders.GetValue();
    const bool focused = hasFocus();
    const qreal dpr = p.device() ? p.device()->devicePixelRatio() : 1.0;
    const int center_bucket = SizeBucketOf(m.card_w);
    const int stack_bucket = SizeBucketOf(m.card_w * ScaleStep);

    // Draw the farthest cards first so the centered card sits on top of the stack.
    struct Card {
        int position;
        qreal depth;
    };
    std::vector<Card> cards;
    const int max_depth =
        std::clamp(int(UISettings::values.carousel_side_count.GetValue()), 1, MaxSideCount);
    const int base = int(std::floor(m_position));
    for (int position = std::max(0, base - max_depth - 1);
         position <= std::min<int>(int(m_visible.size()) - 1, base + max_depth + 1); ++position) {
        cards.push_back({position, position - m_position});
    }
    std::sort(cards.begin(), cards.end(), [](const Card& a, const Card& b) {
        if (!qFuzzyCompare(std::abs(a.depth), std::abs(b.depth))) {
            return std::abs(a.depth) > std::abs(b.depth);
        }
        // While sliding, the outgoing card passes under the incoming one.
        return a.depth > b.depth;
    });
    for (const Card& card : cards) {
        const QRectF rect = CardRect(card.depth, m);
        // A sliver past the viewport edge cannot be seen; skipping it also skips its icon fetch.
        if (rect.right() < 0.0 || rect.left() >= viewport()->width()) {
            continue;
        }
        const qreal alpha = StackAlpha(card.depth, max_depth);
        if (alpha <= 0.0) {
            continue;
        }
        const QModelIndex index = model()->index(m_visible[card.position], 0);
        const bool hovered = card.position == m_hover_position && !m_dragging;
        const bool pressed = m_pressed && !m_dragging && !m_caught && m_press_index.isValid() &&
                             card.position == PositionOf(m_press_index.row());
        QRectF shown = rect;
        if (pressed) {
            shown = QRectF(QPointF(), rect.size() * PressScale);
            shown.moveCenter(rect.center());
        }
        DrawCard(p, index, shown, card.depth, palette(), focused, borders, dpr, m.peek,
                 center_bucket, stack_bucket, alpha, hovered ? HoverFogScale : 1.0);
    }

    // The play button belongs to the settled center card under the mouse.
    const int center = std::clamp(int(std::lround(m_position)), 0, int(m_visible.size()) - 1);
    if (!m_dragging && m_hover_position == center &&
        std::abs(m_position - center) < 0.02) {
        DrawPlayButton(p, PlayButtonRect(), palette(), m_hover_play);
    }

    if (m_mouse_inside && !m_dragging) {
        for (const int direction : {-1, 1}) {
            if (ArrowEnabled(direction)) {
                DrawArrowButton(p, ArrowRect(direction), direction, palette(),
                                m_hover_arrow == direction);
            }
        }
    }

    if (m.names) {
        // The title belongs to the card settling into the center slot and fades with it.
        const auto nearest = std::min_element(
            cards.begin(), cards.end(), [](const Card& a, const Card& b) {
                return std::abs(a.depth) < std::abs(b.depth);
            });
        if (nearest != cards.end() && std::abs(nearest->depth) < 0.5) {
            p.setOpacity(std::max<qreal>(0.0, 1.0 - 2.0 * std::abs(nearest->depth)));
            DrawTitle(p, model()->index(m_visible[nearest->position], 0), m, palette(),
                      nearest->position, int(m_visible.size()));
            p.setOpacity(1.0);
        }
    }
}

QModelIndex GameCarousel::indexAt(const QPoint& point) const {
    if (m_visible.empty() || !model()) {
        return {};
    }
    const CarouselMetrics m = MetricsFor(viewport()->size());
    // The cartridge slot carries a transparent margin around the solid body, and the next
    // card's art shows through it: hit-test the body, or a click on what looks like the
    // neighbour's cartridge lands on the centered card's empty margin and selects nothing.
    const bool cartridge = UISettings::values.game_icon_borders.GetValue() &&
                           UISettings::values.game_cartridge_kind.GetValue() == 1;
    // Only cards paintEvent draws can be hit: a faded-out card past the stack is not there.
    const int max_depth =
        std::clamp(int(UISettings::values.carousel_side_count.GetValue()), 1, MaxSideCount);
    int best = -1;
    qreal best_depth = 0.0;
    for (int position = 0; position < int(m_visible.size()); ++position) {
        const qreal depth = position - m_position;
        if (std::abs(depth) > max_depth + 1 || StackAlpha(depth, max_depth) <= 0.0) {
            continue;
        }
        const QRectF hit = cartridge ? GameCard::CartridgeBodyRect(CardRect(depth, m))
                                     : CardRect(depth, m);
        if (hit.contains(QPointF(point)) &&
            (best < 0 || std::abs(depth) < std::abs(best_depth))) {
            best = position;
            best_depth = depth;
        }
    }
    return best < 0 ? QModelIndex{} : model()->index(m_visible[best], 0);
}

QRect GameCarousel::visualRect(const QModelIndex& index) const {
    if (!index.isValid()) {
        return {};
    }
    const int position = PositionOf(index.row());
    if (position < 0) {
        return {};
    }
    const CarouselMetrics m = MetricsFor(viewport()->size());
    return CardRect(position - m_position, m).toAlignedRect();
}

bool GameCarousel::isIndexHidden(const QModelIndex& index) const {
    return PositionOf(index.row()) < 0;
}

void GameCarousel::scrollTo(const QModelIndex& index, ScrollHint) {
    if (!index.isValid()) {
        return;
    }
    const int position = PositionOf(index.row());
    if (position >= 0) {
        MoveTo(position);
    }
}

QModelIndex GameCarousel::moveCursor(CursorAction cursor_action, Qt::KeyboardModifiers) {
    if (m_visible.empty() || !model()) {
        return {};
    }
    const int from = CurrentPosition();
    int position = from;
    int direction = 0;
    switch (cursor_action) {
    case MoveLeft:
    case MoveUp:
        position = std::max(0, position - 1);
        direction = -1;
        break;
    case MoveRight:
    case MoveDown:
        position = std::min(int(m_visible.size()) - 1, position + 1);
        direction = 1;
        break;
    case MoveHome:
        position = 0;
        break;
    case MoveEnd:
        position = int(m_visible.size()) - 1;
        break;
    case MovePageUp:
        position = std::max(0, position - 4);
        direction = -1;
        break;
    case MovePageDown:
        position = std::min(int(m_visible.size()) - 1, position + 4);
        direction = 1;
        break;
    default:
        break;
    }
    if (position == from && direction != 0) {
        Bounce(direction);
    }
    return model()->index(m_visible[position], 0);
}

void GameCarousel::setSelection(const QRect& rect, QItemSelectionModel::SelectionFlags command) {
    // A carousel cannot be rubber-banded: the card under the press is the card selected.
    const QModelIndex index = indexAt(rect.center());
    if (index.isValid()) {
        selectionModel()->select(index, command);
    }
}

QRegion GameCarousel::visualRegionForSelection(const QItemSelection& selection) const {
    QRegion region;
    for (const QItemSelectionRange& range : selection) {
        for (int row = range.top(); row <= range.bottom(); ++row) {
            const QRect rect = visualRect(model()->index(row, 0));
            if (!rect.isNull()) {
                region += rect;
            }
        }
    }
    return region;
}

void GameCarousel::wheelEvent(QWheelEvent* event) {
    if (m_visible.empty() || !model()) {
        QListView::wheelEvent(event);
        return;
    }
    // The strip is one row deep: the wheel steps between games instead of scrolling. Trackpads
    // and smooth wheels send many small deltas, so travel adds up to one step per notch.
    constexpr int Notch = 120;
    const int delta = event->angleDelta().x() + event->angleDelta().y();
    if (delta == 0) {
        event->ignore();
        return;
    }
    if ((delta > 0) != (m_wheel_accum > 0)) {
        m_wheel_accum = 0;
    }
    m_wheel_accum += delta;
    const int steps = m_wheel_accum / Notch;
    m_wheel_accum -= steps * Notch;
    event->accept();
    if (steps == 0) {
        return;
    }
    const int from = CurrentPosition();
    const int position = std::clamp(from - steps, 0, int(m_visible.size()) - 1);
    if (position == from) {
        Bounce(steps > 0 ? -1 : 1);
        return;
    }
    setCurrentIndex(model()->index(m_visible[position], 0));
}

void GameCarousel::updateGeometries() {
    // The cards follow m_position, not QListView's item layout. Without a scroll range nothing
    // can shift the viewport's pixels under the painted strip.
    QAbstractItemView::updateGeometries();
    horizontalScrollBar()->setRange(0, 0);
    verticalScrollBar()->setRange(0, 0);
}

void GameCarousel::scrollContentsBy(int, int) {
    viewport()->update();
}

void GameCarousel::currentChanged(const QModelIndex& current, const QModelIndex& previous) {
    QListView::currentChanged(current, previous);
    if (!current.isValid()) {
        return;
    }
    const int position = PositionOf(current.row());
    if (position >= 0) {
        MoveTo(position);
    }
}

QRectF GameCarousel::PlayButtonRect() const {
    const CarouselMetrics m = MetricsFor(viewport()->size());
    const QRectF card = CardRect(0.0, m);
    const qreal diameter = std::clamp(m.card_w * 0.17, qreal(44), qreal(72));
    const qreal bottom = card.bottom() - std::max<qreal>(12.0, m.card_h * 0.07);
    return {card.center().x() - diameter / 2.0, bottom - diameter, diameter, diameter};
}

QRectF GameCarousel::ArrowRect(int direction) const {
    const CarouselMetrics m = MetricsFor(viewport()->size());
    const qreal x = direction < 0 ? ArrowMargin : viewport()->width() - ArrowMargin - ArrowDiameter;
    return {x, m.center.y() - ArrowDiameter / 2.0, qreal(ArrowDiameter), qreal(ArrowDiameter)};
}

bool GameCarousel::ArrowEnabled(int direction) const {
    if (m_visible.size() < 2) {
        return false;
    }
    const int position = CurrentPosition();
    return direction < 0 ? position > 0 : position < int(m_visible.size()) - 1;
}

int GameCarousel::ArrowAt(const QPoint& pos) const {
    if (!m_mouse_inside) {
        return 0;
    }
    for (const int direction : {-1, 1}) {
        if (ArrowEnabled(direction) && ArrowRect(direction).contains(QPointF(pos))) {
            return direction;
        }
    }
    return 0;
}

void GameCarousel::Announce() {
    if (!QAccessible::isActive() || !hasFocus() || m_visible.empty() || !model()) {
        return;
    }
    const int position = CurrentPosition();
    const QString title = model()
                              ->index(m_visible[position], 0)
                              .data(Qt::DisplayRole)
                              .toString()
                              .split(QLatin1Char('\n'))
                              .first();
    const QString text =
        tr("%1, %2 of %3").arg(title).arg(position + 1).arg(int(m_visible.size()));
    setAccessibleDescription(text);
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
    QAccessibleAnnouncementEvent event(this, text);
#else
    QAccessibleEvent event(this, QAccessible::DescriptionChanged);
#endif
    QAccessible::updateAccessibility(&event);
}

void GameCarousel::UpdateHover(const QPoint& pos) {
    // The arrows show while the mouse is over the view.
    const bool entered = !m_mouse_inside;
    m_mouse_inside = true;
    const int arrow = ArrowAt(pos);
    const QModelIndex index = arrow != 0 ? QModelIndex{} : indexAt(pos);
    const int position = index.isValid() ? PositionOf(index.row()) : -1;
    const bool play = position >= 0 && position == CurrentPosition() &&
                      PlayButtonRect().contains(QPointF(pos));
    viewport()->setCursor(position >= 0 || arrow != 0 ? Qt::PointingHandCursor
                                                      : Qt::ArrowCursor);
    if (entered || position != m_hover_position || play != m_hover_play ||
        arrow != m_hover_arrow) {
        m_hover_position = position;
        m_hover_play = play;
        m_hover_arrow = arrow;
        viewport()->update();
    }
}

bool GameCarousel::viewportEvent(QEvent* event) {
    if (event->type() == QEvent::Leave && !m_pressed) {
        m_mouse_inside = false;
        m_hover_position = -1;
        m_hover_play = false;
        m_hover_arrow = 0;
        viewport()->update();
    }
    return QListView::viewportEvent(event);
}

void GameCarousel::mousePressEvent(QMouseEvent* event) {
    // Other buttons keep the stock behaviour: a right press selects the card for its menu.
    if (event->button() != Qt::LeftButton || m_visible.empty()) {
        QListView::mousePressEvent(event);
        return;
    }
    setFocus(Qt::MouseFocusReason);
    m_pressed = true;
    m_dragging = false;
    m_press_pos = event->position().toPoint();
    m_press_arrow = ArrowAt(m_press_pos);
    // A press on a strip still travelling (a flick, a long jump) stops it where it is, as a
    // finger would. The tail of a one-card slide is left to finish, so quick clicks still land.
    m_caught = false;
    if (m_press_arrow == 0 && m_position_anim->state() == QAbstractAnimation::Running &&
        std::abs(m_position_anim->endValue().toReal() - m_position) > 0.5) {
        m_position_anim->stop();
        m_caught = true;
    }
    m_press_index = m_press_arrow != 0 || m_caught ? QPersistentModelIndex{}
                                                   : QPersistentModelIndex(indexAt(m_press_pos));
    viewport()->update();
    event->accept();
}

void GameCarousel::mouseMoveEvent(QMouseEvent* event) {
    const QPoint pos = event->position().toPoint();
    if (!m_pressed || !(event->buttons() & Qt::LeftButton)) {
        UpdateHover(pos);
        return;
    }
    if (m_press_arrow != 0) {
        return;
    }
    const CarouselMetrics m = MetricsFor(viewport()->size());
    if (!m_dragging) {
        if (std::abs(pos.x() - m_press_pos.x()) < QApplication::startDragDistance()) {
            return;
        }
        // The drag holds the card under the press, or the centered one from empty space, and
        // picks the strip up where it is, mid-slide included.
        m_dragging = true;
        m_position_anim->stop();
        const QModelIndex grabbed = indexAt(m_press_pos);
        const int grabbed_position = grabbed.isValid() ? PositionOf(grabbed.row()) : -1;
        m_grab_position = grabbed_position >= 0
                              ? grabbed_position
                              : std::clamp(int(std::lround(m_position)), 0,
                                           int(m_visible.size()) - 1);
        m_grab_x = SlotCenterX(m_grab_position - m_position, m);
        m_press_pos = pos;
        m_last_click_index = QPersistentModelIndex{};
        m_drag_samples.clear();
        m_drag_clock.start();
        viewport()->setCursor(Qt::ClosedHandCursor);
    }
    const qreal target_x = m_grab_x + (pos.x() - m_press_pos.x()) * DragGain;
    const qreal raw = m_grab_position - DepthForSlotX(target_x, m);
    // Past either end the strip follows with growing resistance, then springs back on release.
    const qreal last = qreal(m_visible.size() - 1);
    const auto resist = [](qreal over) { return 0.5 * over / (1.0 + over); };
    m_position = raw < 0.0 ? -resist(-raw) : raw > last ? last + resist(raw - last) : raw;

    m_drag_samples.push_back({m_drag_clock.elapsed(), raw});
    // Only the last ~100 ms decide the fling.
    while (m_drag_samples.size() > 2 &&
           m_drag_samples.back().ms - m_drag_samples.front().ms > 100) {
        m_drag_samples.erase(m_drag_samples.begin());
    }
    // The backdrop waits for the release: it would flicker through every game passed.
    m_pending_center = std::clamp(int(std::lround(m_position)), 0, int(m_visible.size()) - 1);
    viewport()->update();
}

void GameCarousel::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton) {
        QListView::mouseReleaseEvent(event);
        return;
    }
    if (!m_pressed) {
        // The release after a double-click.
        return;
    }
    m_pressed = false;
    const QPoint pos = event->position().toPoint();
    viewport()->update();

    if (m_dragging) {
        m_dragging = false;
        qreal velocity = 0.0;
        if (m_drag_samples.size() >= 2) {
            const DragSample& first = m_drag_samples.front();
            const DragSample& last = m_drag_samples.back();
            // A pause before letting go is a placement, not a flick.
            if (m_drag_clock.elapsed() - last.ms < 60 && last.ms > first.ms) {
                velocity = (last.position - first.position) * 1000.0 / (last.ms - first.ms);
            }
        }
        const qreal carry = std::clamp(velocity * FlingSeconds, qreal(-MaxFlingCards),
                                       qreal(MaxFlingCards));
        const int target = std::clamp(int(std::lround(m_position + carry)), 0,
                                      int(m_visible.size()) - 1);
        const int duration = std::min(650, AnimationMs + 50 * int(std::abs(target - m_position)));
        setCurrentIndex(model()->index(m_visible[target], 0));
        MoveTo(target, duration);
        UpdateHover(pos);
        return;
    }

    if (m_caught) {
        // Settle on the nearest card; the press only stopped the strip.
        m_caught = false;
        m_last_click_index = QPersistentModelIndex{};
        const int target =
            std::clamp(int(std::lround(m_position)), 0, int(m_visible.size()) - 1);
        setCurrentIndex(model()->index(m_visible[target], 0));
        MoveTo(target);
        return;
    }

    if (m_press_arrow != 0) {
        const int direction = m_press_arrow;
        m_press_arrow = 0;
        m_last_click_index = QPersistentModelIndex{};
        if (ArrowAt(pos) == direction) {
            const int target = std::clamp(CurrentPosition() + direction, 0,
                                          int(m_visible.size()) - 1);
            setCurrentIndex(model()->index(m_visible[target], 0));
        }
        UpdateHover(pos);
        return;
    }

    const QModelIndex index = indexAt(pos);
    if (!index.isValid() || index != QModelIndex(m_press_index)) {
        m_last_click_index = QPersistentModelIndex{};
        if (!index.isValid() && !m_press_index.isValid()) {
            emit BackgroundClicked();
        }
        return;
    }
    m_last_click_index = index;
    const int position = PositionOf(index.row());
    m_last_click_center = position == CurrentPosition() && std::abs(m_position - position) < 0.05;
    if (!m_last_click_center) {
        // A side card, or one still sliding in: bring it to the center, nothing more.
        setCurrentIndex(index);
        UpdateHover(pos);
        return;
    }
    if (PlayButtonRect().contains(QPointF(pos))) {
        m_last_click_index = QPersistentModelIndex{};
        emit LaunchRequested(index);
        return;
    }
    emit clicked(index);
}

void GameCarousel::mouseDoubleClickEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton) {
        QListView::mouseDoubleClickEvent(event);
        return;
    }
    // Only the card that was already centered launches. A double-click on a side card just
    // centers it, its first click already did, so quick browsing never starts a game.
    m_pressed = false;
    if (m_last_click_index.isValid() && m_last_click_center) {
        const QModelIndex index = m_last_click_index;
        m_last_click_index = QPersistentModelIndex{};
        emit doubleClicked(index);
    }
}
