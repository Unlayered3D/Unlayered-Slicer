#include "BBLTopbar.hpp"
#include "wx/artprov.h"
#include "wx/aui/framemanager.h"
#include "wx/display.h"
#include <wx/gdicmn.h>
#include <wx/event.h>
#include <wx/dcbuffer.h>
#include <wx/dc.h>
#include <wx/aui/auibar.h>
#include "slic3r/GUI/Widgets/StateColor.hpp"
#include <wx/settings.h>
#include "slic3r/GUI/Widgets/Label.hpp"
#include <wx/image.h>
#include <wx/string.h>
#include <cstddef>
#include <wx/utils.h>
#include <wx/window.h>
#include "I18N.hpp"
#include "GUI_App.hpp"
#include "GUI.hpp"
#ifdef __WXGTK__
#include "LinuxDisplayBackend.hpp"
#endif
#include "wxExtensions.hpp"
#include "Plater.hpp"
#include "MainFrame.hpp"
#include "Theme.hpp"
#include "Widgets/StateColor.hpp"

// The bar's colours come from the theme: the "Top bar" role for the background, and white text
// pushed dark when the auto-contrast option is on and the bar is light.
static wxColour topbar_background() { return StateColor::darkModeColorFor(wxColour("#262E31")); }
static wxColour topbar_text()
{
    const wxColour bg = topbar_background();
    return Slic3r::GUI::ThemeManager::get().active().auto_contrast_text ? StateColor::EnsureContrast(*wxWHITE, bg, 4.5) : *wxWHITE;
}
#ifdef SLIC3R_CAD
#include "CAD/DesignPanel.hpp"
#endif
#include "WebViewDialog.hpp"
#include "PartPlate.hpp"

#include <boost/log/trivial.hpp>
#include "libslic3r/AppConfig.hpp"

#include <algorithm>
#include <vector>

#ifdef __WXGTK__
#include <gtk/gtk.h>
#endif

#define TOPBAR_ICON_SIZE  18
#define TOPBAR_TITLE_WIDTH  300

using namespace Slic3r;
using namespace Slic3r::GUI;

enum CUSTOM_ID
{
    ID_TOP_MENU_TOOL = 3100,
    ID_LOGO,
    ID_TOP_FILE_MENU,
    ID_TOP_DROPDOWN_MENU,
    ID_TITLE,
    ID_MODEL_STORE,
    ID_CALIB,
    ID_TOOL_BAR = 3200,
    ID_AMS_NOTEBOOK,
};

CenteredTitle::CenteredTitle(wxWindow* parent)
    : wxControl()
{
    SetBackgroundStyle(wxBG_STYLE_PAINT);
    Create(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE);
    // Display only: keyboard traversal skips it.
    SetCanFocus(false);
    DisableFocusFromKeyboard();
    Bind(wxEVT_PAINT, [this](wxPaintEvent&) {
        wxBufferedPaintDC dc(this);
        dc.SetBackground(wxBrush(topbar_background()));
        dc.Clear();

        dc.SetTextForeground(topbar_text());

        wxFontMetrics fm = dc.GetFontMetrics();
        int textHeight = fm.ascent + fm.descent;

        wxRect   rect       = GetClientRect();
        wxString ellipsized = wxControl::Ellipsize(m_title, dc, wxELLIPSIZE_END, wxMax(0, rect.GetWidth() - FromDIP(8)));

        int y = rect.y + (rect.height - textHeight) / 2 + 1;
        int x = rect.x + ((ellipsized != m_title)                      // is ellipsized
            ? FromDIP(4)                                               // align to left when clipped
            : (rect.width - dc.GetTextExtent(m_title).GetWidth()) / 2); // centered when has available space

        dc.DrawText(ellipsized, x, y);
    });

    // repaint for Ellipsize
    Bind(wxEVT_SIZE, [this](wxSizeEvent& e) {
        Refresh();
        e.Skip();
    });

    auto forwardMouseEvent = [this](wxMouseEvent& e) {
        if (e.LeftDown() && e.GetClickCount() > 1) return; // prevent duplicate event
        e.SetPosition(GetParent()->ScreenToClient(ClientToScreen(e.GetPosition())));
        GetParent()->GetEventHandler()->ProcessEvent(e);
    };

    Bind(wxEVT_LEFT_DOWN,   forwardMouseEvent);
    Bind(wxEVT_LEFT_DCLICK, forwardMouseEvent);
}

void CenteredTitle::SetTitle(const wxString& title) {
    m_title = title;
    Refresh();
}

// required for proper height
wxSize CenteredTitle::DoGetBestSize() const
{
    return wxSize(10, FromDIP(30));
}


class BBLTopbarArt : public wxAuiDefaultToolBarArt
{
public:
    virtual void DrawBackground(wxDC& dc, wxWindow* wnd, const wxRect& rect) wxOVERRIDE;
    virtual void DrawButton(wxDC& dc, wxWindow* wnd, const wxAuiToolBarItem& item, const wxRect& rect) wxOVERRIDE;
};

