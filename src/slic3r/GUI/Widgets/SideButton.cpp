#include "SideButton.hpp"
#include "Label.hpp"

#include <vector>
#include "slic3r/GUI/Widgets/StateHandler.hpp"
#include <wx/colour.h>
#include <wx/dc.h>
#include <wx/dcclient.h>
#include <wx/dcgraph.h>
#include <wx/string.h>
#include <wx/stattext.h>
#include <wx/gdicmn.h>
#include <wx/event.h>
#include "slic3r/GUI/Widgets/StateColor.hpp"
#include "slic3r/GUI/wxExtensions.hpp"

BEGIN_EVENT_TABLE(SideButton, wxWindow)
EVT_LEFT_DOWN(SideButton::mouseDown)
EVT_LEFT_UP(SideButton::mouseReleased)
EVT_KEY_DOWN(SideButton::keyDown)
EVT_MOUSE_CAPTURE_LOST(SideButton::mouseCaptureLost)
EVT_PAINT(SideButton::paintEvent)
END_EVENT_TABLE()

SideButton::SideButton(wxWindow* parent, wxString text, wxString icon, long stlye, int iconSize)
    : wxWindow(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, stlye)
    , state_handler(this)
{
    radius = 12;
#ifdef __APPLE__
    extra_size = wxSize(38 + FromDIP(20), 10);
    text_margin = 15 + FromDIP(20);
#else
    extra_size = wxSize(38, 10);
    text_margin = 15;
#endif

    icon_offset = 0;
    text_orientation = HO_Left;

    // A disabled side button reads as a GHOST of the bar behind it: one step lighter than the
    // backdrop this widget already paints under itself (bottom_color, see dorender), so the
    // pill's shape survives but nothing about it invites a click.
    //
    // It cannot be a fixed grey. Upstream leaned on hue for the distinction -- enabled was
    // the teal accent, and #6B6B6B read as "greyed out" next to it. With the accent itself
    // now a grey (#767676, Accent::Base in GUI/AccentColor.hpp) the two collapsed to 11/255
    // apart, and dark mode inverted the meaning outright: #6B6B6B has a gDarkColors row to
    // #818183, so the DISABLED pill rendered LIGHTER than the enabled one.
    //
    // #4F585A carries a gDarkColors row to #414144 (see the note there) because this bar is
    // dark in BOTH themes -- slate #3B4446 in light, #2D2D30 in dark -- so a single literal
    // cannot sit one step above both. The row also means colorForStates() re-resolves it on
    // every paint, so a live theme switch stays correct.
    const wxColour ghost = wxColour("#4F585A");

    border_color.append(ghost, StateColor::Disabled);
    border_color.append(wxColour(92, 92, 92), StateColor::Pressed);
    border_color.append(wxColour(136, 136, 136), StateColor::Hovered);
    border_color.append(0x767676, StateColor::Normal);
    border_color.setTakeFocusedAsHovered(false);

    // Disabled text must still be readable on the ghost pill: #ACACAC would take the generic
    // dimmed-text dark row (#65656A, 1.8:1 on #414144). Own key with its own dark row instead
    // (4.7:1 light, 5.6:1 dark), still a step below the enabled white.
    text_color.append(wxColour("#D0D0D1"), StateColor::Disabled);
    text_color.append(0xFEFEFE, StateColor::Pressed);
    text_color.append(0xFEFEFE, StateColor::Hovered);
    text_color.append(0xFEFEFE, StateColor::Normal);

    background_color.append(ghost, StateColor::Disabled);
    background_color.append(wxColour(92, 92, 92), StateColor::Pressed);
    background_color.append(wxColour(136, 136, 136), StateColor::Hovered);
    background_color.append(0x767676, StateColor::Normal);
    background_color.setTakeFocusedAsHovered(false);

    SetBottomColour(wxColour("#3B4446"));

    state_handler.attach({ &border_color, &text_color, &background_color });
    state_handler.update_binds();

    // icon only
    if (!icon.IsEmpty()) {
        this->icon = ScalableBitmap(this, icon.ToStdString(), iconSize > 0 ? iconSize : 14);
    }

    SetFont(Label::Body_14);
    wxWindow::SetLabel(text);

    messureSize();
}

void SideButton::SetCornerRadius(double radius)
{
    this->radius = radius;
    Refresh();
}

