#include "StateColor.hpp"
#include <algorithm>
#include <cmath>
#include <wx/colour.h>
#include <map>
#include <tuple>
#include <wx/string.h>

static bool gDarkMode = false;

// Light -> dark colour pairs. Inverted at runtime by revert() for lightModeColorFor().
//
// NOTE: Accent::Base and Accent::Pressed (GUI/AccentColor.hpp) are deliberately ABSENT
// from this table. darkModeColorFor2() returns its input on a miss, which is what makes
// the accent render identically in both themes. Do not add rows for #767676 / #5C5C5C.
// Accent::Hover is the documented exception -- see its row below.
//
// NOTE: revert() below uses emplace, so a duplicate dark value resolves "first wins" --
// but iteration is ordered by operator<(wxColour) over GetRGBA() (packed ABGR), NOT by
// source order, so which row wins is not what reading this table top-to-bottom suggests.
// #3E3E45 (rows for #DFDFDF and #E8E8E8) is the one remaining duplicate.
static std::map<wxColour, wxColour> gDarkColors{
    {"#1F8EEA", "#2778D2"}, // rgb(31, 142, 234)   ???
    {"#FF6F00", "#D15B00"}, // rgb(255, 111, 0)    Secondary color
    {"#D01B1B", "#BB2A3A"}, // rgb(208, 27, 27)    ???
    {"#262E30", "#EFEFF0"}, // rgb(38, 46, 48)     Button text color | Input Text Color
    {"#DFDFDF", "#3E3E45"}, // rgb(223, 223, 223)  Button Background color
    {"#D4D4D4", "#4D4D54"}, // rgb(212, 212, 212)  Button Background color on Hover
    {"#6B6A6A", "#ACACB0"}, // rgb(107, 107, 106)  Button Dimmed text (was #909090, 3.3:1 on #3E3E45; now 4.7:1)
    {"#6B6B6A", "#B3B3B5"}, // rgb(107, 107, 106)  Input box side text
    {"#2C2C2E", "#B3B3B4"}, // rgb(44, 44, 46)     ???
    {"#6B6B6B", "#A4A4A8"}, // rgb(107, 107, 107)  Disabled Text (was #818183, 3.5:1 on the window)
    {"#ACACAC", "#65656A"}, // rgb(172, 172, 172)  Disabled Text on boxes | Dimmed Elements
    {"#EEEEEE", "#4C4C55"}, // rgb(238, 238, 238)  Separator Line | Title Line Color
    {"#E8E8E8", "#3E3E45"}, // rgb(232, 232, 232)  ???
    {"#323A3D", "#E5E5E4"}, // rgb(50, 58, 61)     Softer text color
    {"#262E31", "#272F31"}, // rgb(38, 46, 49)     Top bar (BBLTopbar): slate in both themes, a key of its own so it can be themed ("Top bar" role).
                            //                     Its dark value must not be another row's light key (#262E30 is the button/input text key):
                            //                     the live preview matches windows by every colour a role has had, so a shared colour lets a
                            //                     top-bar edit recolour the headings after a base flip.
    {"#FFFEFF", "#232328"}, // rgb(255, 254, 255)  Input background (TextInput, ComboBox, SpinInput): a key of its own so it can be themed apart from the window; its dark value differs from the window's by one unit so no two roles ever share a colour (the reverse map and the live re-theme both key on it)
    {"#FFFFFF", "#232327"}, // rgb(255, 255, 255)  Window background (was #2D2D31; ImGuiWrapper::COL_WINDOW_BG_DARK, IMSlider, 2DBed, BitmapCache and GUI_App::init_label_colours mirror this)
    {"#F8F8F8", "#2B2B30"}, // rgb(248, 248, 248)  Sidebar > Titlebar > Gradient Top | BBL monitor page titlebar bg (one step above the window)
    {"#F1F1F1", "#2B2B2F"}, // rgb(241, 241, 241)  Sidebar > Titlebar > Gradient Bottom
    {"#3B4446", "#1E1E21"}, // rgb(59, 68, 78)     Top Bar / Main tab bar bg color (Notebook::default_btn_bg mirrors this)
    {"#CECECE", "#54545B"}, // rgb(206, 206, 206)  Selected button / tab background (GUI_App::m_color_selected_btn_bg); NOT a panel: darkening it hides the selected tab
    {"#000000", "#FFFFFE"}, // rgb(0, 0, 0)        Mostly Text color wxBlack
    {"#F4F4F4", "#2E2E34"}, // rgb(244, 244, 244)  ??? (one step above the window)
    {"#DBDBDB", "#78787F"}, // rgb(219, 219, 219)  Input/Combo Box Border Color (was #4A4A51, 1.6:1 against the window; now 3.1:1; BitmapCache mirrors this)
    {"#323A3C", "#E5E5E6"}, // rgb(50, 58, 60)     Text color used on search list |
    {"#303A3C", "#E5E5E5"}, // rgb(48, 58, 60)     Object Table > Column header text color | StaticBox Border Color
    {"#FEFFFF", "#1B1B1E"}, // rgb(254, 255, 255)  Side Tabbar bg
    {"#A6A9AA", "#2D2D29"}, // rgb(166, 169, 170)  Seperator color
    {"#363636", "#E2E2E5"}, // rgb(54, 54, 54)     Sidebar > Parameter Label/Title color | Sidebar tab text | Create Filament window text (GUI_App::init_label_colours mirrors this)
    {"#F0F0F1", "#2B2B2F"}, // rgb(240, 240, 241)  Disabled element background
    {"#9E9E9E", "#53545A"}, // rgb(158, 158, 158)  ???
    {"#2B3436", "#808080"}, // rgb(43, 52, 54)     Also spelled wxColour(43, 52, 54) in CameraPopup/Search/Tab
    {"#D9D9D9", "#27272A"}, // rgb(217, 217, 217)  Sidebar > Toggle button track color
    {"#FFFEFE", "#D8D9D9"}, // rgb(255, 254, 254)  Sidebar > Toggle button thumb color
    //{"#F0F0F0", "#4C4C54"},
    // Unlayered accent tints -- Accent::Tint25 / Accent::Tint10 in GUI/AccentColor.hpp.
    // Unlike the interaction ramp these DO follow the theme: a background band has to.
    {"#D8D8D8", "#4F4F53"}, // rgb(216, 216, 216)  Dropdown checked item background color
    {"#ECECEC", "#3A3A3E"}, // rgb(236, 236, 236)  Combo / Dropdown focused background color
    // Accent::Hover. The only ramp colour with a dark pair: on #2D2D31 the plain
    // #767676 -> #888888 hover step is ~1.3:1 and invisible, so dark mode lifts it.
    {"#888888", "#A8A8A8"}, // rgb(136, 136, 136)  Accent hover (Slice / Print / buttons)
    // Disabled SideButton fill -- a GHOST of the top bar, one step lighter than the bar behind
    // it (1.37:1 light, 1.35:1 dark), so an unavailable Slice/Print still reads as a button
    // without reading as pressable. This needs a row of its own precisely because that bar is
    // NOT theme-symmetric: it is dark slate #3B4446 in the light theme and near-black #2D2D30
    // in the dark one, so no single literal sits one step above both. Derived from those two,
    // and must stay in step with SideButton's bottom_color if that ever changes.
    {"#4F585A", "#414144"}, // rgb(79, 88, 90)     Disabled Slice / Print pill
    {"#D0D0D1", "#C0C0C4"}, // rgb(208, 208, 209)  Disabled Slice / Print text (4.7:1 / 5.6:1 on the pill)
};

