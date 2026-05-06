#pragma once

#include <QByteArray>
#include <QColor>
#include <QIcon>
#include <QPainter>
#include <QPixmap>
#include <QSvgRenderer>

namespace dtv {
namespace ui {

// --- Theme Colors (Seer Baseline) ---

namespace Colors {
// Shared
inline constexpr const char *Accent = "#0288D1";

// Dark Theme
inline constexpr const char *DarkBG = "#121212";
inline constexpr const char *DarkSurface = "#1E1E1E";
inline constexpr const char *DarkText = "#EEEEEE";
inline constexpr const char *DarkTextDim = "#BDBDBD";
inline constexpr const char *DarkBorder = "#333333";
inline constexpr const char *DarkInput = "#1E1E1E";

// Light Theme
inline constexpr const char *LightBG = "#FFFFFF";
inline constexpr const char *LightSurface = "#F5F5F5";
inline constexpr const char *LightText = "#212121";
inline constexpr const char *LightTextDim = "#757575";
inline constexpr const char *LightBorder = "#E0E0E0";
inline constexpr const char *LightInput = "#FFFFFF";
} // namespace Colors

// --- SVGs (Material Symbols) ---

// Material Symbol: "Search"
inline constexpr auto g_svg_search = R"SVG(
<svg xmlns="http://www.w3.org/2000/svg" height="24" viewBox="0 -960 960 960" width="24">
  <path fill="currentColor" d="M784-120 532-372q-30 24-69 38t-83 14q-109 0-184.5-75.5T120-580q0-109 75.5-184.5T380-840q109 0 184.5 75.5T640-580q0 44-14 83t-38 69l252 252-56 56ZM380-400q75 0 127.5-52.5T560-580q0-75-52.5-127.5T380-760q-75 0-127.5 52.5T200-580q0 75 52.5 127.5T380-400Z"/>
</svg>)SVG";

// Material Symbol: "Info" Rounded, Outline
inline constexpr auto g_svg_info = R"SVG(
<svg xmlns="http://www.w3.org/2000/svg" height="24" viewBox="0 -960 960 960" width="24">
  <path fill="currentColor" d="M480-120q-75 0-140.5-28.5t-114-77q-48.5-48.5-77-114T120-480q0-75 28.5-140.5t77-114q48.5-48.5 114-77T480-840q75 0 140.5 28.5t114 77q48.5 48.5 77 114T840-480q0 75-28.5 140.5t-77 114q-48.5 48.5-114 77T480-120Zm0-72q120 0 204-84t84-204q0-120-84-204t-204-84q-120 0-204 84t-84 204q0 120 84 204t204 84Zm-40-101h80v-240h-80v240Zm40-327q17 0 28.5-11.5T520-660q0-17-11.5-28.5T480-700q-17 0-28.5 11.5T440-660q0 17 11.5 28.5T480-620Z"/>
</svg>)SVG";

// Material Symbol: "Table Rows" (Table icon)
inline constexpr auto g_svg_table = R"SVG(
<svg xmlns="http://www.w3.org/2000/svg" height="24" viewBox="0 -960 960 960" width="24">
  <path fill="currentColor" d="M120-120v-720h720v720H120Zm72-456h138v-192H192v192Zm210 0h138v-192H402v192Zm210 0h138v-192H612v192ZM192-396h138v-108H192v108Zm210 0h138v-108H402v108Zm210 0h138v-108H612v108ZM192-192h138v-132H192v132Zm210 0h138v-132H402v132Zm210 0h138v-132H612v132Z"/>
</svg>)SVG";

// Material Symbol: "Arrow Back" (for Table Picker return)
inline constexpr auto g_svg_arrow_back = R"SVG(
<svg xmlns="http://www.w3.org/2000/svg" height="24" viewBox="0 -960 960 960" width="24">
  <path fill="currentColor" d="m313-440 224 224-57 56-320-320 320-320 57 56-224 224h487v80H313Z"/>
</svg>)SVG";

// --- QSS (Qt Style Sheets) ---

// Placeholder args: %1: SurfaceBG, %2: Border, %3: InputBG, %4: Text, %5:
// Accent, %6: Radius, %7: PaddingV, %8: PaddingH
inline constexpr auto g_qss_top_bar = R"(
    QWidget#topBar { background-color: %1; border: none; }
    QLineEdit {
        background-color: %3; border: 1px solid %2; border-radius: %6px;
        color: %4; padding: %7px %8px; selection-background-color: %5;
    }
    QLineEdit:focus { border: 1px solid %5; }
    QPushButton {
        border: none; background: transparent; border-radius: %6px; padding: %7px;
    }
    QPushButton:hover { background-color: rgba(128, 128, 128, 40); }
    QPushButton:pressed { background-color: rgba(128, 128, 128, 60); }
)";

// Placeholder args: %1: SurfaceBG, %2: Border
inline constexpr auto g_qss_bottom_bar = R"(
    QWidget#btmBar {
        background-color: %1;
        border-top: 1px solid %2;
    }
)";

inline QIcon createMultiStateIcon(const char *data, const QColor &normalColor,
                                  const QColor &selectedColor, int iconSz = 20)
{
    QIcon icon;

    auto render = [&](const QColor &c) {
        QByteArray svg(data);
        svg.replace("currentColor", c.name(QColor::HexRgb).toUtf8());
        QSvgRenderer renderer(svg);
        QPixmap pix(iconSz, iconSz);
        pix.fill(Qt::transparent);
        QPainter p(&pix);
        p.setRenderHint(QPainter::Antialiasing);
        renderer.render(&p);
        return pix;
    };

    QPixmap normal = render(normalColor);
    QPixmap selected = render(selectedColor);

    // Normal state (Off/On)
    icon.addPixmap(normal, QIcon::Normal, QIcon::Off);
    icon.addPixmap(normal, QIcon::Normal, QIcon::On);

    // Active state (Off/On) - used when the window is focused
    icon.addPixmap(selected, QIcon::Active, QIcon::Off);
    icon.addPixmap(selected, QIcon::Active, QIcon::On);

    // Selected state (Off/On) - used when the item is selected
    icon.addPixmap(selected, QIcon::Selected, QIcon::Off);
    icon.addPixmap(selected, QIcon::Selected, QIcon::On);

    // Also add to Disabled just in case, using a dimmed version
    QColor disabledColor = normalColor;
    disabledColor.setAlpha(100);
    icon.addPixmap(render(disabledColor), QIcon::Disabled, QIcon::Off);

    return icon;
}

inline QIcon createIcon(const char *data, const QColor &color, int iconSz = 20)
{
    return createMultiStateIcon(data, color, Qt::white, iconSz);
}

} // namespace ui
} // namespace dtv