void BBLTopbarArt::DrawBackground(wxDC& dc, wxWindow* wnd, const wxRect& rect)
{
    dc.SetBrush(wxBrush(topbar_background()));
    wxRect clipRect = rect;
    clipRect.y -= 8;
    clipRect.height += 8;
    dc.SetClippingRegion(clipRect);
    dc.DrawRectangle(rect);
    dc.DestroyClippingRegion();
}

void BBLTopbarArt::DrawButton(wxDC& dc, wxWindow* wnd, const wxAuiToolBarItem& item, const wxRect& rect)
{
    int textWidth = 0, textHeight = 0;
    wxFontMetrics fm;

    if (m_flags & wxAUI_TB_TEXT)
    {
        dc.SetFont(m_font);
        fm = dc.GetFontMetrics();
        textHeight = fm.ascent + fm.descent;
        dc.GetTextExtent(item.GetLabel(), &textWidth, nullptr);
    }

    int bmpX = 0, bmpY = 0;
    int textX = 0, textY = 0;

    // ORCA resolves the toolbar item bitmap using the actual window DPI context used for painting.
    // GetBitmap() / GetDisabledBitmap() was using internal window pointer (m_window), not the paint-time wnd
    // m_window was created before final DPI context was known so items not scales properly
    const wxBitmap bmp = item.GetCurrentBitmapFor(wnd);

    const wxSize bmpSize = bmp.IsOk() ? bmp.GetScaledSize() : wxSize(0, 0);

    if (m_textOrientation == wxAUI_TBTOOL_TEXT_BOTTOM)
    {
        bmpX = rect.x +
            (rect.width / 2) -
            (bmpSize.x / 2);

        bmpY = rect.y +
            ((rect.height - textHeight) / 2) -
            (bmpSize.y / 2);

        textX = rect.x + (rect.width / 2) - (textWidth / 2) + 1;
        textY = rect.y + rect.height - textHeight - 1;
    }
    else if (m_textOrientation == wxAUI_TBTOOL_TEXT_RIGHT)
    {
        bmpX = rect.x + wnd->FromDIP(3);

        bmpY = rect.y +
            (rect.height / 2) -
            (bmpSize.y / 2);

        textX = bmpX + wnd->FromDIP(3) + bmpSize.x;
        textY = bmpY + bmpSize.y / 2 - textHeight / 2 - fm.externalLeading + 1;
    }


    if (!(item.GetState() & wxAUI_BUTTON_STATE_DISABLED))
    {
        if (item.GetState() & wxAUI_BUTTON_STATE_PRESSED)
        {
            dc.SetPen(wxPen(StateColor::darkModeColorFor("#767676"))); // ORCA
            dc.SetBrush(wxBrush(StateColor::darkModeColorFor("#767676"))); // ORCA
            dc.DrawRectangle(rect);
        }
        else if ((item.GetState() & wxAUI_BUTTON_STATE_HOVER) || item.IsSticky())
        {
            dc.SetPen(wxPen(StateColor::darkModeColorFor("#767676"))); // ORCA
            dc.SetBrush(wxBrush(StateColor::darkModeColorFor("#767676"))); // ORCA

            // draw an even lighter background for checked item hovers (since
            // the hover background is the same color as the check background)
            if (item.GetState() & wxAUI_BUTTON_STATE_CHECKED)
                dc.SetBrush(wxBrush(StateColor::darkModeColorFor("#767676"))); // ORCA

            dc.DrawRectangle(rect);
        }
        else if (item.GetState() & wxAUI_BUTTON_STATE_CHECKED)
        {
            // it's important to put this code in an else statement after the
            // hover, otherwise hovers won't draw properly for checked items
            dc.SetPen(wxPen(StateColor::darkModeColorFor("#767676"))); // ORCA
            dc.SetBrush(wxBrush(StateColor::darkModeColorFor("#767676"))); // ORCA
            dc.DrawRectangle(rect);
        }
    }

    if (bmp.IsOk())
        dc.DrawBitmap(bmp, bmpX, bmpY, true);

    // set the item's text color based on if it is disabled
    dc.SetTextForeground(topbar_text());
    if (item.GetState() & wxAUI_BUTTON_STATE_DISABLED)
    {
        dc.SetTextForeground(wxSystemSettings::GetColour(wxSYS_COLOUR_GRAYTEXT));
    }

    if ((m_flags & wxAUI_TB_TEXT) && !item.GetLabel().empty())
    {
        dc.DrawText(item.GetLabel(), textX, textY);
    }
}

BBLTopbar::BBLTopbar(wxFrame* parent) 
    : wxAuiToolBar(parent, ID_TOOL_BAR, wxDefaultPosition, wxDefaultSize, wxAUI_TB_TEXT | wxAUI_TB_HORZ_TEXT)
{ 
    Init(parent);
}