void SideButton::SetCornerEnable(const std::vector<bool>& enable)
{
    radius_enable.clear();
    for (auto en : enable) {
        radius_enable.push_back(en);
    }
}

void SideButton::SetTextLayout(EHorizontalOrientation orient, int margin)
{
    text_orientation = orient;
    text_margin = margin;
    messureSize();
    Refresh();
}

void SideButton::SetLayoutStyle(int style)
{
    layout_style = style;
    messureSize();
    Refresh();
}

void SideButton::SetLabel(const wxString& label)
{
    wxWindow::SetLabel(label);
    messureSize();
    Refresh();
}

bool SideButton::SetForegroundColour(wxColour const &color)
{
    text_color = StateColor(color);
    state_handler.update_binds();
    return true;
}

bool SideButton::SetBackgroundColour(wxColour const& color)
{
    background_color = StateColor(color);
    state_handler.update_binds();
    return true;
}

bool SideButton::SetBottomColour(wxColour const& color)
{
    bottom_color = color;
    return true;
}

void SideButton::SetMinSize(const wxSize& size)
{
    minSize = size;
    messureSize();
}

void SideButton::SetBorderColor(StateColor const &color)
{
    border_color = color;
    state_handler.update_binds();
    Refresh();
}

void SideButton::SetForegroundColor(StateColor const &color)
{
    text_color = color;
    state_handler.update_binds();
    Refresh();
}

void SideButton::SetBackgroundColor(StateColor const &color)
{
    background_color = color;
    state_handler.update_binds();
    Refresh();
}

bool SideButton::Enable(bool enable)
{
    bool result = wxWindow::Enable(enable);
    if (result) {
        wxCommandEvent e(EVT_ENABLE_CHANGED);
        e.SetEventObject(this);
        GetEventHandler()->ProcessEvent(e);
    }
    return result;
}

void SideButton::Rescale()
{
    if (this->icon.bmp().IsOk())
        this->icon.msw_rescale();
    messureSize();
}

void SideButton::SetExtraSize(const wxSize& size)
{
    extra_size = size;
    messureSize();
}

void SideButton::SetIconOffset(const int offset)
{
    icon_offset = offset;
    messureSize();
}

void SideButton::paintEvent(wxPaintEvent& evt)
{
    // depending on your system you may need to look at double-buffered dcs
    wxPaintDC dc(this);
#ifdef __WXMSW__
    wxGCDC dc2(dc);
#else
    wxDC & dc2(dc);
#endif

    wxDC & dctext(dc);
    dorender(dc2, dctext);
}

/*
 * Here we do the actual rendering. I put it in a separate
 * method so that it can work no matter what type of DC
 * (e.g. wxPaintDC or wxClientDC) is used.
 */