std::tuple<double, double, double> StateColor::GetLAB(const wxColour& color) {
    // Convert color to RGB color space
    double r = color.Red() / 255.0;
    double g = color.Green() / 255.0;
    double b = color.Blue() / 255.0;

    // Convert to XYZ color space
    double x = 0.412453*r + 0.357580*g + 0.180423*b;
    double y = 0.212671*r + 0.715160*g + 0.072169*b;
    double z = 0.019334*r + 0.119193*g + 0.950227*b;

    // Normalize XYZ values
    double x_n = x / 0.950456;
    double y_n = y / 1.0;
    double z_n = z / 1.088754;

    // Convert to LAB color space
    double epsilon = 0.008856;
    double kappa = 903.3;
    double fx = (x_n > epsilon) ? cbrt(x_n) : (kappa*x_n + 16.0) / 116.0;
    double fy = (y_n > epsilon) ? cbrt(y_n) : (kappa*y_n + 16.0) / 116.0;
    double fz = (z_n > epsilon) ? cbrt(z_n) : (kappa*z_n + 16.0) / 116.0;

    double l = 116.0 * fy - 16.0;
    double a = 500.0 * (fx - fy);
    double b_lab = 200.0 * (fy - fz);

    return std::tuple<double, double, double>(l, a, b_lab);
}