BBLTopbar::BBLTopbar(wxWindow* pwin, wxFrame* parent)
    : wxAuiToolBar(pwin, ID_TOOL_BAR, wxDefaultPosition, wxDefaultSize, wxAUI_TB_TEXT | wxAUI_TB_HORZ_TEXT) 
{ 
    Init(parent);
}

void BBLTopbar::Init(wxFrame* parent) 
{
    auto* art = new BBLTopbarArt();
    art->SetFont(Label::Body_12);
    SetArtProvider(art);
    m_frame = parent;
    m_skip_popup_file_menu = false;
    m_skip_popup_dropdown_menu = false;
    m_skip_popup_calib_menu    = false;

    wxInitAllImageHandlers();

    bool window_btns_on_left = false;

#ifdef __linux__
    window_btns_on_left = wxGetApp().app_config->get("window_buttons_on_left")  == "true";

    if(window_btns_on_left){
        wxBitmap close_bitmap = create_scaled_bitmap("topbar_close", nullptr, TOPBAR_ICON_SIZE);
        wxAuiToolBarItem* close_btn = this->AddTool(wxID_CLOSE_FRAME, "", close_bitmap, _L("Close"));

        this->AddSpacer(FromDIP(4));

        maximize_bitmap = create_scaled_bitmap("topbar_max", nullptr, TOPBAR_ICON_SIZE);
        window_bitmap = create_scaled_bitmap("topbar_win", nullptr, TOPBAR_ICON_SIZE);
        if (m_frame->IsMaximized()) {
            maximize_btn = this->AddTool(wxID_MAXIMIZE_FRAME, "", window_bitmap, _L("Maximize/Restore"));
        }
        else {
            maximize_btn = this->AddTool(wxID_MAXIMIZE_FRAME, "", maximize_bitmap, _L("Maximize/Restore"));
        }

        this->AddSpacer(FromDIP(4));

        wxBitmap iconize_bitmap = create_scaled_bitmap("topbar_min", nullptr, TOPBAR_ICON_SIZE);
        wxAuiToolBarItem* iconize_btn = this->AddTool(wxID_ICONIZE_FRAME, "", iconize_bitmap, _L("Minimize"));

        this->AddSpacer(15);
    }
#endif

    if(!window_btns_on_left)
        this->AddSpacer(5);

    /*wxBitmap logo_bitmap = create_scaled_bitmap("topbar_logo", nullptr, TOPBAR_ICON_SIZE);
    wxAuiToolBarItem* logo_item = this->AddTool(ID_LOGO, "", logo_bitmap);
    logo_item->SetHoverBitmap(logo_bitmap);
    logo_item->SetActive(false);*/

    wxBitmap file_bitmap = create_scaled_bitmap("topbar_file", nullptr, TOPBAR_ICON_SIZE);
    m_file_menu_item = this->AddTool(ID_TOP_FILE_MENU, _L("File"), file_bitmap, wxEmptyString, wxITEM_NORMAL);

    this->SetForegroundColour(topbar_text());

    this->AddSpacer(FromDIP(5));

    wxBitmap dropdown_bitmap = create_scaled_bitmap("topbar_dropdown", nullptr, TOPBAR_ICON_SIZE);
    m_dropdown_menu_item = this->AddTool(ID_TOP_DROPDOWN_MENU, "",
        dropdown_bitmap, _L("Menu"));

    this->AddSpacer(FromDIP(5));
    this->AddSeparator();
    this->AddSpacer(FromDIP(5));

    //wxBitmap open_bitmap = create_scaled_bitmap("topbar_open", nullptr, TOPBAR_ICON_SIZE);
    //wxAuiToolBarItem* tool_item = this->AddTool(wxID_OPEN, "", open_bitmap);

    this->AddSpacer(FromDIP(10));

    wxBitmap save_bitmap = create_scaled_bitmap("topbar_save", nullptr, TOPBAR_ICON_SIZE);
    wxAuiToolBarItem* save_btn = this->AddTool(wxID_SAVE, "", save_bitmap, _L("Save project"));

    this->AddSpacer(FromDIP(10));

    wxBitmap undo_bitmap = create_scaled_bitmap("topbar_undo", nullptr, TOPBAR_ICON_SIZE);
    m_undo_item = this->AddTool(wxID_UNDO, "", undo_bitmap, _L("Undo"));
    wxBitmap undo_inactive_bitmap = create_scaled_bitmap("topbar_undo_inactive", nullptr, TOPBAR_ICON_SIZE);
    m_undo_item->SetDisabledBitmap(undo_inactive_bitmap);

    this->AddSpacer(FromDIP(10));

    wxBitmap redo_bitmap = create_scaled_bitmap("topbar_redo", nullptr, TOPBAR_ICON_SIZE);
    m_redo_item = this->AddTool(wxID_REDO, "", redo_bitmap, _L("Redo"));
    wxBitmap redo_inactive_bitmap = create_scaled_bitmap("topbar_redo_inactive", nullptr, TOPBAR_ICON_SIZE);
    m_redo_item->SetDisabledBitmap(redo_inactive_bitmap);

    this->AddSpacer(FromDIP(10));

    wxBitmap calib_bitmap          = create_scaled_bitmap("calib_sf", nullptr, TOPBAR_ICON_SIZE);
    wxBitmap calib_bitmap_inactive = create_scaled_bitmap("calib_sf_inactive", nullptr, TOPBAR_ICON_SIZE);
    m_calib_item                   = this->AddTool(ID_CALIB, _L("Calibration"), calib_bitmap);
    m_calib_item->SetDisabledBitmap(calib_bitmap_inactive);

    this->AddSpacer(FromDIP(25));
    //this->AddStretchSpacer(1);

    m_title_ctrl = new CenteredTitle(this);
    m_title_ctrl->SetFont(Label::Body_12);
    wxAuiToolBarItem* title_item = this->AddControl(m_title_ctrl, "");
    title_item->SetProportion(1); 

    this->AddSpacer(FromDIP(25));
    //this->AddStretchSpacer(1);

    //this->EnableTool(m_publish_item->GetId(), false);
    //this->AddSpacer(FromDIP(4));

    /*wxBitmap model_store_bitmap = create_scaled_bitmap("topbar_store", nullptr, TOPBAR_ICON_SIZE);
    m_model_store_item = this->AddTool(ID_MODEL_STORE, "", model_store_bitmap);
    this->AddSpacer(12);
    */

    //this->AddSeparator();
    //this->AddSpacer(FromDIP(4));

    if(!window_btns_on_left){
    wxBitmap iconize_bitmap = create_scaled_bitmap("topbar_min", nullptr, TOPBAR_ICON_SIZE);
    wxAuiToolBarItem* iconize_btn = this->AddTool(wxID_ICONIZE_FRAME, "", iconize_bitmap, _L("Minimize"));

    this->AddSpacer(FromDIP(4));

    maximize_bitmap = create_scaled_bitmap("topbar_max", nullptr, TOPBAR_ICON_SIZE);
    window_bitmap = create_scaled_bitmap("topbar_win", nullptr, TOPBAR_ICON_SIZE);
    if (m_frame->IsMaximized()) {
        maximize_btn = this->AddTool(wxID_MAXIMIZE_FRAME, "", window_bitmap, _L("Maximize/Restore"));
    }
    else {
        maximize_btn = this->AddTool(wxID_MAXIMIZE_FRAME, "", maximize_bitmap, _L("Maximize/Restore"));
    }

    this->AddSpacer(FromDIP(4));

    wxBitmap close_bitmap = create_scaled_bitmap("topbar_close", nullptr, TOPBAR_ICON_SIZE);
    wxAuiToolBarItem* close_btn = this->AddTool(wxID_CLOSE_FRAME, "", close_bitmap, _L("Close"));
    }

    Realize();
    // m_toolbar_h = this->GetSize().GetHeight();
    m_toolbar_h = FromDIP(30);

    int client_w = parent->GetClientSize().GetWidth();
    this->SetSize(client_w, m_toolbar_h);

    this->Bind(wxEVT_MOTION, &BBLTopbar::OnMouseMotion, this);
    this->Bind(wxEVT_MOUSE_CAPTURE_LOST, &BBLTopbar::OnMouseCaptureLost, this);
    this->Bind(wxEVT_MENU_CLOSE, &BBLTopbar::OnMenuClose, this);
    this->Bind(wxEVT_AUITOOLBAR_TOOL_DROPDOWN, &BBLTopbar::OnFileToolItem, this, ID_TOP_FILE_MENU);
    this->Bind(wxEVT_AUITOOLBAR_TOOL_DROPDOWN, &BBLTopbar::OnDropdownToolItem, this, ID_TOP_DROPDOWN_MENU);
    this->Bind(wxEVT_AUITOOLBAR_TOOL_DROPDOWN, &BBLTopbar::OnCalibToolItem, this, ID_CALIB);
    this->Bind(wxEVT_AUITOOLBAR_TOOL_DROPDOWN, &BBLTopbar::OnIconize, this, wxID_ICONIZE_FRAME);
    this->Bind(wxEVT_AUITOOLBAR_TOOL_DROPDOWN, &BBLTopbar::OnFullScreen, this, wxID_MAXIMIZE_FRAME);
    this->Bind(wxEVT_AUITOOLBAR_TOOL_DROPDOWN, &BBLTopbar::OnCloseFrame, this, wxID_CLOSE_FRAME);
    this->Bind(wxEVT_LEFT_DCLICK, &BBLTopbar::OnMouseLeftDClock, this);
    this->Bind(wxEVT_LEFT_DOWN, &BBLTopbar::OnMouseLeftDown, this);
    this->Bind(wxEVT_LEFT_UP, &BBLTopbar::OnMouseLeftUp, this);
    this->Bind(wxEVT_KEY_DOWN, &BBLTopbar::OnKeyDown, this);
    this->Bind(wxEVT_KILL_FOCUS, [this](wxFocusEvent& event) {
        // Focus moving elsewhere ends keyboard mode without pulling it back.
        if (!m_popup_open) {
            m_keyboard_return_focus = nullptr;
            SetKeyboardTool(-1);
        }
        event.Skip();
    });
    this->Bind(wxEVT_LEAVE_WINDOW, [this](wxMouseEvent& event) {
        m_last_mouse_position = wxDefaultPosition;
        event.Skip();
    });
    this->Bind(wxEVT_AUITOOLBAR_TOOL_DROPDOWN, &BBLTopbar::OnOpenProject, this, wxID_OPEN);
    this->Bind(wxEVT_AUITOOLBAR_TOOL_DROPDOWN, &BBLTopbar::OnSaveProject, this, wxID_SAVE);
    this->Bind(wxEVT_AUITOOLBAR_TOOL_DROPDOWN, &BBLTopbar::OnRedo, this, wxID_REDO);
    this->Bind(wxEVT_AUITOOLBAR_TOOL_DROPDOWN, &BBLTopbar::OnUndo, this, wxID_UNDO);
    //this->Bind(wxEVT_AUITOOLBAR_TOOL_DROPDOWN, &BBLTopbar::OnModelStoreClicked, this, ID_MODEL_STORE);
}