void SideButton::dorender(wxDC& dc, wxDC& text_dc)
{
    wxSize size = GetSize();

    // draw background
    dc.SetPen(wxNullPen);
    // The corners outside the pill show whatever the parent shows: paint them with the parent's
    // current background so they can never lag behind a theme change.
    dc.SetBrush(wxBrush(GetParent() != nullptr ? GetParent()->GetBackgroundColour() : StateColor::darkModeColorFor(bottom_color)));
    dc.DrawRectangle(0, 0, size.x, size.y);

    int states = state_handler.states();
    dc.SetBrush(wxBrush(background_color.colorForStates(states)));

    dc.SetPen(wxPen(border_color.colorForStates(states)));
    int pen_width = dc.GetPen().GetWidth();


    // draw icon style
    if (icon.bmp().IsOk()) {
        if (radius > 1e-5) {
            dc.DrawRoundedRectangle(0, 0, size.x, size.y, radius);
            dc.DrawRectangle(radius, 0, size.x - radius, size.y);
            dc.SetPen(wxNullPen);
            dc.DrawRectangle(radius - pen_width, pen_width, radius, size.y - 2 * pen_width);
        }
        else {
            dc.DrawRectangle(0, 0, size.x, size.y);
        }
    }
    // draw text style
    else {
        if (radius > 1e-5) {
            if (layout_style == 1) {
                dc.DrawRoundedRectangle(0, 0, size.x, size.y, radius);
                dc.SetPen(wxNullPen);
            } else {
                dc.DrawRoundedRectangle(0, 0, size.x, size.y, radius);
                dc.DrawRectangle(0, 0, radius, size.y);
                dc.SetPen(wxNullPen);
                dc.DrawRectangle(pen_width, pen_width, size.x - radius, size.y - 2 * pen_width);
            }
        } else {
            dc.DrawRectangle(0, 0, size.x, size.y);
        }
    }

    dc.SetBrush(*wxTRANSPARENT_BRUSH);
    // calc content size
    wxSize szIcon;
    wxSize szContent = textSize;
    if (icon.bmp().IsOk()) {
        if (szContent.y > 0) {
            //BBS norrow size between text and icon
            szContent.x += 5;
        }
        szIcon = icon.GetBmpSize();
        szContent.x += szIcon.x;
        if (szIcon.y > szContent.y)
            szContent.y = szIcon.y;
    }
    // move to center
    wxRect rcContent = { {0, 0}, size };
    if (text_orientation == EHorizontalOrientation::HO_Center) {
        wxSize offset = (size - szContent) / 2;
        rcContent.Deflate(offset.x, offset.y);
    } else if (text_orientation == EHorizontalOrientation::HO_Left) {
        wxSize offset = (size - szContent) / 2;
        rcContent.Deflate(text_margin, offset.y);
    } else if (text_orientation == EHorizontalOrientation::HO_Right) {
        wxSize offset = (size - szContent) / 2;
        rcContent.Deflate(size.x - text_margin, offset.y);
    }

    // start draw
    wxPoint pt = rcContent.GetLeftTop();
    if (icon.bmp().IsOk()) {
        //BBS extra pixels for icon
        pt.x += icon_offset;
        pt.y += (rcContent.height - szIcon.y) / 2;
        dc.DrawBitmap(icon.bmp(), pt);
        //BBS norrow size between text and icon
        pt.x += szIcon.x + 5;
        pt.y = rcContent.y;
    }

    auto text = GetLabel();
    if (!text.IsEmpty()) {
        pt.y += (rcContent.height - textSize.y) / 2;
        text_dc.SetFont(GetFont());
        text_dc.SetTextForeground(text_color.colorForStates(states));
        text_dc.DrawText(text, pt);
    }

}

void SideButton::messureSize()
{
    textSize = GetTextExtent(GetLabel());
    if (minSize.GetWidth() > 0) {
        wxWindow::SetMinSize(minSize);
        return;
    }

    wxSize szContent = textSize;
    if (this->icon.bmp().IsOk()) {
        if (szContent.y > 0) {
            szContent.x += 5;
        }
        wxSize szIcon = this->icon.GetBmpSize();
        szContent.x += szIcon.x;
        if (szIcon.y > szContent.y)
            szContent.y = szIcon.y;
        //BBS icon only
        wxWindow::SetMinSize(szContent + wxSize(szContent.GetX() + extra_size.GetX(), minSize.GetHeight()));
    }
    else {
        if (minSize.GetHeight() > 0) {
            //BBS with text size
            wxWindow::SetMinSize(wxSize(szContent.GetX() + extra_size.GetX(), minSize.GetHeight()));
        } else {
            //BBS with text size
            wxWindow::SetMinSize(szContent + extra_size);
        }
    }
}

void SideButton::mouseDown(wxMouseEvent& event)
{
    event.Skip();
    pressedDown = true;
    SetFocus();
    // Capturing twice pushes this button onto the capture stack twice, and the
    // release would then hand the capture straight back to it.
    if (!HasCapture())
        CaptureMouse();
}

void SideButton::mouseReleased(wxMouseEvent& event)
{
    event.Skip();
    // A press whose release went elsewhere still holds the capture.
    if (HasCapture())
        ReleaseMouse();
    if (pressedDown) {
        pressedDown = false;
        if (wxRect({0, 0}, GetSize()).Contains(event.GetPosition()))
            sendButtonEvent();
    }
}

void SideButton::keyDown(wxKeyEvent& event)
{
    // Space/Enter press the button, like a native one.
    if ((event.GetKeyCode() == WXK_SPACE || event.GetKeyCode() == WXK_RETURN) && IsEnabled())
        sendButtonEvent();
    else
        event.Skip();
}

void SideButton::mouseCaptureLost(wxMouseCaptureLostEvent&)
{
    pressedDown = false;
}

void SideButton::sendButtonEvent()
{
    wxCommandEvent event(wxEVT_COMMAND_BUTTON_CLICKED, GetId());
    event.SetEventObject(this);
    GetEventHandler()->ProcessEvent(event);
}
