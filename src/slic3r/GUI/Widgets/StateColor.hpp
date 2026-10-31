#ifndef slic3r_GUI_StateColor_hpp_
#define slic3r_GUI_StateColor_hpp_

#include <tuple>
#include <utility>
#include <vector>
#include <wx/colour.h>

#include <map>
#include <wx/string.h>

// Orders the std::map<wxColour, wxColour> tables below. Declared here rather than in
// StateColor.cpp so every translation unit that looks up GetDarkMap() sees the same ordering.
inline bool operator<(wxColour const &l, wxColour const &r) { return l.GetRGBA() < r.GetRGBA(); }

class StateColor
{
public:
    enum State {
        Normal = 0,
        Enabled = 1,
        Checked = 2,
        Focused = 4,
        Hovered = 8,
        Pressed = 16,
        Disabled = 1 << 16,
        NotChecked = 2 << 16,
        NotFocused = 4 << 16,
        NotHovered = 8 << 16,
        NotPressed = 16 << 16,
    };

public:
    static std::tuple<double, double, double> GetLAB(const wxColour& color);
    static double GetLightness(const wxColour& color);
    static wxColour SetLightness(const wxColour& color, double lightness);
    static wxColour LightenDarkenColor(const wxColour& color, int amount);
    static double GetColorDifference(const wxColour& c1, const wxColour& c2);
    // WCAG contrast ratio (1..21) and a text colour pushed light or dark, hue kept, until it
    // reads at `min_ratio` on `bg` (black or white as the last resort).
    static double ContrastRatio(const wxColour& a, const wxColour& b);
    static wxColour EnsureContrast(const wxColour& text, const wxColour& bg, double min_ratio);
    // A theme value must not coincide with the colour table's namespace: on the dark base with a
    // light key (a black window would be sent through the black-text row when a window's colour
    // is re-resolved), on the light base with a dark value (the reverse map would turn it into a
    // light key on a theme flip). Nudges such a colour by one unit per channel towards mid-grey.
    static wxColour AvoidTableCollision(const wxColour& color, bool dark_base);
    static double LAB_Delta_E(const wxColour& c1, const wxColour& c2);

    static void SetDarkMode(bool dark);

    static std::map<wxColour, wxColour> const & GetDarkMap();
    // User theme overrides (Preferences > Appearance > Theme colours). Keyed by the light-theme
    // colour that code passes to darkModeColorFor(), i.e. the same key as gDarkColors; kept per
    // theme. An invalid `value` removes the override.
    static void SetThemeOverride(bool dark, wxColour const &light_key, wxColour const &value);
    static void ClearThemeOverrides();
    static wxColour darkModeColorFor(wxColour const &color);
    static wxColour lightModeColorFor(wxColour const &color);

public:
    template<typename ...Colors>
    StateColor(std::pair<Colors, int>... colors) {
        fill(colors...);
    }

    // single color
    StateColor(wxColour const & color);

    // single color
    StateColor(wxString const &color);

    // single color
    StateColor(unsigned long color);

    // operator==
    bool operator==(StateColor const& other) const{
        return statesList_ == other.statesList_ && colors_ == other.colors_ && takeFocusedAsHovered_ == other.takeFocusedAsHovered_;
    };

    // operator!=
    bool operator!=(StateColor const& other) const{
        return !(*this == other);
    };

public:
    void append(wxColour const & color, int states);

    void append(wxString const &color, int states);

    void append(unsigned long color, int states);

    void clear();

public:
    int count() const { return statesList_.size(); }

    int states() const;

public:
    wxColour defaultColor();

    wxColour colorForStates(int states);

    wxColour colorForStatesNoDark(int states);

    int colorIndexForStates(int states);

    bool setColorForStates(wxColour const & color, int states);

    void setTakeFocusedAsHovered(bool set);

private:
    template<typename Color, typename ...Colors>
    void fill(std::pair<Color, int> color, std::pair<Colors, int>... colors) {
        fillOne(color);
        fill(colors...);
    }

    template<typename Color>
    void fillOne(std::pair<Color, int> color) {
        append(color.first, color.second);
    }

    void fill() {
    }

private:
    std::vector<int> statesList_;
    std::vector<wxColour> colors_;
    bool takeFocusedAsHovered_ = true;
};

#endif // !slic3r_GUI_StateColor_hpp_