BBLTopbar::~BBLTopbar()
{
    m_file_menu_item = nullptr;
    m_dropdown_menu_item = nullptr;
    m_file_menu = nullptr;
}

void BBLTopbar::OnOpenProject(wxAuiToolBarEvent& event)
{
    MainFrame* main_frame = dynamic_cast<MainFrame*>(m_frame);
    Plater* plater = main_frame->plater();
    plater->load_project();
}

void BBLTopbar::OnSaveProject(wxAuiToolBarEvent& event)
{
    MainFrame* main_frame = dynamic_cast<MainFrame*>(m_frame);
    Plater* plater = main_frame->plater();
    plater->save_project();
}

void BBLTopbar::OnUndo(wxAuiToolBarEvent& event)
{
    MainFrame* main_frame = dynamic_cast<MainFrame*>(m_frame);
#ifdef SLIC3R_CAD
    if (DesignPanel* design = main_frame->shown_design_panel()) { design->menu_undo_redo(false); return; }
#endif
    Plater* plater = main_frame->plater();
    plater->undo();
}

void BBLTopbar::OnRedo(wxAuiToolBarEvent& event)
{
    MainFrame* main_frame = dynamic_cast<MainFrame*>(m_frame);
#ifdef SLIC3R_CAD
    if (DesignPanel* design = main_frame->shown_design_panel()) { design->menu_undo_redo(true); return; }
#endif
    Plater* plater = main_frame->plater();
    plater->redo();
}