double StateColor::LAB_Delta_E(const wxColour& color1, const wxColour& color2) {
    auto [l1, a1, b1] = GetLAB(color1);
    auto [l2, a2, b2] = GetLAB(color2);
    return sqrt((l1 - l2) * (l1 - l2) + (a1 - a2) * (a1 - a2) + (b1 - b2) * (b1 - b2));
}

double StateColor::GetColorDifference(const wxColour& color1, const wxColour& color2) {
    return LAB_Delta_E(color1, color2);
}

double StateColor::ContrastRatio(const wxColour& a, const wxColour& b)
{
    auto lum = [](const wxColour& c) {
        auto f = [](double v) { v /= 255.0; return v <= 0.03928 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4); };
        return 0.2126 * f(c.Red()) + 0.7152 * f(c.Green()) + 0.0722 * f(c.Blue());
    };
    const double la = lum(a), lb = lum(b);
    return (std::max(la, lb) + 0.05) / (std::min(la, lb) + 0.05);
}

wxColour StateColor::EnsureContrast(const wxColour& text, const wxColour& bg, double min_ratio)
{
    if (ContrastRatio(text, bg) >= min_ratio)
        return text;
    // Lightness (HSL) is not luminance: a mid grey can read better against white than black.
    // Try the likely direction first, then the other, and fall back to whichever of black and
    // white reads best.
    const bool dark_first = GetLightness(bg) > 50.0;
    for (const bool go_dark : { dark_first, !dark_first }) {
        double l = GetLightness(text);
        for (int i = 0; i < 20; ++i) {
            l += go_dark ? -5.0 : 5.0;
            if (l < 0.0 || l > 100.0)
                break;
            const wxColour c = SetLightness(text, l);
            if (ContrastRatio(c, bg) >= min_ratio)
                return c;
        }
    }
    const wxColour black(0, 0, 0), white(255, 255, 255);
    return ContrastRatio(black, bg) >= ContrastRatio(white, bg) ? black : white;
}

double StateColor::GetLightness(const wxColour& color) {
    auto [l, a, b_lab] = GetLAB(color);
    return l;
}

// Function to lighten or darken a wxColour using LAB color space
wxColour StateColor::SetLightness(const wxColour& color, double lightness) {
    auto [l, a, b_lab] = GetLAB(color);

    // Clamp lightness value
    l = std::max(0.0, std::min(100.0, lightness));

    // Convert back to XYZ color space
    double fy_3 = (l + 16.0) / 116.0;
    double fx_3 = a / 500.0 + fy_3;
    double fz_3 = fy_3 - b_lab / 200.0;

    double epsilon = 0.008856;
    double kappa = 903.3;
    double x_3 = (fx_3 > epsilon) ? fx_3 * fx_3 * fx_3 : (116.0 * fx_3 - 16.0) / kappa;
    double y_3 = (l > kappa*epsilon) ? fy_3 * fy_3 * fy_3 : l / kappa;
    double z_3 = (fz_3 > epsilon) ? fz_3 * fz_3 * fz_3 : (116.0 * fz_3 - 16.0) / kappa;

    // Denormalize XYZ values
    double x = x_3 * 0.950456;
    double y = y_3 * 1.0;
    double z = z_3 * 1.088754;

    // Convert XYZ to RGB
    double r_new = 3.240479*x - 1.537150*y - 0.498535*z;
    double g_new = -0.969256*x + 1.875992*y + 0.041556*z;
    double b_new = 0.055648*x - 0.204043*y + 1.057311*z;

    // Clamp RGB values
    r_new = std::max(0.0, std::min(1.0, r_new));
    g_new = std::max(0.0, std::min(1.0, g_new));
    b_new = std::max(0.0, std::min(1.0, b_new));

    // Convert back to wxColour
    int r_int = static_cast<int>(r_new * 255);
    int g_int = static_cast<int>(g_new * 255);
    int b_int = static_cast<int>(b_new * 255);

    return wxColour(r_int, g_int, b_int);
}