void BBLTopbar::EnableUndoRedoItems()
{
    this->EnableTool(m_undo_item->GetId(), true);
    this->EnableTool(m_redo_item->GetId(), true);
    this->EnableTool(m_calib_item->GetId(), true);
    Refresh();
}

void BBLTopbar::EnableUndoRedo(bool undo, bool redo)
{
    this->EnableTool(m_undo_item->GetId(), undo);
    this->EnableTool(m_redo_item->GetId(), redo);
    Refresh();
}

void BBLTopbar::DisableUndoRedoItems()
{
    this->EnableTool(m_undo_item->GetId(), false);
    this->EnableTool(m_redo_item->GetId(), false);
    this->EnableTool(m_calib_item->GetId(), false);
    Refresh();
}

void BBLTopbar::SaveNormalRect()
{
    m_normalRect = m_frame->GetRect();
}

void BBLTopbar::ShowCalibrationButton(bool show)
{
    m_calib_item->GetSizerItem()->Show(show);
    m_sizer->Layout();
    if (!show)
        m_calib_item->GetSizerItem()->SetDimension({-1000, 0}, {0, 0});
    Refresh();
}

void BBLTopbar::OnModelStoreClicked(wxAuiToolBarEvent& event)
{
    //GUI::wxGetApp().load_url(wxString(wxGetApp().app_config->get_web_host_url() + MODEL_STORE_URL));
}

void BBLTopbar::SetFileMenu(wxMenu* file_menu)
{
    m_file_menu = file_menu;
}

void BBLTopbar::AddDropDownSubMenu(wxMenu* sub_menu, const wxString& title)
{
    m_top_menu.AppendSubMenu(sub_menu, title);
}

void BBLTopbar::AddDropDownMenuItem(wxMenuItem* menu_item)
{
    m_top_menu.Append(menu_item);
}

wxMenu* BBLTopbar::GetTopMenu()
{
    return &m_top_menu;
}

wxMenu* BBLTopbar::GetCalibMenu()
{
    return &m_calib_menu;
}

void BBLTopbar::SetTitle(wxString title)
{
    m_titleText = title;
    if (m_title_ctrl)
        m_title_ctrl->SetTitle(title);
}

void BBLTopbar::SetMaximizedSize()
{
    maximize_btn->SetBitmap(maximize_bitmap);
}

void BBLTopbar::SetWindowSize()
{
    maximize_btn->SetBitmap(window_bitmap);
}

void BBLTopbar::UpdateToolbarWidth(int width)
{
    this->SetSize(width, m_toolbar_h);
}

void BBLTopbar::Rescale() {
    int em = em_unit(this);
    wxAuiToolBarItem* item;

    /*item = this->FindTool(ID_LOGO);
    item->SetBitmap(create_scaled_bitmap("topbar_logo", nullptr, TOPBAR_ICON_SIZE));*/

    item = this->FindTool(ID_TOP_FILE_MENU);
    item->SetBitmap(create_scaled_bitmap("topbar_file", this, TOPBAR_ICON_SIZE));

    item = this->FindTool(ID_TOP_DROPDOWN_MENU);
    item->SetBitmap(create_scaled_bitmap("topbar_dropdown", this, TOPBAR_ICON_SIZE));

    //item = this->FindTool(wxID_OPEN);
    //item->SetBitmap(create_scaled_bitmap("topbar_open", nullptr, TOPBAR_ICON_SIZE));

    item = this->FindTool(wxID_SAVE);
    item->SetBitmap(create_scaled_bitmap("topbar_save", this, TOPBAR_ICON_SIZE));

    item = this->FindTool(wxID_UNDO);
    item->SetBitmap(create_scaled_bitmap("topbar_undo", this, TOPBAR_ICON_SIZE));
    item->SetDisabledBitmap(create_scaled_bitmap("topbar_undo_inactive", this, TOPBAR_ICON_SIZE));

    item = this->FindTool(wxID_REDO);
    item->SetBitmap(create_scaled_bitmap("topbar_redo", this, TOPBAR_ICON_SIZE));
    item->SetDisabledBitmap(create_scaled_bitmap("topbar_redo_inactive", this, TOPBAR_ICON_SIZE));

    item = this->FindTool(ID_CALIB);
    item->SetBitmap(create_scaled_bitmap("calib_sf", this, TOPBAR_ICON_SIZE));
    item->SetDisabledBitmap(create_scaled_bitmap("calib_sf_inactive", this, TOPBAR_ICON_SIZE));

    if (m_title_ctrl)
        m_title_ctrl->SetTitle(m_titleText);

    /*item = this->FindTool(ID_MODEL_STORE);
    item->SetBitmap(create_scaled_bitmap("topbar_store", this, TOPBAR_ICON_SIZE));
    */

    item = this->FindTool(wxID_ICONIZE_FRAME);
    item->SetBitmap(create_scaled_bitmap("topbar_min", this, TOPBAR_ICON_SIZE));

    item = this->FindTool(wxID_MAXIMIZE_FRAME);
    maximize_bitmap = create_scaled_bitmap("topbar_max", this, TOPBAR_ICON_SIZE);
    window_bitmap   = create_scaled_bitmap("topbar_win", this, TOPBAR_ICON_SIZE);
    if (m_frame->IsMaximized()) {
        item->SetBitmap(window_bitmap);
    }
    else {
        item->SetBitmap(maximize_bitmap);
    }

    item = this->FindTool(wxID_CLOSE_FRAME);
    item->SetBitmap(create_scaled_bitmap("topbar_close", this, TOPBAR_ICON_SIZE));

    Realize();
}

void BBLTopbar::OnIconize(wxAuiToolBarEvent& event)
{
    m_frame->Iconize();
}

void BBLTopbar::OnFullScreen(wxAuiToolBarEvent& event)
{
#ifdef __WXGTK__
    GtkWindow* gtk_window = GTK_WINDOW(m_frame->m_widget);
    if (gtk_window_is_maximized(gtk_window)) {
        gtk_window_unmaximize(gtk_window);
    }
    else {
        m_normalRect = m_frame->GetRect();
        gtk_window_maximize(gtk_window);
    }
    return;
#endif

    if (m_frame->IsMaximized()) {
        m_frame->Restore();
    }
    else {
        m_normalRect = m_frame->GetRect();
        m_frame->Maximize();
    }
}

void BBLTopbar::OnCloseFrame(wxAuiToolBarEvent& event)
{
    m_frame->Close();
}