wxColour StateColor::LightenDarkenColor(const wxColour& color, int amount) {
    auto [l, a, b_lab] = GetLAB(color);

    // Modify lightness
    l += amount;

    // Clamp lightness value
    l = std::max(0.0, std::min(100.0, l));

    // Convert back to XYZ color space
    double fy_3 = (l + 16.0) / 116.0;
    double fx_3 = a / 500.0 + fy_3;
    double fz_3 = fy_3 - b_lab / 200.0;

    double epsilon = 0.008856;
    double kappa = 903.3;
    double x_3 = (fx_3 > epsilon) ? fx_3 * fx_3 * fx_3 : (116.0 * fx_3 - 16.0) / kappa;
    double y_3 = (l > kappa*epsilon) ? fy_3 * fy_3 * fy_3 : l / kappa;
    double z_3 = (fz_3 > epsilon) ? fz_3 * fz_3 * fz_3 : (116.0 * fz_3 - 16.0) / kappa;

    // Denormalize XYZ values
    double x = x_3 * 0.950456;
    double y = y_3 * 1.0;
    double z = z_3 * 1.088754;

    // Convert XYZ to RGB
    double r_new = 3.240479*x - 1.537150*y - 0.498535*z;
    double g_new = -0.969256*x + 1.875992*y + 0.041556*z;
    double b_new = 0.055648*x - 0.204043*y + 1.057311*z;

    // Clamp RGB values
    r_new = std::max(0.0, std::min(1.0, r_new));
    g_new = std::max(0.0, std::min(1.0, g_new));
    b_new = std::max(0.0, std::min(1.0, b_new));

    // Convert back to wxColour
    int r_int = static_cast<int>(r_new * 255);
    int g_int = static_cast<int>(g_new * 255);
    int b_int = static_cast<int>(b_new * 255);

    return wxColour(r_int, g_int, b_int);
}

std::map<wxColour, wxColour> const & StateColor::GetDarkMap()
{
    return gDarkColors;
}

wxColour StateColor::AvoidTableCollision(const wxColour& color, bool dark_base)
{
    auto collides = [&](const wxColour& c) {
        for (const auto& [key, value] : gDarkColors)
            if (dark_base ? key == c : value == c)
                return true;
        return false;
    };
    wxColour c = color;
    for (int i = 0; i < 8 && collides(c); ++i) {
        auto step = [](unsigned char v) { return (unsigned char) (v < 128 ? v + 1 : v - 1); };
        c = wxColour(step(c.Red()), step(c.Green()), step(c.Blue()));
    }
    return c;
}

void StateColor::SetDarkMode(bool dark) { gDarkMode = dark; }

static std::map<wxColour, wxColour> gDarkOverrides;
static std::map<wxColour, wxColour> gLightOverrides;

void StateColor::SetThemeOverride(bool dark, wxColour const &light_key, wxColour const &value)
{
    auto &map = dark ? gDarkOverrides : gLightOverrides;
    if (value.IsOk())
        map[light_key] = value;
    else
        map.erase(light_key);
}

void StateColor::ClearThemeOverrides()
{
    gDarkOverrides.clear();
    gLightOverrides.clear();
}