void BBLTopbar::OnMouseLeftDClock(wxMouseEvent& mouse)
{
    m_last_mouse_position = mouse.GetPosition();
    wxPoint mouse_pos = this->ClientToScreen(mouse.GetPosition());
    wxAuiToolBarItem* item = this->FindToolByPosition(mouse.GetX(), mouse.GetY());
    // check whether mouse is not on any tool item
    if (item != NULL && item->GetWindow() != m_title_ctrl) {
        mouse.Skip();
        return;
    }
#ifdef __W1XMSW__
    ::PostMessage((HWND) m_frame->GetHandle(), WM_NCLBUTTONDBLCLK, HTCAPTION, MAKELPARAM(mouse_pos.x, mouse_pos.y));
    return;
#endif //  __WXMSW__

    wxAuiToolBarEvent evt;
    OnFullScreen(evt);
}

void BBLTopbar::PopupToolMenu(int tool_id, wxMenu* menu, bool& skip_popup)
{
    SetToolSticky(tool_id, true);

    if (!skip_popup) {
        const wxRect rec = GetToolRect(tool_id);
        m_popup_open     = true;
        GetParent()->PopupMenu(menu, wxPoint(rec.GetLeft(), GetSize().GetHeight() - 2));
        m_popup_open = false;
    }
    else {
        skip_popup = false;
    }

    // make sure the button is "un-stuck", unless it carries the keyboard highlight
    SetToolSticky(tool_id, tool_id == m_keyboard_tool);
}

void BBLTopbar::OnFileToolItem(wxAuiToolBarEvent&)     { PopupToolMenu(ID_TOP_FILE_MENU, m_file_menu, m_skip_popup_file_menu); }
void BBLTopbar::OnDropdownToolItem(wxAuiToolBarEvent&) { PopupToolMenu(ID_TOP_DROPDOWN_MENU, &m_top_menu, m_skip_popup_dropdown_menu); }
void BBLTopbar::OnCalibToolItem(wxAuiToolBarEvent&)    { PopupToolMenu(ID_CALIB, &m_calib_menu, m_skip_popup_calib_menu); }

void BBLTopbar::ToggleKeyboardMode()
{
    if (m_keyboard_tool >= 0) {
        SetKeyboardTool(-1);
        return;
    }
    m_keyboard_return_focus = wxWindow::FindFocus();
    SetCanFocus(true); // the toolbar is a decorative container for Tab, but Alt/F10 focus it on purpose
    SetFocus();
    SetKeyboardTool(ID_TOP_FILE_MENU);
}

void BBLTopbar::SetKeyboardTool(int tool_id)
{
    if (m_keyboard_tool >= 0)
        SetToolSticky(m_keyboard_tool, false);
    m_keyboard_tool = tool_id;
    if (tool_id >= 0)
        SetToolSticky(tool_id, true);
    else if (wxWindow* back = m_keyboard_return_focus) {
        m_keyboard_return_focus = nullptr;
        back->SetFocus();
    }
    Refresh();
}

void BBLTopbar::OnKeyDown(wxKeyEvent& event)
{
    if (m_keyboard_tool < 0) {
        event.Skip();
        return;
    }
    switch (event.GetKeyCode()) {
    case WXK_LEFT:
    case WXK_RIGHT: {
        // The menus in bar order, skipping one that is hidden (Calibration) or disabled.
        std::vector<int> tools;
        for (int id : { ID_TOP_FILE_MENU, ID_TOP_DROPDOWN_MENU, ID_CALIB })
            if (wxAuiToolBarItem* item = FindTool(id); item != nullptr && item->GetSizerItem()->IsShown() && GetToolEnabled(id))
                tools.push_back(id);
        const auto it  = std::find(tools.begin(), tools.end(), m_keyboard_tool);
        int        pos = it == tools.end() ? 0 : int(it - tools.begin());
        pos            = (pos + (event.GetKeyCode() == WXK_RIGHT ? 1 : int(tools.size()) - 1)) % int(tools.size());
        SetKeyboardTool(tools[pos]);
        break;
    }
    case WXK_RETURN:
    case WXK_NUMPAD_ENTER:
    case WXK_SPACE:
    case WXK_DOWN:
        // A mouse-closed menu leaves its skip flag set; a keyboard open must not be eaten by it.
        if (m_keyboard_tool == ID_TOP_FILE_MENU) {
            m_skip_popup_file_menu = false;
            PopupToolMenu(ID_TOP_FILE_MENU, m_file_menu, m_skip_popup_file_menu);
        }
        else if (m_keyboard_tool == ID_TOP_DROPDOWN_MENU) {
            m_skip_popup_dropdown_menu = false;
            PopupToolMenu(ID_TOP_DROPDOWN_MENU, &m_top_menu, m_skip_popup_dropdown_menu);
        }
        else {
            m_skip_popup_calib_menu = false;
            PopupToolMenu(ID_CALIB, &m_calib_menu, m_skip_popup_calib_menu);
        }
        break;
    case WXK_ESCAPE:
        SetKeyboardTool(-1);
        break;
    default:
        event.Skip();
    }
}