inline wxColour darkModeColorFor2(wxColour const &color)
{
    if (!gDarkMode) {
        auto over = gLightOverrides.find(color);
        return over != gLightOverrides.end() ? over->second : color;
    }
    auto over = gDarkOverrides.find(color);
    if (over != gDarkOverrides.end()) return over->second;
    // A colour that is itself the value of an override is already resolved: a user's black
    // window background must not go through the #000000 (text) row and come out white when a
    // window's current colour is re-resolved.
    for (const auto& [key, value] : gDarkOverrides)
        if (value == color)
            return color;
    auto iter = gDarkColors.find(color);
    if (iter != gDarkColors.end()) return iter->second;
    return color;
}

std::map<wxColour, wxColour> revert(std::map<wxColour, wxColour> const & map)
{
    std::map<wxColour, wxColour> map2;
    for (auto &p : map) map2.emplace(p.second, p.first);
    return map2;
}

wxColour StateColor::lightModeColorFor(wxColour const &color)
{
    static std::map<wxColour, wxColour> gLightColors = revert(gDarkColors);
    auto iter = gLightColors.find(color);
    if (iter != gLightColors.end()) return iter->second;
    return color;
}

wxColour StateColor::darkModeColorFor(wxColour const &color) { return darkModeColorFor2(color); }

StateColor::StateColor(wxColour const &color) { append(color, 0); }

StateColor::StateColor(wxString const &color) { append(color, 0); }

StateColor::StateColor(unsigned long color) { append(color, 0); }

void StateColor::append(wxColour const & color, int states)
{
    statesList_.push_back(states);
    colors_.push_back(color);
}

void StateColor::append(wxString const & color, int states)
{
    wxColour c1(color);
    append(c1, states);
}

void StateColor::append(unsigned long color, int states)
{
    if ((color & 0xff000000) == 0)
        color |= 0xff000000;
    wxColour cl; cl.SetRGBA((color & 0xff00ff00) | ((color & 0xff) << 16) | ((color >> 16) & 0xff));
    append(cl, states);
}

void StateColor::clear()
{
    statesList_.clear();
    colors_.clear();
}

int StateColor::states() const
{
    int states = 0;
    for (auto s : statesList_) states |= s;
    states = (states & 0xffff) | (states >> 16);
    if (takeFocusedAsHovered_ && (states & Hovered))
        states |= Focused;
    return states;
}

wxColour StateColor::defaultColor() {
    return colorForStates(0);
}

wxColour StateColor::colorForStates(int states)
{
    bool focused = takeFocusedAsHovered_ && (states & Focused);
    for (int i = 0; i < statesList_.size(); ++i) {
        int s = statesList_[i];
        int on = s & 0xffff;
        int off = s >> 16;
        if ((on & states) == on && (off & ~states) == off) {
            return darkModeColorFor2(colors_[i]);
        }
        if (focused && (on & Hovered)) {
            on |= Focused;
            on &= ~Hovered;
            if ((on & states) == on && (off & ~states) == off) {
                return darkModeColorFor2(colors_[i]);
            }
        }
    }
    return wxColour(0, 0, 0, 0);
}

wxColour StateColor::colorForStatesNoDark(int states)
{
    bool focused = takeFocusedAsHovered_ && (states & Focused);
    for (int i = 0; i < statesList_.size(); ++i) {
        int s = statesList_[i];
        int on = s & 0xffff;
        int off = s >> 16;
        if ((on & states) == on && (off & ~states) == off) {
            return colors_[i];
        }
        if (focused && (on & Hovered)) {
            on |= Focused;
            on &= ~Hovered;
            if ((on & states) == on && (off & ~states) == off) {
                return colors_[i];
            }
        }
    }
    return wxColour(0, 0, 0, 0);
}

int StateColor::colorIndexForStates(int states)
{
    for (int i = 0; i < statesList_.size(); ++i) {
        int s   = statesList_[i];
        int on  = s & 0xffff;
        int off = s >> 16;
        if ((on & states) == on && (off & ~states) == off) { return i; }
    }
    return -1;
}

bool StateColor::setColorForStates(wxColour const &color, int states)
{
    for (int i = 0; i < statesList_.size(); ++i) {
        if (statesList_[i] == states) {
            colors_[i] = color;
            return true;
        }
    }
    return false;
}

void StateColor::setTakeFocusedAsHovered(bool set) { takeFocusedAsHovered_ = set; }