void BBLTopbar::OnMouseLeftDown(wxMouseEvent& event)
{
    m_last_mouse_position = event.GetPosition();
    // Use event-relative coords converted to screen, instead of wxGetMousePosition()
    // which returns (0,0) on Wayland for global screen coordinates.
    wxPoint mouse_pos = this->ClientToScreen(event.GetPosition());
    wxPoint frame_pos = m_frame->GetScreenPosition();
    wxAuiToolBarItem* item = this->FindToolByPosition(event.GetX(), event.GetY());
    m_delta = mouse_pos - frame_pos;

    if (item == NULL || item->GetWindow() == m_title_ctrl)
    {
#ifdef __WXMSW__
        CaptureMouse();
        ReleaseMouse();
        ::PostMessage((HWND) m_frame->GetHandle(), WM_NCLBUTTONDOWN, HTCAPTION, MAKELPARAM(mouse_pos.x, mouse_pos.y));
        return;
#elif defined(__WXGTK__)
        // Use WM-integrated drag for smoother window movement on Linux.
        gtk_window_begin_move_drag(
            GTK_WINDOW(m_frame->m_widget),
            1,  // left mouse button
            mouse_pos.x, mouse_pos.y,
            gtk_get_current_event_time());
        return;
#else
        CaptureMouse();
#endif
    }

    event.Skip();
}

void BBLTopbar::OnMouseLeftUp(wxMouseEvent& event)
{
    m_last_mouse_position = event.GetPosition();
    wxPoint mouse_pos = this->ClientToScreen(event.GetPosition());
    if (HasCapture())
    {
        ReleaseMouse();
    }

    event.Skip();
}

void BBLTopbar::OnMouseMotion(wxMouseEvent& event)
{
    m_last_mouse_position = event.GetPosition();
    // Use event-relative coords converted to screen, instead of wxGetMousePosition()
    // which returns (0,0) on Wayland for global screen coordinates.
    wxPoint mouse_pos = this->ClientToScreen(event.GetPosition());

    if (!HasCapture()) {
        //m_frame->OnMouseMotion(event);

        event.Skip();
        return;
    }

    if (event.Dragging() && event.LeftIsDown())
    {
        // leave max state and adjust position 
        if (m_frame->IsMaximized()) {
            wxRect rect = m_frame->GetRect();
            // Filter unexcept mouse move
            if (m_delta + rect.GetLeftTop() != mouse_pos) {
                m_delta = mouse_pos - rect.GetLeftTop();
                m_delta.x = m_delta.x * m_normalRect.width / rect.width;
                m_delta.y = m_delta.y * m_normalRect.height / rect.height;
                m_frame->Restore();
            }
        }
        m_frame->Move(mouse_pos - m_delta);
    }
    event.Skip();
}

void BBLTopbar::OnMouseCaptureLost(wxMouseCaptureLostEvent& event)
{
}

void BBLTopbar::OnMenuClose(wxMenuEvent& event)
{
    wxAuiToolBarItem* item = this->FindToolByCurrentPosition();
    if (item == m_file_menu_item) {
        m_skip_popup_file_menu = true;
    }
    else if (item == m_dropdown_menu_item) {
        m_skip_popup_dropdown_menu = true;
    }
}

wxAuiToolBarItem* BBLTopbar::FindToolByCurrentPosition()
{
    if (m_last_mouse_position != wxDefaultPosition && GetClientRect().Contains(m_last_mouse_position))
        return this->FindToolByPosition(m_last_mouse_position.x, m_last_mouse_position.y);

#ifdef __WXGTK__
    if (Slic3r::GUI::is_running_on_wayland())
        return nullptr;
#endif

    wxPoint mouse_pos = ::wxGetMousePosition();
    wxPoint client_pos = this->ScreenToClient(mouse_pos);
    return this->FindToolByPosition(client_pos.x, client_pos.y);
}

#ifdef __WXMSW__
#include <windowsx.h>

WXLRESULT CenteredTitle::MSWWindowProc(WXUINT nMsg, WXWPARAM wParam, WXLPARAM lParam)
{
    switch (nMsg) {
    case WM_NCHITTEST: {
        // Pass all mouse event to parent
        return HTTRANSPARENT;
    }
    }

    return wxControl::MSWWindowProc(nMsg, wParam, lParam);
}

WXLRESULT BBLTopbar::MSWWindowProc(WXUINT nMsg, WXWPARAM wParam, WXLPARAM lParam)
{
    switch (nMsg) {
    case WM_NCHITTEST: {
        m_last_mouse_position = ScreenToClient({GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)});

        wxAuiToolBarItem* item = this->FindToolByCurrentPosition();
        if (item != NULL && item->GetWindow() != m_title_ctrl) {
            break;
        }

        // Pass the event to main window if mouse is on the top bar and not on any of the buttons
        return HTTRANSPARENT;
    }
    }

    return wxAuiToolBar::MSWWindowProc(nMsg, wParam, lParam);
}
#endif
