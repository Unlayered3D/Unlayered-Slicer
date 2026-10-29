#include "Preferences.hpp"
#include "OptionsGroup.hpp"
#include "GUI_App.hpp"
#include "FocusRing.hpp"
#include "Theme.hpp"
#include <wx/clrpicker.h>
#include <wx/textdlg.h>
#include <wx/filedlg.h>
#include <memory>
#ifdef __WXGTK__
#include <gtk/gtk.h>
#endif
#include "MainFrame.hpp"
#include "Plater.hpp"
#include "GLCanvas3D.hpp" // ORCA: for live preview refresh when toggling "Dim lower layers"
#include "MsgDialog.hpp"
#include "I18N.hpp"
#include "WebAppPanel.hpp"
#include "WebAppBridge.hpp"
#include "SliceServiceOptionsDialog.hpp"
#include "libslic3r/AppConfig.hpp"
#include "libslic3r/Format/DRC.hpp"
#include <wx/language.h>
#include "OG_CustomCtrl.hpp"
#include "wx/graphics.h"
#include <wx/listimpl.cpp>
#include <wx/display.h>
#include "NetworkTestDialog.hpp"
#include "Widgets/StaticLine.hpp"
#include "Widgets/RadioGroup.hpp"
#include "slic3r/Utils/bambu_networking.hpp"
#include "slic3r/Utils/NetworkAgent.hpp"
#include "slic3r/Utils/UsageTelemetry.hpp"
#include "slic3r/Utils/PrivacyConsent.hpp"
#include "slic3r/Utils/PrivacyChoiceStats.hpp"
#include "PrivacySetupDialog.hpp"
#include "NetworkPluginDialog.hpp"
#include "DownloadProgressDialog.hpp"

#ifdef __WINDOWS__
#ifdef _MSW_DARK_MODE
#include "dark_mode.hpp"
#endif // _MSW_DARK_MODE
#endif //__WINDOWS__

namespace Slic3r { namespace GUI {

class MyscrolledWindow : public wxScrolledWindow {
public:
    MyscrolledWindow(wxWindow* parent,
        wxWindowID id = wxID_ANY,
        const wxPoint& pos = wxDefaultPosition,
        const wxSize& size = wxDefaultSize,
        long style = wxVSCROLL) : wxScrolledWindow(parent, id, pos, size, style) {}

    bool ShouldScrollToChildOnFocus(wxWindow* child) override { return false; }
};

// TODO before replacing with HyperLink class
// make Wrap(-1) and Wrap(width) functional
// ellipsize_end on wrap(-1)
// add SetUnderlined() for allowing always highlighted while using as HyperLink
class WikiLabel : public wxPanel {
private:
    wxString      m_label;
    wxString      m_url;
    wxArrayString m_lines;
    bool          m_hovered = false;
    wxFont        m_font;
    int           m_last_wrap_width = -1;

public:
    WikiLabel(
        wxWindow*       parent,
        const wxString& label,
        const wxString& url     = wxEmptyString,
        const wxPoint&  pos     = wxDefaultPosition,
        const wxSize&   size    = wxDefaultSize
    )
        : wxPanel(parent, wxID_ANY, pos, size, wxFULL_REPAINT_ON_RESIZE)
        , m_label(label)
        , m_url(url)
    {
#ifndef __WXOSX__ 
        SetDoubleBuffered(true);// SetDoubleBuffered exists on Win and Linux/GTK, but is missing on OSX
#endif
        SetBackgroundColour(parent->GetBackgroundColour());

        SetFont(Label::Body_14);

        Bind(wxEVT_PAINT,        &WikiLabel::OnPaint, this);
        Bind(wxEVT_SIZE,         &WikiLabel::OnSize, this);
        Bind(wxEVT_MOTION,       &WikiLabel::OnMotion, this);
        Bind(wxEVT_LEAVE_WINDOW, &WikiLabel::OnLeaveWin, this);
        Bind(wxEVT_LEFT_DOWN,    &WikiLabel::OnLeftDown, this);
    }

    void SetLabel(const wxString& label) override
    {
        m_label = label;
        m_last_wrap_width = -1; // force re-wrap
        ReflowText();
        Refresh();
    }

    bool SetFont(const wxFont& font) override
    {
        const bool changed = wxPanel::SetFont(font);
        m_font = font;
        m_last_wrap_width = -1; // force re-wrap
        if (IsShownOnScreen()) {
            ReflowText();
            Refresh();
        }
        return changed;
    }

    wxString  GetLabel()  const override   { return m_label; }
    void      SetURL(const wxString& url)  { m_url = url; }
    wxString  GetURL()    const            { return m_url; }

    void ReflowText()
    {
        const int clientW = GetClientSize().GetWidth();
 
        if (clientW <= 0 || (clientW == m_last_wrap_width && !m_lines.IsEmpty()))
            return;

        m_last_wrap_width = clientW;

        wxArrayString lines;
        for (const wxString& para : wxSplit(m_label, '\n')) {
            if (para.IsEmpty())
                lines.Add(wxEmptyString);
            else {
                wxString currentLine;
                for (const wxString& word : wxSplit(para, ' ')) {
                    wxString candidate = currentLine.IsEmpty() ? word : (currentLine + ' ' + word);

                    if (GetTextExtent(candidate).GetWidth() <= clientW)
                        currentLine = candidate;
                    else {
                        if (currentLine.IsEmpty())
                            lines.Add(word); // single word wider than column
                        else {
                            lines.Add(currentLine);
                            currentLine = word;
                        }
                    }
                }
                if (!currentLine.IsEmpty())
                    lines.Add(currentLine);
            }
        }
        m_lines = lines;
 
        const int lineH  = wxMax(1, wxWindow::GetCharHeight()); // GTK can return 0 from GetCharHeight() before the window is realized
        const int nLines = m_lines.IsEmpty() ? 1 : static_cast<int>(m_lines.size());
        const int totalH = static_cast<int>(nLines * lineH * 1.3);
 
        SetMinSize(wxSize(-1, totalH));
        InvalidateBestSize();
    }
 
    wxSize DoGetBestSize() const override
    {
        const int lineH  = wxMax(1, wxWindow::GetCharHeight()); // GTK can return 0 from GetCharHeight() before the window is realized
        const int nLines = m_lines.IsEmpty() ? 1 : static_cast<int>(m_lines.size());
        const int totalH = static_cast<int>(nLines * lineH * 1.3);
 
        const int clientW = GetClientSize().GetWidth();
 
        if (clientW > 0)
            return wxSize(clientW, totalH);
 
        if (m_label.IsEmpty())
            return wxSize(1, lineH);
 
        int maxW = 0;
        for (const wxString& line : wxSplit(m_label, '\n'))
            maxW = wxMax(maxW, GetTextExtent(line).GetWidth());
 
        return wxSize(wxMax(1, maxW), totalH);
    }

    void Rescale()
    {
        m_last_wrap_width = -1; // force re-wrap
        m_lines.Clear();
        InvalidateBestSize();
    }
 
private:
    void OnPaint(wxPaintEvent& evt)
    {
        wxPaintDC dc(this);
 
        dc.SetBackground(wxBrush(GetParent() ? GetParent()->GetBackgroundColour() : *wxWHITE));
        dc.Clear();
 
        wxColour textCol = StateColor::darkModeColorFor(m_hovered ? "#26A69A" : "#363636");
 
        dc.SetTextForeground(textCol);
        dc.SetFont(m_font);
        dc.SetBackgroundMode(wxTRANSPARENT);
 
        int lineH = dc.GetCharHeight();
        int y     = lround(lineH * 0.15);
 
        for (const wxString& line : m_lines) {
            if (!line.IsEmpty()) {
                dc.DrawText(line, 0, y);

                if (m_hovered) {
                    int tw, th;
                    dc.GetTextExtent(line, &tw, &th);
 
                    int underlineY = y + lineH - 1; // 1 px below the baseline
                    dc.SetPen(wxPen(textCol, 1));
                    dc.DrawLine(0, underlineY, tw, underlineY);
                }
            }
            y += lineH;
        }
    }
 
    void OnSize(wxSizeEvent& evt)
    {
        ReflowText();
        Refresh();
        evt.Skip();
    }

    void OnMotion(wxMouseEvent& evt)
    {
        if(!m_url.IsEmpty() && !m_hovered){
            m_hovered = true;
            Refresh();
        }
        evt.Skip();
    }
 
    void OnLeaveWin(wxMouseEvent& evt)
    {
        if(!m_url.IsEmpty() && m_hovered){
            m_hovered = false;
            Refresh();
        }
        evt.Skip();
    }
 
    void OnLeftDown(wxMouseEvent& evt)
    {
        if (!m_url.IsEmpty())
            wxLaunchDefaultBrowser(m_url);
        evt.Skip();
    } 
};

wxBoxSizer *PreferencesDialog::create_item_title(wxString title)
{
    wxBoxSizer *m_sizer_title = new wxBoxSizer(wxHORIZONTAL);

    auto title_ctrl = new StaticLine(m_parent, 0, title);
    title_ctrl->SetFont(Label::Head_14);
    title_ctrl->SetForegroundColour(DESIGN_GRAY900_COLOR);
    m_sizer_title->AddSpacer(FromDIP(DESIGN_LEFT_MARGIN - 10));
    m_sizer_title->Add(title_ctrl, 1, wxEXPAND | wxBOTTOM | wxTOP, FromDIP(6));
    m_sizer_title->AddSpacer(FromDIP(DESIGN_LEFT_MARGIN - 10));

    return m_sizer_title;
}

wxBoxSizer *PreferencesDialog::create_item_label(wxString label, wxString tooltip, wxString wiki_url)
{
    wxBoxSizer *sizer = new wxBoxSizer(wxHORIZONTAL);
    sizer->AddSpacer(FromDIP(DESIGN_LEFT_MARGIN));

    wxString url;
    if(!wiki_url.IsEmpty())
        url = "https://www.orcaslicer.com/wiki/" + wiki_url;

    auto label_ctrl = new WikiLabel(m_parent, label, url, wxDefaultPosition, DESIGN_TITLE_SIZE);

    label_ctrl->SetToolTip(tooltip);

    sizer->Add(label_ctrl, 0, wxALIGN_CENTER | wxTOP | wxBOTTOM, FromDIP(3));
    sizer->AddSpacer(FromDIP(5));

    return sizer;
}

std::tuple<wxBoxSizer*, ComboBox*> PreferencesDialog::create_item_combobox_base(wxString title, wxString tooltip, std::string param, std::vector<wxString> vlist, unsigned int current_index, const wxString wiki_url)
{
    auto tip = tooltip.IsEmpty() ? title : tooltip; // auto fill tooltips with title if its empty

    wxBoxSizer *m_sizer = create_item_label(title, tip, wiki_url);

    auto combobox = new ::ComboBox(m_parent, wxID_ANY, wxEmptyString, wxDefaultPosition, DESIGN_LARGE_COMBOBOX_SIZE, 0, nullptr, wxCB_READONLY);
    combobox->GetDropDown().SetUseContentWidth(true);
    combobox->SetToolTip(tip);

    std::vector<wxString>::iterator iter;
    for (iter = vlist.begin(); iter != vlist.end(); iter++) {
        combobox->Append(*iter);
    }

    combobox->SetSelection(current_index);

    m_sizer->Add(combobox, 0, wxALIGN_CENTER);

    return {m_sizer, combobox};
}

wxBoxSizer* PreferencesDialog::create_item_combobox(wxString title, wxString tooltip, std::string param, std::vector<wxString> vlist, std::function<void(wxString)> onchange, const wxString wiki_url)
{
    unsigned int current_index = 0;

    auto current_setting = app_config->get(param);
    if (!current_setting.empty()) {
        current_index = atoi(current_setting.c_str());
    }

    auto [sizer, combobox] = create_item_combobox_base(title, tooltip, param, vlist, current_index, wiki_url);

    //// save config
    combobox->GetDropDown().Bind(wxEVT_COMBOBOX, [this, param, onchange](wxCommandEvent& e) {
        app_config->set(param, std::to_string(e.GetSelection()));
        if (onchange)
            onchange(std::to_string(e.GetSelection()));
        e.Skip();
    });

    return sizer;
}

wxBoxSizer *PreferencesDialog::create_item_combobox(wxString title, wxString tooltip, std::string param, std::vector<wxString> vlist, std::vector<std::string> config_name_index, const wxString wiki_url, std::function<void(const std::string&)> onchange)
{
    assert(vlist.size() == config_name_index.size());
    unsigned int current_index = 0;

    auto current_setting = app_config->get(param);
    if (!current_setting.empty()) {
        auto compare  = [current_setting](string possible_setting) { return current_setting == possible_setting; };
        auto iterator = find_if(config_name_index.begin(), config_name_index.end(), compare);
        if (iterator != config_name_index.end())
            current_index = static_cast<unsigned int>(iterator - config_name_index.begin());
    }

    auto [sizer, combobox] = create_item_combobox_base(title, tooltip, param, vlist, current_index);

    //// save config
    combobox->GetDropDown().Bind(wxEVT_COMBOBOX, [this, param, config_name_index, onchange](wxCommandEvent& e) {
        app_config->set(param, config_name_index[e.GetSelection()]);
        if (onchange)
            onchange(config_name_index[e.GetSelection()]);
        e.Skip();
    });

    return sizer;
}

wxBoxSizer *PreferencesDialog::create_item_language_combobox(wxString title, wxString tooltip)
{
    wxLanguage supported_languages[]{
        wxLANGUAGE_ENGLISH,
        wxLANGUAGE_CHINESE_SIMPLIFIED,
        wxLANGUAGE_CHINESE_TRADITIONAL,
        wxLANGUAGE_GERMAN,
        wxLANGUAGE_CZECH,
        wxLANGUAGE_FRENCH,
        wxLANGUAGE_SPANISH,
        wxLANGUAGE_SWEDISH,
        wxLANGUAGE_DUTCH,
        wxLANGUAGE_HUNGARIAN,
        wxLANGUAGE_JAPANESE,
        wxLANGUAGE_ITALIAN,
        wxLANGUAGE_KOREAN,
        wxLANGUAGE_RUSSIAN,
        wxLANGUAGE_UKRAINIAN,
        wxLANGUAGE_TURKISH,
        wxLANGUAGE_POLISH,
        wxLANGUAGE_CATALAN,
        wxLANGUAGE_PORTUGUESE_BRAZILIAN,
        wxLANGUAGE_LITHUANIAN,
        wxLANGUAGE_VIETNAMESE,
        wxLANGUAGE_THAI
    };

    auto translations = wxTranslations::Get()->GetAvailableTranslations(SLIC3R_APP_CATALOG);
    std::vector<const wxLanguageInfo *> language_infos;
    language_infos.emplace_back(wxLocale::GetLanguageInfo(wxLANGUAGE_ENGLISH));
    for (size_t i = 0; i < translations.GetCount(); ++i) {
        const wxLanguageInfo *langinfo = wxLocale::FindLanguageInfo(translations[i]);

        if (langinfo == nullptr) continue;
        int language_num = sizeof(supported_languages) / sizeof(supported_languages[0]);
        for (auto si = 0; si < language_num; si++) {
            if (langinfo == wxLocale::GetLanguageInfo(supported_languages[si])) {
                language_infos.emplace_back(langinfo);
            }
        }
        //if (langinfo != nullptr) language_infos.emplace_back(langinfo);
    }
    sort_remove_duplicates(language_infos);
    std::sort(language_infos.begin(), language_infos.end(), [](const wxLanguageInfo *l, const wxLanguageInfo *r) { return l->Description < r->Description; });

    auto vlist = language_infos;
    auto param = "language";

    auto tip = tooltip.IsEmpty() ? title : tooltip; // auto fill tooltips with title if its empty

    wxBoxSizer *m_sizer = create_item_label(title, tip);

    auto combobox = new ::ComboBox(m_parent, wxID_ANY, wxEmptyString, wxDefaultPosition, DESIGN_LARGE_COMBOBOX_SIZE, 0, nullptr, wxCB_READONLY);
    combobox->GetDropDown().SetUseContentWidth(true);
    combobox->SetToolTip(tip);

    auto language = app_config->get(param);
    m_current_language_selected = -1;
    std::vector<wxString>::iterator iter;
    for (size_t i = 0; i < vlist.size(); ++i) {
        auto language_name = vlist[i]->Description;

        if (vlist[i] == wxLocale::GetLanguageInfo(wxLANGUAGE_CHINESE_SIMPLIFIED)) {
            language_name = wxString::FromUTF8("\xe4\xb8\xad\xe6\x96\x87\x28\xe7\xae\x80\xe4\xbd\x93\x29");
        }
        else if (vlist[i] == wxLocale::GetLanguageInfo(wxLANGUAGE_CHINESE_TRADITIONAL)) {
            language_name = wxString::FromUTF8("\xe4\xb8\xad\xe6\x96\x87\x28\xe7\xb9\x81\xe9\xab\x94\x29");
        }
        else if (vlist[i] == wxLocale::GetLanguageInfo(wxLANGUAGE_SPANISH)) {
            language_name = wxString::FromUTF8("\x45\x73\x70\x61\xc3\xb1\x6f\x6c");
        }
        else if (vlist[i] == wxLocale::GetLanguageInfo(wxLANGUAGE_GERMAN)) {
            language_name = wxString::FromUTF8("Deutsch");
        }
        else if (vlist[i] == wxLocale::GetLanguageInfo(wxLANGUAGE_CZECH)) {
            language_name = wxString::FromUTF8("Czech");
        }
        else if (vlist[i] == wxLocale::GetLanguageInfo(wxLANGUAGE_SWEDISH)) {
            language_name = wxString::FromUTF8("\x53\x76\x65\x6e\x73\x6b\x61"); //Svenska
        }
        else if (vlist[i] == wxLocale::GetLanguageInfo(wxLANGUAGE_DUTCH)) {
            language_name = wxString::FromUTF8("Nederlands");
        }
        else if (vlist[i] == wxLocale::GetLanguageInfo(wxLANGUAGE_FRENCH)) {
            language_name = wxString::FromUTF8("\x46\x72\x61\x6E\xC3\xA7\x61\x69\x73");
        }
        else if (vlist[i] == wxLocale::GetLanguageInfo(wxLANGUAGE_HUNGARIAN)) {
            language_name = wxString::FromUTF8("Magyar");
        }
        else if (vlist[i] == wxLocale::GetLanguageInfo(wxLANGUAGE_JAPANESE)) {
            language_name = wxString::FromUTF8("\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E");
        }
        else if (vlist[i] == wxLocale::GetLanguageInfo(wxLANGUAGE_ITALIAN)) {
            language_name = wxString::FromUTF8("\x69\x74\x61\x6c\x69\x61\x6e\x6f");
        }
        else if (vlist[i] == wxLocale::GetLanguageInfo(wxLANGUAGE_KOREAN)) {
            language_name = wxString::FromUTF8("\xED\x95\x9C\xEA\xB5\xAD\xEC\x96\xB4");
        }
        else if (vlist[i] == wxLocale::GetLanguageInfo(wxLANGUAGE_RUSSIAN)) {
            language_name = wxString::FromUTF8("\xd0\xa0\xd1\x83\xd1\x81\xd1\x81\xd0\xba\xd0\xb8\xd0\xb9");
        }
        else if (vlist[i] == wxLocale::GetLanguageInfo(wxLANGUAGE_UKRAINIAN)) {
            language_name = wxString::FromUTF8("Ukrainian");
        }
        else if (vlist[i] == wxLocale::GetLanguageInfo(wxLANGUAGE_TURKISH)) {
            language_name = wxString::FromUTF8("Turkish");
        }
        else if (vlist[i] == wxLocale::GetLanguageInfo(wxLANGUAGE_POLISH)) {
            language_name = wxString::FromUTF8("Polski");
        }
        else if (vlist[i] == wxLocale::GetLanguageInfo(wxLANGUAGE_CATALAN)) {
            language_name = wxString::FromUTF8("Catalan");
        }
        else if (vlist[i] == wxLocale::GetLanguageInfo(wxLANGUAGE_PORTUGUESE_BRAZILIAN)) {
            language_name = wxString::FromUTF8("Português (Brasil)");
        }
        else if (vlist[i] == wxLocale::GetLanguageInfo(wxLANGUAGE_LITHUANIAN)) {
            language_name = wxString::FromUTF8("Lietuvių");
        }
        else if (vlist[i] == wxLocale::GetLanguageInfo(wxLANGUAGE_VIETNAMESE)) {
            language_name = wxString::FromUTF8("Tiếng Việt");
        }
        else if (vlist[i] == wxLocale::GetLanguageInfo(wxLANGUAGE_THAI)) {
            language_name = wxString::FromUTF8("\xE0\xB9\x84\xE0\xB8\x97\xE0\xB8\xA2");
        }

        if (app_config->get(param) == vlist[i]->CanonicalName) {
            m_current_language_selected = i;
        }
        combobox->Append(language_name);
    }
    if (m_current_language_selected == -1 && language.size() >= 5) {
        language = language.substr(0, 2);
        for (size_t i = 0; i < vlist.size(); ++i) {
            if (vlist[i]->CanonicalName.StartsWith(language)) {
                m_current_language_selected = i;
                break;
            }
        }
    }
    combobox->SetSelection(m_current_language_selected);

    m_sizer->Add(combobox, 0, wxALIGN_CENTER);

    combobox->Bind(wxEVT_LEFT_DOWN, [this, combobox](wxMouseEvent &e) {
        m_current_language_selected = combobox->GetSelection();
        e.Skip();
    });

    combobox->Bind(wxEVT_COMBOBOX, [this, param, vlist, combobox](wxCommandEvent &e) {
        if (combobox->GetSelection() == m_current_language_selected)
            return;

        if (e.GetString().ToStdString() != app_config->get(param)) {
            {
                //check if the project has changed
                if (wxGetApp().plater()->is_project_dirty()) {
                    auto result = MessageDialog(static_cast<wxWindow*>(this), _L("The current project has unsaved changes. Would you like to save before continuing\?"),
                        wxString(SLIC3R_APP_FULL_NAME) + " - " + _L("Save"), wxYES_NO | wxCANCEL | wxYES_DEFAULT | wxCENTRE).ShowModal();

                    if (result == wxID_YES) {
                        wxGetApp().plater()->save_project();
                    }
                }


                // the dialog needs to be destroyed before the call to switch_language()
                // or sometimes the application crashes into wxDialogBase() destructor
                // so we put it into an inner scope
                MessageDialog msg_wingow(nullptr, _L("Switching languages requires the application to restart.\n") + "\n" + _L("Do you want to continue?"),
                                         L("Language selection"), wxICON_QUESTION | wxOK | wxCANCEL);
                if (msg_wingow.ShowModal() == wxID_CANCEL) {
                    combobox->SetSelection(m_current_language_selected);
                    return;
                }
            }

            auto check = [](bool yes_or_no) {
                // if (yes_or_no)
                //    return true;
                int act_btns = ActionButtons::SAVE;
                return wxGetApp().check_and_keep_current_preset_changes(_L("Switching application language"),
                                                                        _L("Switching application language while some presets are modified."), act_btns);
            };

            m_current_language_selected = combobox->GetSelection();
            if (m_current_language_selected >= 0 && m_current_language_selected < vlist.size()) {
                m_pending_language = vlist[m_current_language_selected]->CanonicalName.ToUTF8().data();
                m_recreate_GUI = true;
                EndModal(wxID_OK);
                return;
            }
        }

        e.Skip();
    });

    return m_sizer;
}

wxBoxSizer *PreferencesDialog::create_item_region_combobox(wxString title, wxString tooltip)
{

    std::vector<wxString> Regions         = {_L("Asia-Pacific"), _L("China"), _L("Europe"), _L("North America"), _L("Others")};
    std::vector<wxString> local_regions = {"Asia-Pacific", "China", "Europe", "North America", "Others"};

    auto vlist = Regions;

    auto tip = tooltip.IsEmpty() ? title : tooltip; // auto fill tooltips with title if its empty

    wxBoxSizer *m_sizer = create_item_label(title, tip);

    auto combobox = new ::ComboBox(m_parent, wxID_ANY, wxEmptyString, wxDefaultPosition, DESIGN_LARGE_COMBOBOX_SIZE, 0, nullptr, wxCB_READONLY);
    combobox->GetDropDown().SetUseContentWidth(true);
    combobox->SetToolTip(tip);

    m_sizer->Add(combobox, 0, wxALIGN_CENTER);

    std::vector<wxString>::iterator iter;
    for (iter = vlist.begin(); iter != vlist.end(); iter++) { combobox->Append(*iter); }

    AppConfig * config       = GUI::wxGetApp().app_config;

    int         current_region = 0;
    if (!config->get("region").empty()) {
        std::string country_code = config->get("region");
        for (auto i = 0; i < vlist.size(); i++) {
            if (local_regions[i].ToStdString() == country_code) {
                combobox->SetSelection(i);
                current_region = i;
            }
        }
    }

    combobox->GetDropDown().Bind(wxEVT_COMBOBOX, [this, combobox, current_region, local_regions](wxCommandEvent &e) {
        auto region_index = e.GetSelection();
        auto region       = local_regions[region_index];

        /*auto area   = "";
        if (region == "CHN" || region == "China")
            area = "CN";
        else if (region == "USA")
            area = "US";
        else if (region == "Asia-Pacific")
            area = "Others";
        else if (region == "Europe")
            area = "US";
        else if (region == "North America")
            area = "US";
        else
            area = "Others";*/
        combobox->SetSelection(region_index);
        NetworkAgent* agent = wxGetApp().getAgent();
        AppConfig* config = GUI::wxGetApp().app_config;
        if (agent) {
            MessageDialog msg_wingow(this, _L("Changing the region will log you out of your account.\n") + "\n" + _L("Do you want to continue?"), _L("Region selection"),
                                     wxICON_QUESTION | wxOK | wxCANCEL);
            if (msg_wingow.ShowModal() == wxID_CANCEL) {
                combobox->SetSelection(current_region);
                return;
            } else {
                wxGetApp().request_user_logout();
                config->set("region", region.ToStdString());
                auto area = config->get_country_code();
                if (agent) {
                    agent->set_country_code(area);
                }
                EndModal(wxID_CANCEL);
            }
        } else {
            config->set("region", region.ToStdString());
        }

        wxGetApp().update_publish_status();
        e.Skip();
    });

    return m_sizer;
}

wxBoxSizer *PreferencesDialog::create_item_loglevel_combobox(wxString title, wxString tooltip, std::vector<wxString> vlist)
{
    auto tip = tooltip.IsEmpty() ? title : tooltip; // auto fill tooltips with title if its empty

    wxBoxSizer *m_sizer = create_item_label(title, tip);

    auto combobox = new ::ComboBox(m_parent, wxID_ANY, wxEmptyString, wxDefaultPosition, DESIGN_COMBOBOX_SIZE, 0, nullptr, wxCB_READONLY);
    combobox->GetDropDown().SetUseContentWidth(true);
    combobox->SetToolTip(tip);

    std::vector<wxString>::iterator iter;
    for (iter = vlist.begin(); iter != vlist.end(); iter++) { combobox->Append(*iter); }

    auto severity_level = app_config->get("log_severity_level");
    if (!severity_level.empty()) { combobox->SetValue(severity_level); }

    m_sizer->Add(combobox, 0, wxALIGN_CENTER);

    //// save config
    combobox->GetDropDown().Bind(wxEVT_COMBOBOX, [this](wxCommandEvent &e) {
        auto level = Slic3r::get_string_logging_level(e.GetSelection());
        Slic3r::set_logging_level(Slic3r::level_string_to_boost(level));
        app_config->set("log_severity_level",level);
        e.Skip();
     });
    return m_sizer;
}

wxBoxSizer *PreferencesDialog::create_item_input(wxString title, wxString title2, wxString tooltip, std::string param, std::function<void(wxString)> onchange, const wxString wiki_url, long validator_style)
{
    auto tip = tooltip.IsEmpty() ? title : tooltip; // auto fill tooltips with title if its empty

    wxBoxSizer *m_sizer = create_item_label(title, tip, wiki_url);

    auto       input = new ::TextInput(m_parent, wxEmptyString, wxEmptyString, wxEmptyString, wxDefaultPosition, DESIGN_INPUT_SIZE, wxTE_PROCESS_ENTER);
    StateColor input_bg(std::pair<wxColour, int>(wxColour("#F0F0F1"), StateColor::Disabled), std::pair<wxColour, int>(*wxWHITE, StateColor::Enabled));
    input->SetBackgroundColor(input_bg);
    input->GetTextCtrl()->SetValue(app_config->get(param));
    input->SetToolTip(tip);
    if (validator_style != wxFILTER_NONE) {
        wxTextValidator validator(validator_style);
        input->GetTextCtrl()->SetValidator(validator);
    }

    auto second_title = new wxStaticText(m_parent, wxID_ANY, title2, wxDefaultPosition, wxDefaultSize, 0);
    second_title->SetForegroundColour(DESIGN_GRAY900_COLOR);
    second_title->SetFont(::Label::Body_14);
    second_title->SetToolTip(tip);
    second_title->Wrap(-1);

    m_sizer->Add(input       , 0, wxALIGN_CENTER_VERTICAL);
    m_sizer->Add(second_title, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(2));

    // onchange defaults to {}, so it must be tested before being called -- an empty std::function
    // throws std::bad_function_call. Every pre-existing caller happens to pass a real lambda,
    // which is the only reason this never fired.
    input->GetTextCtrl()->Bind(wxEVT_TEXT_ENTER, [this, param, input, onchange](wxCommandEvent &e) {
        auto value = input->GetTextCtrl()->GetValue();
        app_config->set(param, std::string(value.mb_str()));
        app_config->save();
        if (onchange)
            onchange(value);
        e.Skip();
    });

    input->GetTextCtrl()->Bind(wxEVT_KILL_FOCUS, [this, param, input, onchange](wxFocusEvent &e) {
        auto value = input->GetTextCtrl()->GetValue();
        app_config->set(param, std::string(value.mb_str()));
        if (onchange)
            onchange(value);
        e.Skip();
    });

    return m_sizer;
}

// A URL field. create_item_input cannot serve here even with wxFILTER_NONE, because a URL needs
// validating rather than filtering: we accept https:// anywhere, and http:// only for loopback,
// matching WebAppPanel::is_secure_context. Modelled on create_camera_orbit_mult_input, which is
// the existing precedent for "same widget, different validation". on_commit refreshes the tab
// that owns `param`; empty means the generic web application tab.
wxBoxSizer *PreferencesDialog::create_item_url_input(wxString title, wxString tooltip, std::string param, std::function<void()> on_commit)
{
    wxBoxSizer *sizer_input = new wxBoxSizer(wxHORIZONTAL);
    auto        input_title = new wxStaticText(m_parent, wxID_ANY, title, wxDefaultPosition, DESIGN_TITLE_SIZE, wxST_NO_AUTORESIZE);
    input_title->SetForegroundColour(DESIGN_GRAY900_COLOR);
    input_title->SetFont(::Label::Body_14);
    input_title->SetToolTip(tooltip);
    input_title->Wrap(DESIGN_TITLE_SIZE.x);

    auto input = new ::TextInput(m_parent, wxEmptyString, wxEmptyString, wxEmptyString, wxDefaultPosition,
                                 wxSize(FromDIP(260), DESIGN_INPUT_SIZE.y), wxTE_PROCESS_ENTER);
    StateColor input_bg(std::pair<wxColour, int>(wxColour("#F0F0F1"), StateColor::Disabled),
                        std::pair<wxColour, int>(*wxWHITE, StateColor::Enabled));
    input->SetBackgroundColor(input_bg);
    input->GetTextCtrl()->SetValue(app_config->get(param));
    input->SetToolTip(tooltip);

    sizer_input->AddSpacer(FromDIP(DESIGN_LEFT_MARGIN));
    sizer_input->Add(input_title, 0, wxALIGN_CENTER_VERTICAL);
    sizer_input->Add(input, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(5));

    auto commit = [this, param, input, on_commit]() {
        wxString value = input->GetTextCtrl()->GetValue();
        value.Trim(true).Trim(false);

        if (!value.empty() && !WebAppPanel::is_secure_context(value)) {
            // Put the stored value back rather than persisting something that would only produce
            // the "not secure" fallback page.
            input->GetTextCtrl()->SetValue(app_config->get(param));
            input->SetToolTip(_L("Only https:// addresses are accepted, apart from loopback addresses such as "
                                 "http://127.0.0.1 and http://localhost."));
            return;
        }

        app_config->set(param, std::string(value.ToUTF8().data()));
        app_config->save();
        if (on_commit)
            on_commit();
        else if (wxGetApp().mainframe != nullptr)
            wxGetApp().mainframe->refresh_web_app();
    };

    input->GetTextCtrl()->Bind(wxEVT_TEXT_ENTER, [commit](wxCommandEvent &e) { commit(); e.Skip(); });
    input->GetTextCtrl()->Bind(wxEVT_KILL_FOCUS, [commit](wxFocusEvent &e) { commit(); e.Skip(); });

    return sizer_input;
}

wxBoxSizer *PreferencesDialog::create_item_spinctrl(wxString title, wxString title2, wxString side_label, wxString tooltip, std::string param, int min, int max, std::function<void(int)> onchange, const wxString wiki_url)
{
    auto tip = tooltip.IsEmpty() ? title : tooltip; // auto fill tooltips with title if its empty

    wxBoxSizer *m_sizer = create_item_label(title, tip, wiki_url);

    auto input = new SpinInput(m_parent, wxEmptyString, side_label, wxDefaultPosition, DESIGN_INPUT_SIZE, wxSP_ARROW_KEYS, min, max, stoi(app_config->get(param)));
    input->SetToolTip(tip);

    // ORCA: this one is only meaningful while the dimming it controls is enabled
    if (param == "preview_dim_previous_layers_brightness") {
        m_dim_previous_layers_brightness_input = input;
        input->Enable(app_config->get_bool("preview_dim_previous_layers"));
    }

    m_sizer->Add(input, 0, wxALIGN_CENTER_VERTICAL);

    if(!title2.empty()){
        auto second_title = new wxStaticText(m_parent, wxID_ANY, title2, wxDefaultPosition, wxDefaultSize, 0);
        second_title->SetForegroundColour(DESIGN_GRAY900_COLOR);
        second_title->SetFont(::Label::Body_14);
        second_title->SetToolTip(tip);
        m_sizer->Add(second_title, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(5));
    }

    input->Bind(wxEVT_TEXT_ENTER, [this, param, input, onchange](wxCommandEvent& e) {
        auto value = input->GetValue();
        app_config->set(param, std::to_string(value));
        app_config->save();
        if (onchange != nullptr) onchange(value);
        e.Skip();
    });

    input->Bind(wxEVT_SPINCTRL, [this, param, input, onchange](wxCommandEvent& e) {
        auto value = input->GetValue();
        app_config->set(param, std::to_string(value));
        app_config->save();
        if (onchange != nullptr) onchange(value);
        e.Skip();
    });

    input->Bind(wxEVT_KILL_FOCUS, [this, param, input, onchange](wxFocusEvent &e) {
        auto value = input->GetValue();
        app_config->set(param, std::to_string(value));
        if (onchange != nullptr) onchange(value);
        e.Skip();
    });

    return m_sizer;
}

wxBoxSizer *PreferencesDialog::create_camera_orbit_mult_input(wxString title, wxString tooltip)
{
    auto tip = tooltip.IsEmpty() ? title : tooltip; // auto fill tooltips with title if its empty

    wxBoxSizer *m_sizer = create_item_label(title, tip);

    auto param = "camera_orbit_mult";

    auto       input = new ::TextInput(m_parent, wxEmptyString, wxEmptyString, wxEmptyString, wxDefaultPosition, DESIGN_INPUT_SIZE, wxTE_PROCESS_ENTER);
    StateColor input_bg(std::pair<wxColour, int>(wxColour("#F0F0F1"), StateColor::Disabled), std::pair<wxColour, int>(*wxWHITE, StateColor::Enabled));
    input->SetBackgroundColor(input_bg);
    input->GetTextCtrl()->SetValue(app_config->get(param));
    wxTextValidator validator(wxFILTER_NUMERIC);
    input->SetToolTip(tip);
    input->GetTextCtrl()->SetValidator(validator);

    m_sizer->Add(input, 0, wxALIGN_CENTER_VERTICAL);

    const double min = 0.05;
    const double max = 2.0;

    input->GetTextCtrl()->Bind(wxEVT_TEXT_ENTER, [this, param, input, min, max](wxCommandEvent &e) {
        auto value = input->GetTextCtrl()->GetValue();
        double conv = 1.0;
        if (value.ToCDouble(&conv)) {
            conv = conv < min ? min : conv > max ? max : conv;
            auto strval = std::string(wxString::FromCDouble(conv, 2).mb_str());
            input->GetTextCtrl()->SetValue(strval);
            app_config->set(param, strval);
            app_config->save();
        }
        e.Skip();
    });

    input->GetTextCtrl()->Bind(wxEVT_KILL_FOCUS, [this, param, input, min, max](wxFocusEvent &e) {
        auto value = input->GetTextCtrl()->GetValue();
        double conv = 1.0;
        if (value.ToCDouble(&conv)) {
            conv = conv < min ? min : conv > max ? max : conv;
            auto strval = std::string(wxString::FromCDouble(conv, 2).mb_str());
            input->GetTextCtrl()->SetValue(strval);
            app_config->set(param, strval);
        }
        e.Skip();
    });

    return m_sizer;
}

wxBoxSizer *PreferencesDialog::create_item_decimal_input(wxString title, wxString title2, wxString tooltip, std::string param, double min, double max, int decimals, const wxString wiki_url)
{
    auto tip = tooltip.IsEmpty() ? title : tooltip; // auto fill tooltips with title if its empty

    wxBoxSizer *m_sizer = create_item_label(title, tip, wiki_url);

    auto       input = new ::TextInput(m_parent, wxEmptyString, title2, wxEmptyString, wxDefaultPosition, DESIGN_INPUT_SIZE, wxTE_PROCESS_ENTER);
    StateColor input_bg(std::pair<wxColour, int>(wxColour("#F0F0F1"), StateColor::Disabled), std::pair<wxColour, int>(*wxWHITE, StateColor::Enabled));
    input->SetBackgroundColor(input_bg);
    input->GetTextCtrl()->SetValue(app_config->get(param));
    wxTextValidator validator(wxFILTER_NUMERIC);
    input->SetToolTip(tooltip);
    input->GetTextCtrl()->SetValidator(validator);

    m_sizer->Add(input, 0, wxALIGN_CENTER_VERTICAL);

    auto apply_value = [this, param, input, min, max, decimals]() {
        auto value = input->GetTextCtrl()->GetValue();
        double conv = min;
        if (value.ToCDouble(&conv)) {
            conv = conv < min ? min : conv > max ? max : conv;
            auto strval = std::string(wxString::FromCDouble(conv, decimals).mb_str());
            input->GetTextCtrl()->SetValue(strval);
            app_config->set(param, strval);
        }
    };

    input->GetTextCtrl()->Bind(wxEVT_TEXT_ENTER, [apply_value](wxCommandEvent &e) {
        apply_value();
        wxGetApp().app_config->save();
        e.Skip();
    });

    input->GetTextCtrl()->Bind(wxEVT_KILL_FOCUS, [apply_value](wxFocusEvent &e) {
        apply_value();
        e.Skip();
    });

    return m_sizer;
}

wxBoxSizer *PreferencesDialog::create_item_backup(wxString title, wxString tooltip)
{
    auto tip = tooltip.IsEmpty() ? title : tooltip; // auto fill tooltips with title if its empty

    wxBoxSizer *m_sizer = create_item_label(title, tip);

    auto checkbox = new ::CheckBox(m_parent);
    checkbox->SetValue(app_config->get_bool("backup_switch"));
    checkbox->SetToolTip(tip);

    checkbox->Bind(wxEVT_TOGGLEBUTTON, [this, checkbox](wxCommandEvent &e) {
        app_config->set_bool("backup_switch", checkbox->GetValue());
        app_config->save();
        bool pbool = app_config->get("backup_switch") == "true" ? true : false;
        std::string backup_interval = "10";
        app_config->get("backup_interval", backup_interval);
        Slic3r::set_backup_interval(pbool ? boost::lexical_cast<long>(backup_interval) : 0);
        if (m_backup_interval_textinput != nullptr) { m_backup_interval_textinput->Enable(pbool); }
        e.Skip();
    });

    m_backup_interval_time = app_config->get("backup_interval");

    auto input = new ::TextInput(m_parent, wxEmptyString, _L("sec"), "loop", wxDefaultPosition, wxSize(FromDIP(97), -1), wxTE_PROCESS_ENTER);
    StateColor input_bg(std::pair<wxColour, int>(wxColour("#F0F0F1"), StateColor::Disabled), std::pair<wxColour, int>(*wxWHITE, StateColor::Enabled));
    input->SetBackgroundColor(input_bg);
    input->GetTextCtrl()->SetValue(m_backup_interval_time);
    wxTextValidator validator(wxFILTER_DIGITS);
    input->SetToolTip(_L("The period of backup in seconds."));
    input->GetTextCtrl()->SetValidator(validator);

    m_sizer->Add(checkbox, 0, wxALIGN_CENTER);
    m_sizer->Add(input   , 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(5));

    input->GetTextCtrl()->Bind(wxEVT_COMMAND_TEXT_UPDATED, [this, input](wxCommandEvent &e) {
        m_backup_interval_time = input->GetTextCtrl()->GetValue();
        e.Skip();
    });

    std::function<void()> backup_interval = [this, input]() {
        m_backup_interval_time = input->GetTextCtrl()->GetValue();
        app_config->set("backup_interval", std::string(m_backup_interval_time.mb_str()));
        app_config->save();
        long backup_interval = 0;
        m_backup_interval_time.ToLong(&backup_interval);
        Slic3r::set_backup_interval(backup_interval);
    };

    input->GetTextCtrl()->Bind(wxEVT_TEXT_ENTER, [backup_interval](wxCommandEvent &e) {
        backup_interval();
        e.Skip();
    });

     input->GetTextCtrl()->Bind(wxEVT_KILL_FOCUS, [backup_interval](wxFocusEvent &e) {
        backup_interval();
        e.Skip();
    });

    input->Enable(app_config->get("backup_switch") == "true");
    input->Refresh();

    m_backup_interval_textinput = input;
    return m_sizer;
}

wxBoxSizer *PreferencesDialog::create_item_auto_reslice(wxString title, wxString checkbox_tooltip, wxString delay_tooltip)
{
    wxBoxSizer *m_sizer = create_item_label(title, checkbox_tooltip);

    auto checkbox = new ::CheckBox(m_parent);
    checkbox->SetValue(app_config->get_bool("auto_slice_after_change"));
    checkbox->SetToolTip(checkbox_tooltip);

    wxString delay_value = app_config->get("auto_slice_change_delay_seconds");
    if (delay_value.empty())
        delay_value = "0";

    auto input = new ::TextInput(m_parent, wxEmptyString, _L("sec"), wxEmptyString, wxDefaultPosition, wxSize(FromDIP(97), -1), wxTE_PROCESS_ENTER);
    StateColor input_bg(std::pair<wxColour, int>(wxColour("#F0F0F1"), StateColor::Disabled), std::pair<wxColour, int>(*wxWHITE, StateColor::Enabled));
    input->SetBackgroundColor(input_bg);
    input->GetTextCtrl()->SetValue(delay_value);
    wxTextValidator validator(wxFILTER_DIGITS);
    input->SetToolTip(delay_tooltip);
    input->GetTextCtrl()->SetValidator(validator);

    m_sizer->Add(checkbox, 0, wxALIGN_CENTER);
    m_sizer->Add(input   , 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(5));

    auto commit_delay = [this, input]() {
        wxString value = input->GetTextCtrl()->GetValue();
        long seconds = 0;
        if (!value.ToLong(&seconds) || seconds < 0)
            seconds = 0;
        wxString sanitized = wxString::Format("%ld", seconds);
        input->GetTextCtrl()->SetValue(sanitized);
        app_config->set("auto_slice_change_delay_seconds", std::string(sanitized.mb_str()));
        app_config->save();
    };

    input->GetTextCtrl()->Bind(wxEVT_TEXT_ENTER, [commit_delay](wxCommandEvent &e) {
        commit_delay();
        e.Skip();
    });

    input->GetTextCtrl()->Bind(wxEVT_KILL_FOCUS, [commit_delay](wxFocusEvent &e) {
        commit_delay();
        e.Skip();
    });

    checkbox->Bind(wxEVT_TOGGLEBUTTON, [this, checkbox, input](wxCommandEvent &e) {
        const bool enabled = checkbox->GetValue();
        app_config->set_bool("auto_slice_after_change", enabled);
        app_config->save();
        input->Enable(enabled);
        input->Refresh();
        e.Skip();
    });

    input->Enable(checkbox->GetValue());
    input->Refresh();

    return m_sizer;
}

// Unlayered 0.1.6: the "Send usage diagnostics" switch. Switching off is a decline, saved and
// applied at once (queued diagnostics are discarded). Switching on is an explicit choice for the
// region and country already chosen; without one, the privacy steps ask for them first.
void PreferencesDialog::on_usage_diagnostics_toggled(bool on)
{
    namespace pc = PrivacyConsent;
    const std::optional<pc::Record> current = pc::load(*app_config);
    const std::string region  = current ? current->region : std::string();
    const std::string country = current ? current->country : std::string();
    const bool        placed  = pc::is_region_code(region);
    if (on && !placed) {
        run_privacy_setup(this, "preferences");
    } else {
        const pc::Record record = pc::make_choice(region, country, on, false, "preferences", pc::iso8601_now(), pc::legal_approvals());
        pc::save(*app_config, record);
        app_config->save();
        UsageTelemetry::refresh(*app_config);

        PrivacyChoiceStats::Event e;
        e.event_type       = "privacy_preference_changed";
        e.region           = region;
        e.country          = country;
        e.page_version     = "preferences-v1";
        e.notice_version   = pc::NOTICE_VERSION;
        e.choice           = on ? "accepted" : "declined";
        e.choice_source    = "preferences";
        e.selection_method = "explicit";
        if (std::optional<nlohmann::json> body = PrivacyChoiceStats::to_body(e))
            PrivacyChoiceStats::submit(*body);
    }
    // The generic checkbox handler wrote the box's state into the mirror key before calling this;
    // put back what the record actually says (a privacy dialog closed without a choice stays off).
    app_config->set_bool("usage_diagnostics", pc::diagnostics_permitted(*app_config));
    app_config->save();
    update_privacy_controls();
}

// Unlayered 0.1.6 (notice 2026-10-v3): the "Slicing improvement program" box. Only usable while
// diagnostics are shared; joining or leaving is an explicit choice for the place already chosen, saved
// and applied at once (what was queued under the other answer is discarded, UsageTelemetry::refresh).
void PreferencesDialog::on_improvement_program_toggled(bool on)
{
    namespace pc = PrivacyConsent;
    const std::optional<pc::Record> current = pc::load(*app_config);
    if (current && pc::diagnostics_permitted(*app_config)) {
        const pc::Record record = pc::make_choice(current->region, current->country, true, false, "preferences", pc::iso8601_now(),
                                                  pc::legal_approvals(), on);
        pc::save(*app_config, record);
        app_config->save();
        UsageTelemetry::refresh(*app_config);
    }
    app_config->set_bool("usage_improvement_program", pc::program_permitted(*app_config));
    app_config->save();
    update_privacy_controls();
}

void PreferencesDialog::update_privacy_controls()
{
    namespace pc = PrivacyConsent;
    const std::optional<pc::Record> record = pc::load(*app_config);
    if (m_usage_diagnostics_checkbox)
        m_usage_diagnostics_checkbox->SetValue(pc::diagnostics_permitted(*app_config));
    if (m_improvement_program_checkbox) {
        m_improvement_program_checkbox->SetValue(pc::program_permitted(*app_config));
        m_improvement_program_checkbox->Enable(pc::diagnostics_permitted(*app_config));
    }
    if (!m_privacy_status)
        return;

    wxString text;
    if (pc::organization_opt_out())
        text = _L("Switched off on this computer by your organization.");
    else
        switch (pc::status(record, pc::legal_approvals())) {
        case pc::Status::Granted:
            text = pc::program_permitted(*app_config) ? _L("Your choice: share usage diagnostics, as a member of the Slicing improvement program.") :
                                                        _L("Your choice: share usage diagnostics.");
            break;
        case pc::Status::Declined: text = _L("Your choice: keep my data private."); break;
        case pc::Status::NeedsNewChoice: text = _L("Not shared. The privacy notice has changed since your last choice; please choose again."); break;
        case pc::Status::NotAsked: text = _L("Not shared. You have not made a choice yet."); break;
        }
    if (record)
        if (const pc::Place *where = pc::place_of(record->region, record->country))
            text += " " + wxString::Format(_L("Where you live: %s."), wxString::FromUTF8(where->label));
    m_privacy_status->SetLabel(text);
    m_privacy_status->Wrap(FromDIP(560));
    m_parent->Layout();
}

void PreferencesDialog::set_dark_mode()
{
#ifdef __WINDOWS__
#ifdef _MSW_DARK_MODE
    NppDarkMode::SetDarkExplorerTheme(this->GetHWND());
    NppDarkMode::SetDarkTitleBar(this->GetHWND());
    wxGetApp().UpdateDlgDarkUI(this);
    SetActiveWindow(wxGetApp().mainframe->GetHWND());
    SetActiveWindow(GetHWND());
#endif
#endif
}

wxBoxSizer *PreferencesDialog::create_item_checkbox(wxString title, wxString tooltip, std::string param, const wxString secondary_title, const wxString wiki_url)
{
    auto tip = tooltip.IsEmpty() ? title : tooltip; // auto fill tooltips with title if its empty

    wxBoxSizer *m_sizer = create_item_label(title, tip, wiki_url);

    auto checkbox = new ::CheckBox(m_parent);
    checkbox->SetValue(app_config->get_bool(param));
    checkbox->SetToolTip(tip);

    if (param == "sync_user_preset") { m_sync_user_preset_checkbox = checkbox; }
    if (param == "usage_diagnostics") {
        // The privacy choice decides, not the mirror key.
        m_usage_diagnostics_checkbox = checkbox;
        checkbox->SetValue(PrivacyConsent::diagnostics_permitted(*app_config));
        checkbox->Enable(!PrivacyConsent::organization_opt_out());
    }
    if (param == "usage_improvement_program") {
        // Membership follows the privacy record too; only offered while diagnostics are shared.
        m_improvement_program_checkbox = checkbox;
        checkbox->SetValue(PrivacyConsent::program_permitted(*app_config));
        checkbox->Enable(PrivacyConsent::diagnostics_permitted(*app_config));
    }

    m_sizer->Add(checkbox, 0, wxALIGN_CENTER);

    if(!secondary_title.IsEmpty()){
        auto sec_title = new wxStaticText(m_parent, wxID_ANY, secondary_title);
        sec_title->SetForegroundColour(DESIGN_GRAY900_COLOR);
        sec_title->SetFont(::Label::Body_14);
        sec_title->Wrap(-1);
        sec_title->SetToolTip(tip);
        m_sizer->Add(sec_title, 0, wxALIGN_CENTER | wxLEFT, FromDIP(5));
    }

     //// save config
    checkbox->Bind(wxEVT_TOGGLEBUTTON, [this, checkbox, param](wxCommandEvent &e) {
        app_config->set_bool(param, checkbox->GetValue());
        app_config->save();

        // if (param == "staff_pick_switch") {
        //     bool pbool = app_config->get("staff_pick_switch") == "true";
        //     wxGetApp().switch_staff_pick(pbool);
        // }

        if (param == "usage_diagnostics") {
            on_usage_diagnostics_toggled(checkbox->GetValue());
        }
        else if (param == "usage_improvement_program") {
            on_improvement_program_toggled(checkbox->GetValue());
        }
        else if (param == "sync_user_preset") {
            bool sync = app_config->get("sync_user_preset") == "true" ? true : false;
            if (sync) {
                wxGetApp().start_sync_user_preset();
            } else {
                wxGetApp().stop_sync_user_preset();
            }
            BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << " sync_user_preset: " << (sync ? "true" : "false");
        }
        else if (param == "stealth_mode") {
            bool enabled = app_config->get_stealth_mode();
            if (enabled) wxGetApp().on_stealth_mode_enter();
            if (m_sync_user_preset_checkbox) m_sync_user_preset_checkbox->Enable(!enabled);
            if (m_bambu_cloud_checkbox)      m_bambu_cloud_checkbox->Enable(!enabled);
            // Stealth mode now also gates the home page's account panel, so push the change
            // through immediately -- otherwise the panel would not follow until a restart.
            if (wxGetApp().mainframe && wxGetApp().mainframe->m_webview)
                wxGetApp().mainframe->m_webview->SendCloudProvidersInfo();
        }
        else if (param == "hide_login_side_panel") {
            if (wxGetApp().mainframe && wxGetApp().mainframe->m_webview) {
                wxGetApp().mainframe->m_webview->SendCloudProvidersInfo();
            }
        }
        else if (param == "show_cut_chunk_colors") {
            if (Plater* plater = wxGetApp().plater()) {
                if (GLCanvas3D* canvas = plater->get_view3D_canvas3D()) {
                    canvas->set_as_dirty();
                    canvas->request_extra_frame();
                }
            }
        }
        else if (param == "opaque_overlays") {
            wxGetApp().imgui()->set_opaque_overlays(app_config->get_bool(param));
            if (Plater* plater = wxGetApp().plater()) {
                if (GLCanvas3D* canvas = plater->get_current_canvas3D()) {
                    canvas->set_as_dirty();
                    canvas->request_extra_frame();
                }
            }
        }
        // ORCA: apply the preview dimming change immediately to the currently loaded preview
        else if (param == "preview_dim_previous_layers") {
            if (m_dim_previous_layers_brightness_input)
                m_dim_previous_layers_brightness_input->Enable(app_config->get_bool(param));
            if (Plater* plater = wxGetApp().plater()) {
                if (GLCanvas3D* canvas = plater->get_preview_canvas3D()) {
                    canvas->get_gcode_viewer().set_dim_previous_layers(app_config->get_bool(param));
                    canvas->set_as_dirty();
                    canvas->request_extra_frame();
                }
            }
        }

#ifdef __WXMSW__
        if (param == "associate_3mf") {
             bool pbool = app_config->get("associate_3mf") == "true" ? true : false;
             if (pbool) {
                 wxGetApp().associate_files(L"3mf");
             } else {
                 wxGetApp().disassociate_files(L"3mf");
             }
        }

        if (param == "associate_drc") {
            bool pbool = app_config->get("associate_drc") == "true" ? true : false;
            if (pbool) {
                wxGetApp().associate_files(L"drc");
            } else {
                wxGetApp().disassociate_files(L"drc");
            }
        }

        if (param == "associate_stl") {
            bool pbool = app_config->get("associate_stl") == "true" ? true : false;
            if (pbool) {
                wxGetApp().associate_files(L"stl");
            } else {
                wxGetApp().disassociate_files(L"stl");
            }
        }

        if (param == "associate_step") {
            bool pbool = app_config->get("associate_step") == "true" ? true : false;
            if (pbool) {
                wxGetApp().associate_files(L"step");
            } else {
                wxGetApp().disassociate_files(L"step");
            }
        }

        if (param == "installed_networking") {
            bool pbool = app_config->get_bool("installed_networking");
            if (pbool) {
                GUI::wxGetApp().CallAfter([] { GUI::wxGetApp().ShowDownNetPluginDlg(); });
            }
        }

#endif // __WXMSW__

        if (param == "developer_mode") {
            m_developer_mode_def = app_config->get("developer_mode");
            Slic3r::GUI::wxGetApp().update_mode();
        }

        // webview  dump_vedio
        if (param == "internal_developer_mode") {
            m_internal_developer_mode_def = app_config->get("internal_developer_mode");
            if (m_internal_developer_mode_def == "true") {
                Slic3r::GUI::wxGetApp().update_internal_development();
                Slic3r::GUI::wxGetApp().mainframe->show_log_window();
            } else {
                Slic3r::GUI::wxGetApp().update_internal_development();
            }
        }

        if (param == "show_unsupported_presets") {
            wxGetApp().plater()->sidebar().update_presets(Preset::TYPE_FILAMENT);
        }

        if (param == "use_printer_agents")
        {
            // Rebuild the Device tab so the native/web-UI choice reflects the new flag
            // immediately, instead of only on the next printer-preset change or restart.
            if (wxGetApp().plater())
                wxGetApp().plater()->sidebar().update_all_preset_comboboxes();
        }

        // Unlayered: the settings gate is consulted on every bridge call, so nothing needs
        // re-wiring -- the page only has to be told that its capability list changed. Each key
        // belongs to one slot; the other tab is unaffected.
        if (param == SETTING_WEB_APP_ALLOW_SETTINGS || param == SETTING_SIDE_PANEL_ALLOW_SETTINGS ||
            param == SETTING_UNIVERSE_APP_ALLOW_SETTINGS || param == "side_panel_allow_printer") {
            MainFrame*   mainframe = wxGetApp().mainframe;
            WebAppPanel* panel     = nullptr;
            if (mainframe != nullptr) {
                if (param == SETTING_SIDE_PANEL_ALLOW_SETTINGS || param == "side_panel_allow_printer")
                    panel = mainframe->m_side_panel_view;
                else if (param == SETTING_UNIVERSE_APP_ALLOW_SETTINGS)
                    panel = mainframe->m_universe_view;
                else
                    panel = mainframe->m_web_app_view;
            }
            if (panel != nullptr && panel->bridge() != nullptr)
                panel->bridge()->notify_capabilities();
        }

        if (param == "enable_high_low_temp_mixed_printing") {
            if (checkbox->GetValue()) {
                const wxString warning_title = _L("Bed Temperature Difference Warning");
                const wxString warning_message =
                    _L("Using filaments with significantly different temperatures may cause:\n"
                        "• Extruder clogging\n"
                        "• Nozzle damage\n"
                        "• Layer adhesion issues\n\n"
                        "Continue with enabling this feature?");
                std::function<void(const wxString&)> link_callback = [](const wxString&) {
                            const std::string lang_code = wxGetApp().app_config->get("language");
                            const wxString region = (lang_code.find("zh") != std::string::npos) ? L"zh" : L"en";
                            const wxString wiki_url = wxString::Format(
                                L"https://wiki.bambulab.com/%s/filament-acc/filament/h2d-filament-config-limit",
                                region
                            );
                            wxGetApp().open_browser_with_warning_dialog(wiki_url);
                            };

                MessageDialog msg_dialog(
                    nullptr,
                    warning_message,
                    warning_title,
                    wxICON_WARNING | wxYES_NO | wxCANCEL | wxYES_DEFAULT | wxCENTRE,
                    wxEmptyString,
                    _L("Click Wiki for help."),
                    link_callback
                );

                if (msg_dialog.ShowModal() != wxID_YES) {
                    checkbox->SetValue(false);
                    app_config->set_bool(param, false);
                    app_config->save();
                }
            }
        }

        e.Skip();
    });

    //// for debug mode
    if (param == "developer_mode") { m_developer_mode_ckeckbox = checkbox; }
    if (param == "internal_developer_mode") { m_internal_developer_mode_ckeckbox = checkbox; }

    return m_sizer;
}

// A colour swatch that opens the native colour dialog, plus a Reset that restores the theme default.
wxBoxSizer *PreferencesDialog::create_item_colour(wxString title, wxString tooltip, std::function<wxColour()> get, std::function<void(const wxColour &)> set, std::function<void()> reset)
{
    wxBoxSizer *sizer  = create_item_label(title, tooltip);
    auto        picker = new wxColourPickerCtrl(m_parent, wxID_ANY, get());
    picker->SetToolTip(tooltip);
    picker->Bind(wxEVT_COLOURPICKER_CHANGED, [this, set](wxColourPickerEvent &e) { set(e.GetColour()); refresh_theme_items(); }); // status, and swatches auto-contrast moved
    sizer->Add(picker, 0, wxALIGN_CENTER_VERTICAL);
    m_theme_refresh.push_back([picker, get]() { picker->SetColour(get()); });

    auto reset_btn = new Button(m_parent, _L("Reset"));
    reset_btn->SetStyle(ButtonStyle::Regular, ButtonType::Parameter);
    reset_btn->Bind(wxEVT_BUTTON, [this, reset](auto &) {
        reset();
        refresh_theme_items();
    });
    sizer->Add(reset_btn, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(8));
    return sizer;
}

wxBoxSizer* PreferencesDialog::create_item_button(wxString title, wxString title2, wxString tooltip, wxString tooltip2, std::function<void()> onclick, const wxString wiki_url)
{
    auto tip = tooltip.IsEmpty() ? tooltip2 : tooltip; // use button tooltip if label tooltip empty

    wxBoxSizer *m_sizer = create_item_label(title, tip, wiki_url);

    auto m_button_download = new Button(m_parent, title2);
    m_button_download->SetStyle(title2 == _L("Clear") ? ButtonStyle::Alert : ButtonStyle::Regular, ButtonType::Parameter);
    m_button_download->SetToolTip(tooltip2.IsEmpty() ? tooltip : tooltip2); // use label tooltip if button tooltip empty

    m_button_download->Bind(wxEVT_BUTTON, [onclick](auto &e) { onclick(); });

    m_sizer->Add(m_button_download, 0, wxALIGN_CENTER_VERTICAL);

    return m_sizer;
}

wxBoxSizer* PreferencesDialog::create_item_downloads(wxString title, wxString tooltip)
{
    wxString download_path = wxString::FromUTF8(app_config->get("download_path"));

    wxBoxSizer *m_sizer = create_item_label(title, tooltip);

    auto m_staticTextPath = new wxStaticText(m_parent, wxID_ANY, download_path, wxDefaultPosition, wxSize(FromDIP(120),-1), wxST_ELLIPSIZE_END);
    m_staticTextPath->SetForegroundColour(DESIGN_GRAY600_COLOR);
    m_staticTextPath->SetFont(::Label::Body_14);
    m_staticTextPath->Wrap(-1);
    m_staticTextPath->SetToolTip(download_path);

    auto m_button_download = new Button(m_parent, _L("Browse") + dots);
    m_button_download->SetStyle(ButtonStyle::Regular, ButtonType::Parameter);
    m_button_download->SetToolTip(_L("Choose folder for downloaded items"));

    m_button_download->Bind(wxEVT_BUTTON, [this, m_staticTextPath, m_sizer](auto& e) {
        wxString defaultPath = wxT("/");
        wxDirDialog dialog(this, _L("Choose Download Directory"), defaultPath, wxDD_NEW_DIR_BUTTON);

        if (dialog.ShowModal() == wxID_OK) {
            wxString download_path = dialog.GetPath();
            std::string download_path_str = download_path.ToUTF8().data();
            app_config->set("download_path", download_path_str);
            m_staticTextPath->SetLabelText(download_path);
            m_staticTextPath->SetToolTip(download_path);
            m_sizer->Layout();
        }
    });

    m_sizer->Add(m_button_download, 0, wxALIGN_CENTER_VERTICAL);
    m_sizer->Add(m_staticTextPath , 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(10));

    return m_sizer;
}

wxBoxSizer *PreferencesDialog::create_item_bambu_cloud(wxString title, wxString tooltip)
{
    wxBoxSizer *m_sizer = create_item_label(title, tooltip);

    auto cb = new ::CheckBox(m_parent);
    m_bambu_cloud_checkbox = cb;
    cb->SetValue(app_config->has_cloud_provider(BBL_CLOUD_PROVIDER));
    cb->SetToolTip(tooltip);

    cb->Bind(wxEVT_TOGGLEBUTTON, [this, cb](wxCommandEvent &e) {
        e.Skip(); // let CheckBox::update() refresh the bitmap
        if (cb->GetValue()) {
            app_config->add_cloud_provider(BBL_CLOUD_PROVIDER);
        } else {
            app_config->remove_cloud_provider(BBL_CLOUD_PROVIDER);
        }
        app_config->save();

        // Update homepage visibility immediately
        auto *mainframe = wxGetApp().mainframe;
        if (mainframe && mainframe->m_webview)
            mainframe->m_webview->SendCloudProvidersInfo();
    });

    m_sizer->Add(cb, 0, wxALIGN_CENTER);

    return m_sizer;
};

wxBoxSizer *PreferencesDialog::create_item_network_plugin_version(wxString title, wxString tooltip)
{
    wxBoxSizer *m_sizer = create_item_label(title, tooltip);

    m_network_version_combo = new ::ComboBox(m_parent, wxID_ANY, wxEmptyString, wxDefaultPosition, DESIGN_LARGE_COMBOBOX_SIZE, 0, nullptr, wxCB_READONLY);
    m_network_version_combo->GetDropDown().SetUseContentWidth(true);
    m_network_version_combo->SetToolTip(tooltip);

    std::string current_version = app_config->get_network_plugin_version();
    if (current_version.empty()) {
        current_version = get_latest_network_version();
    }
    int current_selection = 0;

    m_available_versions = get_all_available_versions();

    for (size_t i = 0; i < m_available_versions.size(); i++) {
        const auto& ver = m_available_versions[i];
        m_network_version_combo->Append(network_version_label(ver));
        if (current_version == ver.version) {
            current_selection = i;
        }
    }

    m_network_version_combo->SetSelection(current_selection);
    m_sizer->Add(m_network_version_combo, 0, wxALIGN_CENTER);

    m_network_version_combo->GetDropDown().Bind(wxEVT_COMBOBOX, [this](wxCommandEvent& e) {
        e.Skip(); // order-independent flag read after this handler returns; every path just returns
        int selection = e.GetSelection();
        if (selection < 0 || selection >= (int) m_available_versions.size())
            return;

        const auto& selected_ver = m_available_versions[selection];
        const std::string new_version = selected_ver.version;
        std::string old_version = app_config->get_network_plugin_version();
        if (old_version.empty())
            old_version = get_latest_network_version();

        // Move the combo back to the row for `version`, so the UI never shows a build other
        // than the one actually configured/loaded (e.g. after a declined or refused switch).
        auto reselect = [this](const std::string& version) {
            for (size_t i = 0; i < m_available_versions.size(); ++i)
                if (m_available_versions[i].version == version) {
                    m_network_version_combo->SetSelection((int) i);
                    break;
                }
        };

        if (new_version == old_version)
            return;

        BOOST_LOG_TRIVIAL(info) << "Network plugin version selection changed from " << old_version << " to " << new_version;

        if (!selected_ver.warning.empty()) {
            MessageDialog warn_dlg(this, wxString::FromUTF8(selected_ver.warning), _L("Warning"), wxOK | wxCANCEL | wxICON_WARNING);
            if (warn_dlg.ShowModal() != wxID_OK) {
                reselect(old_version);
                return;
            }
        }

        // A build already present on disk loads directly with a hot reload - on any platform
        // and across series (legacy <-> modern). Only claim success once the build that
        // actually loaded is the one that was requested.
        if (Slic3r::NetworkAgent::versioned_library_exists(new_version)) {
            app_config->set_network_plugin_version(new_version);
            app_config->save();
            BOOST_LOG_TRIVIAL(info) << "Version " << new_version << " already exists on disk, triggering hot reload";
            // Claim success only once the series that actually loaded is the one requested - the
            // loaded plug-in reports its full build (02.08.01.53) while the requested identity is
            // the series (02.08.01), so compare series, not the raw string.
            if (wxGetApp().hot_reload_network_plugin() &&
                network_plugin_series(Slic3r::NetworkAgent::get_version()) == network_plugin_series(new_version)) {
                MessageDialog dlg(this, _L("Network plug-in switched successfully."), _L("Success"), wxOK | wxICON_INFORMATION);
                dlg.ShowModal();
            } else {
                MessageDialog dlg(this, _L("Failed to load network plug-in. Please restart the application."), _L("Restart Required"), wxOK | wxICON_WARNING);
                dlg.ShowModal();
                reselect(app_config->get_network_plugin_version());
            }
            return;
        }

        // Not on disk: offer to download it. The endpoint is series-keyed and serves that series'
        // newest build. (A same-series custom build is only ever listed when its file is on disk,
        // so it takes the hot-reload branch above; the only not-on-disk selectable is a series or
        // legacy entry that genuinely needs fetching.)
        wxString msg = wxString::Format(
            _L("You've selected network plug-in version %s.\n\nWould you like to download and install this version now?\n\nNote: The application may need to restart after installation."),
            wxString::FromUTF8(new_version));
        MessageDialog dlg(this, msg, _L("Download Network Plug-in"), wxYES_NO | wxICON_QUESTION);
        if (dlg.ShowModal() == wxID_YES) {
            app_config->set_network_plugin_version(new_version);
            app_config->save();
            DownloadProgressDialog progress_dlg(_L("Downloading Network Plug-in"));
            progress_dlg.ShowModal();
            reselect(app_config->get_network_plugin_version());
        } else {
            reselect(old_version);
        }
    });

    auto reload_btn = new Button(m_parent, wxEmptyString, "refresh", 0, 16);
    reload_btn->SetStyle(ButtonStyle::Regular, ButtonType::Icon);
    reload_btn->SetToolTip(_L("Reload the network plug-in without restarting the application"));
    reload_btn->Bind(wxEVT_BUTTON, [this](auto& e) {
        if (wxGetApp().hot_reload_network_plugin()) {
            MessageDialog dlg(this, _L("Network plug-in reloaded successfully."), _L("Reload"), wxOK | wxICON_INFORMATION);
            dlg.ShowModal();
        } else {
            MessageDialog dlg(this, _L("Failed to reload network plug-in. Please restart the application."), _L("Reload Failed"), wxOK | wxICON_ERROR);
            dlg.ShowModal();
        }
    });
    m_sizer->Add(reload_btn, 0, wxALIGN_CENTER | wxLEFT, FromDIP(5));

    return m_sizer;
}

#ifdef WIN32
wxBoxSizer* PreferencesDialog::create_item_link_association( wxString url_prefix, wxString website_name)
{
    wxString title = _L("Associate") + (boost::format(" %1%://") % url_prefix.c_str()).str();
    wxString tooltip = _L("Associate") + " " + url_prefix + ":// " + _L("with Unlayered Slicer so that it can open models from") + " " + website_name;

    std::wstring registered_bin; // not used, just here to provide a ref to check fn
    bool reg_to_current_instance = wxGetApp().check_url_association(url_prefix.ToStdWstring(), registered_bin);

    auto* h_sizer = new wxBoxSizer(wxHORIZONTAL); // contains checkbox and other elements on the first line
    h_sizer->AddSpacer(FromDIP(DESIGN_LEFT_MARGIN));

    // build checkbox
    auto checkbox = new ::CheckBox(m_parent);
    checkbox->SetToolTip(tooltip);
    checkbox->SetValue(reg_to_current_instance); // If registered to the current instance, checkbox should be checked

    // build text next to checkbox
    auto checkbox_title = new wxStaticText(m_parent, wxID_ANY, title, wxDefaultPosition, DESIGN_TITLE_SIZE);
    checkbox_title->SetToolTip(tooltip);
    checkbox_title->SetForegroundColour(DESIGN_GRAY900_COLOR);
    checkbox_title->SetFont(::Label::Body_14);
    checkbox_title->Wrap(-1);

    h_sizer->Add(checkbox_title, 0, wxALIGN_CENTER | wxTOP | wxBOTTOM, FromDIP(3));
    h_sizer->Add(checkbox      , 0, wxALIGN_CENTER | wxLEFT          , FromDIP(5));

    auto* v_sizer = new wxBoxSizer(wxVERTICAL);
    v_sizer->Add(h_sizer);

    // build text below checkbox that indicates the instance currently registered to handle the link type
    auto* registered_instance_title = new wxStaticText(m_parent, wxID_ANY, "", wxDefaultPosition, wxDefaultSize, wxST_ELLIPSIZE_END);
    registered_instance_title->SetForegroundColour(DESIGN_GRAY600_COLOR);
    registered_instance_title->SetFont(::Label::Body_14);
    registered_instance_title->Wrap(-1);

    // update the text below checkbox
    auto update_current_association_str = [=, &reg_to_current_instance](){
        // get registered binary for given link type
        std::wstring registered_bin;
        reg_to_current_instance = wxGetApp().check_url_association(url_prefix.wc_str(), registered_bin);

        // format registered binary to get only the path and remove excess chars
        if (!registered_bin.empty())
            // skip idx 0 because it is the first quotation mark
            registered_bin = registered_bin.substr(1, registered_bin.find(L'\"', 1) - 1);

        wxString current_association_str = _L("Current Association: ");
        if (reg_to_current_instance) {
            current_association_str += _L("Current Instance");
            registered_instance_title->SetToolTip(_L("Current Instance Path: ") + registered_bin);
        } else if (registered_bin.empty())
            current_association_str += _L("None");
        else{
            current_association_str += registered_bin;
            registered_instance_title->SetToolTip(current_association_str);
        }

        registered_instance_title->SetLabel(current_association_str);
        registered_instance_title->SetMaxSize(wxSize(DESIGN_WINDOW_SIZE.x - FromDIP(DESIGN_LEFT_MARGIN) - FromDIP(40), -1)); // prevent horizontal scroll
    };
    update_current_association_str();

    v_sizer->Add(registered_instance_title, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(DESIGN_LEFT_MARGIN));

    checkbox->Bind(wxEVT_TOGGLEBUTTON, [=](wxCommandEvent& e) {
        if (checkbox->GetValue())
            wxGetApp().associate_url(url_prefix.ToStdWstring());
        else
            wxGetApp().disassociate_url(url_prefix.ToStdWstring());
        update_current_association_str();
        e.Skip();
    });

    return v_sizer;
}
#endif // WIN32

bool PreferencesDialog::recreate_GUI() const
{
    // Any net change of the theme in effect (an edit, another theme, a base flip, an import)
    // was previewed live by patching existing windows; the rebuilt window is constructed from
    // the theme and cannot drift. A round trip back to the same values needs no rebuild.
    return m_recreate_GUI || !ThemeManager::get().active().same_values(m_theme_at_open);
}

PreferencesDialog::PreferencesDialog(wxWindow *parent, wxWindowID id, const wxString &title, const wxPoint &pos, const wxSize &size, long style)
    : DPIDialog(parent, id, _L("Preferences"), pos, size, style)
{
    m_theme_at_open = ThemeManager::get().active();
#ifdef __WXGTK__
    // GNOME attaches a modal dialog to its parent (mutter's attach-modal-dialogs, on by default):
    // it cannot be moved on its own and dragging it drags the whole application. Keep wx's modal
    // loop but drop GTK's modal flag, which is what the window manager keys on, as the window shows.
    // Without GTK's modal flag the main window would accept input while this dialog is up (that
    // crashed when a primitive was added mid-dialog), so the dialog blocks it itself: input events
    // aimed at any other window of the application are dropped while it is shown.
    Bind(wxEVT_SHOW, [this](wxShowEvent& e) {
        if (m_widget != nullptr && e.IsShown())
            gtk_window_set_modal(GTK_WINDOW(m_widget), FALSE);
        wxGetApp().set_input_blocking_dialog(e.IsShown() ? this : nullptr);
        e.Skip();
    });
#endif
    SetBackgroundColour(*wxWHITE);
    SetMinSize(DESIGN_WINDOW_SIZE);
    create();
    wxGetApp().UpdateDlgDarkUI(this);
}

void PreferencesDialog::create()
{
    app_config = get_app_config();

    m_parent = new MyscrolledWindow(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxVSCROLL);
    m_parent->SetScrollRate(5, 5);
    m_parent->SetBackgroundColour(*wxWHITE);

    m_sizer_body = new wxBoxSizer(wxVERTICAL);

    m_pref_tabs = new TabCtrl(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxTR_NO_BUTTONS | wxTR_HIDE_ROOT | wxTR_SINGLE | wxTR_NO_LINES | wxBORDER_NONE | wxWANTS_CHARS | wxTR_FULL_ROW_HIGHLIGHT);
    m_pref_tabs->Bind(wxEVT_RIGHT_DOWN, [](auto &e) {}); // disable right select
    m_pref_tabs->SetFont(Label::Body_14);

    create_items();
    wxGetApp().retheme_from_base(); // this dialog's own panels are built with literal colours

    m_pref_tabs->Bind(wxEVT_TAB_SEL_CHANGED, [this](wxCommandEvent& e) {
        Freeze();
        #ifdef __linux__
            m_pref_tabs->SetFocus();
        #endif
        int selection = e.GetSelection();
        for (size_t i = 0; i < m_pref_tabs->GetCount(); ++i){
            m_pref_tabs->SetItemBold(i, i == selection);
            f_sizers[i]->Show(i == selection);
        }
        Layout();
        Thaw();
    });

    auto item_color = StateColor(
        std::make_pair(wxColour("#6B6B6B"), (int) StateColor::NotChecked), // the disabled-text key
        std::make_pair(wxColour("#363636"), (int) StateColor::Normal)
    );

    for (size_t i = 0; i < m_pref_tabs->GetCount(); ++i)
        m_pref_tabs->SetItemTextColour(i, item_color);

    m_pref_tabs->SelectItem(0);

    m_sizer_body->Add(m_pref_tabs, 0, wxEXPAND | wxBOTTOM | wxTOP, FromDIP(5));
    m_sizer_body->Add(m_parent, 1, wxEXPAND);

    SetSizer(m_sizer_body);
    Layout();
    Fit();
    // Resizable; opens at the size it was last closed at (preferences_dialog_size), otherwise
    // taller than the fitted minimum so more of a page shows at once. Kept within the display.
    {
        wxSize size = GetSize();
        const std::string saved = app_config->get("preferences_dialog_size");
        int w = 0, h = 0;
        if (sscanf(saved.c_str(), "%d,%d", &w, &h) == 2 && w > 0 && h > 0)
            size = wxSize(w, h);
        else
            size.y = std::max(size.y, DESIGN_FONT_SCALED(860));
        const wxRect area = wxDisplay(wxDisplay::GetFromWindow(this)).GetClientArea();
        size.x = std::min(size.x, area.width);
        size.y = std::min(size.y, area.height);
        SetSize(size);
    }
    CenterOnParent();
}

PreferencesDialog::~PreferencesDialog()
{
    if (app_config != nullptr && !IsMaximized())
        app_config->set("preferences_dialog_size", std::to_string(GetSize().x) + "," + std::to_string(GetSize().y));
}

void PreferencesDialog::on_dpi_changed(const wxRect &suggested_rect) {
    m_pref_tabs->Rescale();

    int sel = m_pref_tabs->GetSelection();
    for (size_t i = 0; i < m_pref_tabs->GetCount(); ++i)
        f_sizers[i]->Show(true);

    std::function<void(wxWindow*, int)> WalkControls;
    WalkControls = [&](wxWindow* parent, int depth) -> void {
        if (!parent) return;

        for (auto* child : parent->GetChildren()) {
            if (!child)
                continue;
            else if (auto* btn = dynamic_cast<Button*>(child))
                btn->Rescale();
            else if (auto* chk = dynamic_cast<CheckBox*>(child))
                chk->msw_rescale();
            else if (auto* txt = dynamic_cast<TextInput*>(child))
                txt->Rescale();
            else if (auto* cmb = dynamic_cast<ComboBox*>(child))
                cmb->Rescale();
            else if (auto* spn = dynamic_cast<SpinInput*>(child))
                spn->Rescale();
            else if (auto* lbl = dynamic_cast<WikiLabel*>(child)){
                lbl->SetSize(DESIGN_TITLE_SIZE);
                lbl->Rescale();
            }
                
            WalkControls(child, depth + 1);
        }
    };
    WalkControls(this, 0);

    wxCommandEvent event(wxEVT_TAB_SEL_CHANGED, m_pref_tabs->GetId());
    event.SetInt(sel);
    event.SetEventObject(m_pref_tabs);
    m_pref_tabs->GetEventHandler()->ProcessEvent(event);

    Refresh();
}

void PreferencesDialog::Split(const std::string &src, const std::string &separator, std::vector<wxString> &dest)
{
    std::string            str = src;
    std::string            substring;
    std::string::size_type start = 0, index;
    dest.clear();
    index = str.find_first_of(separator, start);
    do {
        if (index != std::string::npos) {
            substring = str.substr(start, index - start);
            dest.push_back(substring);
            start = index + separator.size();
            index = str.find(separator, start);
            if (start == std::string::npos) break;
        }
    } while (index != std::string::npos);

    substring = str.substr(start);
    dest.push_back(substring);
}

void PreferencesDialog::create_items()
{
    // ORCA
    // Window focus follows item creation order. so below code has to be in same order with UI
    // Create functions for custom controls to keep list clean
    // Tooltips added automatically from related title if its empty

    wxBoxSizer*sizer_page = new wxBoxSizer(wxVERTICAL);
    wxFlexGridSizer* g_sizer; // use same name on all sizers to make easier to ordering without renaming
    auto v_gap = FromDIP(4);

    //////////////////////////
    //// GENERAL TAB 
    /////////////////////////////////////
    m_pref_tabs->AppendItem(_L("General"));
    f_sizers.push_back(new wxFlexGridSizer(1, 1, v_gap, 0));
    g_sizer = f_sizers.back();
    g_sizer->AddGrowableCol(0, 1);

    //// GENERAL > Settings
    g_sizer->Add(create_item_title(_L("Settings")), 1, wxEXPAND);

    auto item_language         = create_item_language_combobox(_L("Language"), "");
    g_sizer->Add(item_language);

    std::vector<wxString>Units = {_L("Metric") + " (mm, g)", _L("Imperial") + " (in, oz)"};
    auto item_currency         = create_item_combobox(_L("Units"), "", "use_inches", Units);
    g_sizer->Add(item_currency);

    std::vector<wxString> DefaultPage = {_L("Home"), _L("Prepare")};
    auto item_default_page     = create_item_combobox(_L("Default page"), _L("Set the page opened on startup."), "default_page", DefaultPage);
    g_sizer->Add(item_default_page);


    auto item_single_instance  = create_item_checkbox(_L("Allow only one Unlayered Slicer instance"),
    #if __APPLE__
            _L("On OSX there is always only one instance of app running by default. However it is allowed to run multiple instances "
                "of same app from the command line. In such case this settings will allow only one instance."),
    #else
            _L("If this is enabled, when starting Unlayered Slicer and another instance of the same Unlayered Slicer is already running, that instance will be reactivated instead."),
    #endif
            "single_instance");
    g_sizer->Add(item_single_instance);

    auto item_show_splash_scr  = create_item_checkbox(_L("Show splash screen"), _L("Show the splash screen during startup."), "show_splash_screen");
    g_sizer->Add(item_show_splash_scr);

#ifdef __linux__
#endif

    //auto item_hints            = create_item_checkbox(_L("Show \"Daily Tips\" after start"), page, _L("If enabled, useful hints are displayed at startup."), "show_daily_tips");
    //g_sizer->Add(item_hints);

    auto item_downloads        = create_item_downloads(_L("Downloads folder"), _L("Target folder for downloaded items"));
    g_sizer->Add(item_downloads);

    //// GENERAL > Project
    g_sizer->Add(create_item_title(_L("Project")), 1, wxEXPAND);

    std::vector<wxString> projectLoadSettingsBehaviourOptions = {_L("Load All"), _L("Ask When Relevant"), _L("Always Ask"), _L("Load Geometry Only")};
    std::vector<string>   projectLoadSettingsConfigOptions    = { OPTION_PROJECT_LOAD_BEHAVIOUR_LOAD_ALL, OPTION_PROJECT_LOAD_BEHAVIOUR_ASK_WHEN_RELEVANT, OPTION_PROJECT_LOAD_BEHAVIOUR_ALWAYS_ASK, OPTION_PROJECT_LOAD_BEHAVIOUR_LOAD_GEOMETRY };
    auto item_project_load     = create_item_combobox(_L("Load behaviour"), _L("Should printer/filament/process settings be loaded when opening a 3MF file?"), SETTING_PROJECT_LOAD_BEHAVIOUR, projectLoadSettingsBehaviourOptions, projectLoadSettingsConfigOptions);
    g_sizer->Add(item_project_load);

    auto item_backup           = create_item_backup(_L("Auto backup"), _L("Backup your project periodically to help with restoring from an occasional crash."));
    g_sizer->Add(item_backup); 

    auto item_max_recent_count = create_item_input(_L("Maximum recent files"), "", _L("Maximum count of recent files"), "max_recent_count", [](wxString value) {
        long max = 0;
        if (value.ToLong(&max))
            wxGetApp().mainframe->set_max_recent_count(max);
    });
    g_sizer->Add(item_max_recent_count);

    auto item_recent_models    = create_item_checkbox(_L("Add STL/STEP files to recent files list"), "", "recent_models");
    g_sizer->Add(item_recent_models);

    auto item_gcodes_warning   = create_item_checkbox(_L("Don't warn when loading 3MF with modified G-code"), "", "no_warn_when_modified_gcodes");
    g_sizer->Add(item_gcodes_warning);

    auto item_step_dialog = create_item_checkbox(
        _L("Show options when importing STEP file"), _L("If enabled, a parameter settings dialog will appear during STEP file import."), 
        "enable_step_mesh_setting", wxEmptyString, "import_export#dont-show-again"
    );
    g_sizer->Add(item_step_dialog);

    auto item_step_linear      = create_item_decimal_input(_L("STEP importing: linear deflection"), "mm", _L("Linear deflection used when meshing imported STEP files.\nSmaller values produce higher-quality meshes but increase processing time.\nUsed as the default in the import dialog, or directly when the import dialog is disabled.\nDefault: 0.003 mm."), "linear_deflection", 0.001, 0.1, 3);
    g_sizer->Add(item_step_linear);

    auto item_step_angle       = create_item_decimal_input(_L("STEP importing: angle deflection"), "", _L("Angle deflection used when meshing imported STEP files.\nSmaller values produce higher-quality meshes but increase processing time.\nUsed as the default in the import dialog, or directly when the import dialog is disabled.\nDefault: 0.5."), "angle_deflection", 0.01, 1.0, 2);
    g_sizer->Add(item_step_angle);

    auto item_step_split       = create_item_checkbox(_L("STEP importing: Split into multiple objects"), _L("If enabled, compound and compsolid shapes in imported STEP files are split into multiple objects.\nUsed as the default in the import dialog, or directly when the import dialog is disabled.\nDefault: disabled."), "is_split_compound");
    g_sizer->Add(item_step_split);

    auto item_draco_bits = create_item_spinctrl(_L("Quality level for Draco export"), "",
        _L("bits"),
        _L("Controls the quantization bit depth used when compressing the mesh to Draco format.\n"
           "0 = lossless compression (geometry is preserved at full precision). Valid lossy values range from 8 to 30.\n"
           "Lower values produce smaller files but lose more geometric detail; higher values preserve more detail at the cost of larger files."),
        "drc_bits", DRC_BITS_MIN, DRC_BITS_MAX, nullptr, "import_export#drc"
    );
    g_sizer->Add(item_draco_bits);

    auto item_full_source_paths = create_item_checkbox(_L("Store full source file paths in projects"),
        _L("If enabled, saved projects store the absolute path to imported source files (STEP/STL/...), so "
           "\"Reload from disk\" still works when the source file is kept in a different folder than the project. "
           "If disabled, only the filename is stored, which keeps projects portable and avoids embedding absolute paths."),
        "export_sources_full_pathnames");
    g_sizer->Add(item_full_source_paths);

    //// GENERAL > Preset
    g_sizer->Add(create_item_title(_L("Preset")), 1, wxEXPAND);

    auto item_remember_printer = create_item_checkbox(_L("Remember printer configuration"), _L("If enabled, Unlayered Slicer will remember and switch filament/process configuration for each printer automatically."), "remember_printer_config");
    g_sizer->Add(item_remember_printer);

    auto item_filament_preset_grouping = create_item_combobox(_L("Group user filament presets"), _L("Group user filament presets based on selection"),
        "group_filament_presets", {_L("All"), _L("None"), _L("By type"), _L("By vendor")}, [](wxString value) {wxGetApp().plater()->sidebar().update_presets(Preset::TYPE_FILAMENT);});
    g_sizer->Add(item_filament_preset_grouping);

    // prevent burst calling on keyboard / spin events
    m_filament_height_timer.Bind(wxEVT_TIMER, [this](wxTimerEvent&) {
        wxGetApp().plater()->sidebar().update_filaments_area_height();
        UpdateSidebarLayout();
    });
    auto item_filament_area_height = create_item_spinctrl(_L("Optimize filaments area height for..."), "", _L("filaments"), _L("Optimizes filament area maximum height by chosen filament count."),
        "filaments_area_preferred_count", 8, 99, [this](int value) {m_filament_height_timer.StartOnce(500);});
    g_sizer->Add(item_filament_area_height); 

    auto item_shared_profiles  = create_item_checkbox(_L("Show shared profiles notification"), _L("Show a notification with a link to browse shared profiles when the selected printer is changed."), "show_shared_profiles_notification");
    g_sizer->Add(item_shared_profiles);

    //// GENERAL > Features
    g_sizer->Add(create_item_title(_L("Features")), 1, wxEXPAND);

    auto item_multi_machine    = create_item_checkbox(_L("Multi device management"), _L("With this option enabled, you can send a task to multiple devices at the same time and manage multiple devices."), "enable_multi_machine", _L("(Requires restart)"));
    g_sizer->Add(item_multi_machine);

    //// GENERAL > Web application
    // Unlayered: the web-application addresses are under Advanced > "Web apps & services"
    // (create_item_web_services). What else a slot may do through the bridge are fields on its
    // WebAppSlot (src/slic3r/GUI/WebAppPanel.cpp), seeded into the configuration only where a key
    // is absent. On a development machine ORCA_WEB_APP_URL, ORCA_SIDE_PANEL_URL and
    // ORCA_UNIVERSE_APP_URL point a tab elsewhere; each enables its tab whatever the configuration
    // says (WebAppPanel::is_enabled) and is never written back. WEB_APP_BRIDGE.md section 7 lists
    // every key.

    // Printer updates have no setting here. The configuration feed knows who is asking from the
    // Unlayered account sign-in (Help menu), and firmware updates are public. The pasted "Access
    // token" that used to sit here is gone: the feed accepts only the account's device token.

#if 0
    g_sizer->Add(create_item_title(_L("Filament Grouping")), 1, wxEXPAND);
    //temporarily disable it
    //auto item_ignore_ext_filament = create_item_checkbox(_L("Ignore ext filament when auto grouping"), _L("Ignore ext filament when auto grouping"), 50, "ignore_ext_filament_when_group");
    auto item_pop_up_filament_map_dialog = create_item_checkbox(_L("Pop up to select filament grouping mode"), _L("Pop up to select filament grouping mode"), 50, "pop_up_filament_map_dialog");
    g_sizer->Add(item_pop_up_filament_map_dialog);
#endif

    //// GENERAL > Plugins
    g_sizer->Add(create_item_title(_L("Plugins")), 1, wxEXPAND);

    auto item_plugin_pages_visible_count = create_item_spinctrl(
        _L("Visible plugin pages"),
        "",
        _L("pages"),
        _L("Number of plugin pages shown as fixed tabs before the remaining pages collapse into a dropdown on the last tab."),
        SETTING_PLUGIN_PAGES_VISIBLE_COUNT,
        PLUGIN_PAGES_VISIBLE_COUNT_MIN,
        PLUGIN_PAGES_VISIBLE_COUNT_MAX,
        [](int value) { wxGetApp().mainframe->plugin_pages().set_visible_page_count(value); }
    );
    g_sizer->Add(item_plugin_pages_visible_count);

    g_sizer->AddSpacer(FromDIP(10));
    sizer_page->Add(g_sizer, 0, wxEXPAND);

    //////////////////////////
    /////////////////////////////////////
    //// APPEARANCE TAB
    /////////////////////////////////////
    m_pref_tabs->AppendItem(_L("Appearance"));
    f_sizers.push_back(new wxFlexGridSizer(1, 1, v_gap, 0));
    g_sizer = f_sizers.back();
    g_sizer->AddGrowableCol(0, 1);

    //// APPEARANCE
    g_sizer->Add(create_item_title(_L("Appearance")), 1, wxEXPAND);

    auto item_color_scheme = create_item_combobox(_L("Colour scheme"),
        _L("Follow the system's light or dark appearance, or force one."),
        "color_scheme", {_L("System"), _L("Light"), _L("Dark")}, std::vector<std::string>{"system", "light", "dark"}, "",
        [this](const std::string& scheme) {
            wxGetApp().apply_color_scheme(scheme);
#ifdef __WINDOWS__
            set_dark_mode();
#endif
            refresh_theme_items(); // the theme in effect may have changed with the base
        });
    g_sizer->Add(item_color_scheme);
    // Choosing a theme of the other base switches the colour scheme (ThemeManager::select), so this
    // list follows the setting whenever the theme items refresh.
    for (wxSizerItem* child : item_color_scheme->GetChildren())
        if (auto* scheme_combo = dynamic_cast<::ComboBox*>(child->GetWindow())) {
            m_theme_refresh.push_back([scheme_combo]() {
                const std::string scheme = wxGetApp().app_config->get("color_scheme");
                scheme_combo->SetSelection(scheme == "light" ? 1 : scheme == "dark" ? 2 : 0);
            });
            break;
        }

    auto item_font_scale = create_item_combobox(_L("Font scale"),
        _L("Scale of the interface text. Larger values need a larger window; the settings panels grow with the text. "
           "The window is rebuilt when Preferences closes."),
        "ui_font_scale", {"100%", "125%", "150%", "175%"}, std::vector<std::string>{"100", "125", "150", "175"}, "",
        [this](const std::string& value) {
            // Fonts are baked into every widget at creation, so the main window is rebuilt on
            // close (the language-change path); the new scale must be in Label before that.
            const float scale = std::clamp(std::atoi(value.c_str()), 50, 300) / 100.f;
            ::Label::setFontScale(scale);
            ImGuiWrapper::set_font_scale(scale);
            ::Label::initSysFont();
            m_recreate_GUI = true;
        });
    g_sizer->Add(item_font_scale);

    auto item_font_family = create_item_combobox(_L("Font"),
        _L("The typeface of the interface text. Comic Sans and OpenDyslexic are easier to read for some people; "
           "Comic Sans uses the system's Comic Sans MS where it is installed and the bundled Comic Neue otherwise. "
           "The window is rebuilt when Preferences closes."),
        "ui_font_family", {_L("Default"), "Comic Sans", "OpenDyslexic"}, std::vector<std::string>{"", "comic", "dyslexic"}, "",
        [this](const std::string& value) {
            ::Label::setFontFamily(value);
            ::Label::initSysFont();
            m_recreate_GUI = true;
        });
    g_sizer->Add(item_font_family);

    auto item_window_button_pos  = create_item_checkbox(_L("Use window buttons on left side"), "", "window_buttons_on_left", _L("(Requires restart)"));
    g_sizer->Add(item_window_button_pos);

    //// APPEARANCE > 3D view (not part of a theme)
    g_sizer->Add(create_item_title(_L("3D view")), 1, wxEXPAND);
    auto item_opaque_overlays = create_item_checkbox(
        _L("Opaque overlays"),
        _L("Draw the floating panels of the 3D view (preview summary, tool position, G-code window, "
           "tool panels) and the toolbars fully opaque instead of letting the model show through."),
        "opaque_overlays"
    );
    g_sizer->Add(item_opaque_overlays);

    auto item_color_vision = create_item_combobox(_L("Colour vision"),
        _L("Palettes for the G-code preview's line types and value ranges, and for the bed mesh height map, "
           "chosen to stay distinguishable with the common colour-vision deficiencies."),
        "preview_color_vision", {_L("Normal"), _L("Protanopia"), _L("Deuteranopia"), _L("Tritanopia")},
        std::vector<std::string>{"normal", "protanopia", "deuteranopia", "tritanopia"}, "",
        [](const std::string&) {
            if (Plater* plater = wxGetApp().plater()) {
                if (GLCanvas3D* canvas = plater->get_preview_canvas3D()) {
                    canvas->get_gcode_viewer().apply_color_vision_palette();
                    canvas->set_as_dirty();
                    canvas->request_extra_frame();
                }
            }
        });
    g_sizer->Add(item_color_vision);

    //// APPEARANCE > Theme (named themes; the working copy is edited live)
    g_sizer->Add(create_item_title(_L("Theme")), 1, wxEXPAND);
    {
        auto theme_names = []() {
            std::vector<wxString> names;
            for (const Theme& t : ThemeManager::get().themes())
                names.push_back(t.builtin ? (t.dark ? _L("Dark") : _L("Light")) : t.bundled ? wxGetTranslation(wxString::FromUTF8(t.name)) : wxString::FromUTF8(t.name));
            return names;
        };
        auto theme_index = []() {
            const auto& themes = ThemeManager::get().themes();
            for (size_t i = 0; i < themes.size(); ++i)
                if (themes[i].name == ThemeManager::get().active_name())
                    return (unsigned int) i;
            return 0u;
        };
        auto [sizer, combo] = create_item_combobox_base(_L("Theme"),
            _L("The theme in use. Light and Dark are built in; choosing a theme of the other base switches the colour scheme to it "
               "(with System selected it applies when the desktop is in that mode). The colours below edit the theme live; "
               "editing a built-in theme starts an unsaved copy."),
            "", theme_names(), theme_index());
        auto* status = new wxStaticText(m_parent, wxID_ANY, "");
        status->SetForegroundColour(DESIGN_GRAY900_COLOR);
        status->SetFont(::Label::Body_14);
        sizer->Add(status, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(8));
        // Refilling the list raises selection events of its own; those must not select a theme.
        auto repopulating = std::make_shared<bool>(false);
        auto repopulate = [combo = combo, status, theme_names, theme_index, repopulating]() {
            *repopulating = true;
            combo->Clear();
            for (const wxString& n : theme_names())
                combo->Append(n);
            combo->SetSelection(theme_index());
            *repopulating = false;
            status->SetLabel(ThemeManager::get().modified() ? _L("(modified, unsaved)") : wxString());
        };
        combo->GetDropDown().Bind(wxEVT_COMBOBOX, [this, repopulating](wxCommandEvent& e) {
            if (*repopulating)
                return;
            const auto& themes = ThemeManager::get().themes();
            if (e.GetSelection() >= 0 && e.GetSelection() < int(themes.size())) {
                const bool base_changes = themes[e.GetSelection()].dark != wxGetApp().dark_mode();
                ThemeManager::get().select(themes[e.GetSelection()].name);
#ifdef __WINDOWS__
                // As the Colour scheme list does after apply_color_scheme().
                if (base_changes)
                    set_dark_mode();
#else
                (void) base_changes;
#endif
            }
            refresh_theme_items();
        });
        m_theme_refresh.push_back(repopulate);
        repopulate();
        g_sizer->Add(sizer);

        // Save / Save as / Rename / Delete / Revert act on the theme file behind the working copy.
        wxBoxSizer* row = create_item_label(_L("Theme file"),
            _L("Save writes the changes to the theme's file (a copy of a built-in theme asks for a name). Revert discards unsaved changes."));
        auto add_button = [this](wxBoxSizer* to, const wxString& label, std::function<void()> action) {
            auto* button = new Button(m_parent, label);
            button->SetStyle(ButtonStyle::Regular, ButtonType::Parameter);
            button->Bind(wxEVT_BUTTON, [action](auto&) { action(); });
            to->Add(button, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(6));
            return button;
        };
        // Rename and Delete only apply to a saved user theme (not Light/Dark or a shipped preset).
        auto only_user_theme = [this](Button* button) {
            m_theme_refresh.push_back([button]() {
                const Theme* saved = ThemeManager::get().find(ThemeManager::get().active_name());
                button->Enable(saved != nullptr && !saved->read_only());
            });
        };
        auto ask_name = [this](const wxString& title, const wxString& initial) -> std::string {
            wxTextEntryDialog dlg(this, _L("Theme name"), title, initial);
            if (dlg.ShowModal() != wxID_OK)
                return {};
            const std::string name = dlg.GetValue().Strip(wxString::both).ToUTF8().data();
            if (name == "Light" || name == "Dark") {
                MessageDialog(this, _L("Light and Dark are the built-in themes; choose another name."), _L("Theme"), wxOK | wxICON_WARNING).ShowModal();
                return {};
            }
            return name;
        };
        auto save_as = [this, ask_name]() {
            ThemeManager& tm = ThemeManager::get();
            const Theme* saved = tm.find(tm.active_name());
            const wxString initial = wxString::FromUTF8(saved && !saved->builtin ? saved->name : (tm.active().dark ? "My dark theme" : "My light theme"));
            const std::string name = ask_name(_L("Save theme as"), initial);
            if (name.empty())
                return;
            if (const Theme* existing = tm.find(name); existing && existing->name != tm.active_name()) {
                MessageDialog dlg(this, wxString::Format(_L("Replace the theme \"%s\"?"), wxString::FromUTF8(name)), _L("Theme"), wxYES_NO | wxICON_QUESTION);
                if (dlg.ShowModal() != wxID_YES)
                    return;
            }
            tm.save_as(name);
            refresh_theme_items();
        };
        add_button(row, _L("Save"), [this, save_as]() {
            if (!ThemeManager::get().save())
                save_as();
            refresh_theme_items();
        });
        add_button(row, _L("Save as…"), save_as);
        g_sizer->Add(row);
        // Second row: the two that change which files exist.
        wxBoxSizer* row2 = create_item_label(wxString(), _L("Rename or delete the saved theme the working copy derives from."));
        only_user_theme(add_button(row2, _L("Rename…"), [this, ask_name]() {
            ThemeManager& tm = ThemeManager::get();
            const Theme* saved = tm.find(tm.active_name());
            if (saved == nullptr || saved->builtin)
                return;
            const std::string name = ask_name(_L("Rename theme"), wxString::FromUTF8(saved->name));
            if (!name.empty() && !tm.rename(name))
                MessageDialog(this, _L("A theme with that name already exists."), _L("Theme"), wxOK | wxICON_WARNING).ShowModal();
            refresh_theme_items();
        }));
        only_user_theme(add_button(row2, _L("Delete"), [this]() {
            ThemeManager& tm = ThemeManager::get();
            const Theme* saved = tm.find(tm.active_name());
            if (saved == nullptr || saved->builtin)
                return;
            MessageDialog dlg(this, wxString::Format(_L("Delete the theme \"%s\"?"), wxString::FromUTF8(saved->name)), _L("Theme"), wxYES_NO | wxICON_QUESTION);
            if (dlg.ShowModal() == wxID_YES)
                tm.remove(saved->name);
            refresh_theme_items();
        }));
        g_sizer->Add(row2);
        wxBoxSizer* row3 = create_item_label(_L("Unsaved changes"), _L("Discard the unsaved changes and return to the saved theme."));
        add_button(row3, _L("Revert"), [this]() { ThemeManager::get().revert(); refresh_theme_items(); });
        g_sizer->Add(row3);
        // Import / Export: theme files travel as plain JSON.
        wxBoxSizer* row4 = create_item_label(_L("Theme exchange"),
            _L("Export writes the theme in use to a .json file; Import adds a theme file as a user theme and selects it."));
        add_button(row4, _L("Import…"), [this]() {
            wxFileDialog dlg(this, _L("Import theme"), wxGetApp().app_config->get_last_dir(), "", _L("Theme files") + " (*.json)|*.json", wxFD_OPEN | wxFD_FILE_MUST_EXIST);
            if (dlg.ShowModal() != wxID_OK)
                return;
            auto theme = Theme::from_file(dlg.GetPath().ToUTF8().data());
            if (!theme) {
                MessageDialog(this, _L("This file is not a theme."), _L("Theme"), wxOK | wxICON_WARNING).ShowModal();
                return;
            }
            ThemeManager& tm = ThemeManager::get();
            if (const Theme* existing = tm.find(theme->name); existing && !existing->read_only()) {
                MessageDialog ask(this, wxString::Format(_L("Replace the theme \"%s\"?"), wxString::FromUTF8(theme->name)), _L("Theme"), wxYES_NO | wxICON_QUESTION);
                if (ask.ShowModal() != wxID_YES)
                    return;
            }
            tm.install(*theme);
            refresh_theme_items();
        });
        add_button(row4, _L("Export…"), [this]() {
            const Theme& t = ThemeManager::get().active();
            wxFileDialog dlg(this, _L("Export theme"), wxGetApp().app_config->get_last_dir(), wxString::FromUTF8(Theme::slug(t.name) + ".json"),
                             _L("Theme files") + " (*.json)|*.json", wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
            if (dlg.ShowModal() != wxID_OK)
                return;
            if (!ThemeManager::get().export_to(dlg.GetPath().ToUTF8().data()))
                MessageDialog(this, _L("The theme could not be written."), _L("Theme"), wxOK | wxICON_ERROR).ShowModal();
        });
        g_sizer->Add(row4);
    }

    g_sizer->Add(create_item_title(_L("Interface colours")), 1, wxEXPAND);
    for (const auto& [key, name] : GUI_App::theme_color_roles()) {
        const std::string light_key = key;
        g_sizer->Add(create_item_colour(_(name), _L("Applies immediately to the theme in use. Reset returns this colour to the built-in value."),
            [light_key]() { return StateColor::darkModeColorFor(wxColour(light_key)); },
            [light_key](const wxColour& c) { wxGetApp().set_theme_color(light_key, c); },
            [light_key]() { wxGetApp().set_theme_color(light_key, wxNullColour); }));
    }
    g_sizer->Add(create_item_button(_L("All colours"), _L("Reset to base"), _L("Return every colour of this theme to the value of the built-in theme it derives from."), "",
        [this]() { wxGetApp().reset_theme_colors(); refresh_theme_items(); }));

    g_sizer->Add(create_item_title(_L("3D view colours")), 1, wxEXPAND);
    auto item_view_bg = create_item_colour(_L("3D view background"),
        _L("Background colour of the 3D view for the theme in use. Reset returns the built-in colour."),
        []() { return wxGetApp().get_view_color("view_bg"); },
        [](const wxColour& c) { wxGetApp().set_view_color("view_bg", c); },
        []() { wxGetApp().set_view_color("view_bg", wxNullColour); });
    g_sizer->Add(item_view_bg);

    auto item_plate_model = create_item_colour(_L("Build plate model"),
        _L("Colour of the build plate itself (the printer's bed model, or the flat plate when it has none) for the theme in use. Reset returns the built-in colour."),
        []() { return wxGetApp().get_view_color("plate_model"); },
        [](const wxColour& c) { wxGetApp().set_view_color("plate_model", c); },
        []() { wxGetApp().set_view_color("plate_model", wxNullColour); });
    g_sizer->Add(item_plate_model);

    auto item_plate_grid = create_item_colour(_L("Build plate grid"),
        _L("Colour of the grid lines on the current build plate for the theme in use. Reset returns the built-in colour."),
        []() { return wxGetApp().get_view_color("plate_grid"); },
        [](const wxColour& c) { wxGetApp().set_view_color("plate_grid", c); },
        []() { wxGetApp().set_view_color("plate_grid", wxNullColour); });
    g_sizer->Add(item_plate_grid);

    auto item_toolbar = create_item_colour(_L("3D view toolbars"),
        _L("Background colour of the toolbars drawn inside the 3D view for the theme in use. Reset returns the built-in look."),
        []() { return wxGetApp().get_view_color("toolbar"); },
        [](const wxColour& c) { wxGetApp().set_view_color("toolbar", c); },
        []() { wxGetApp().set_view_color("toolbar", wxNullColour); });
    g_sizer->Add(item_toolbar);

    g_sizer->Add(create_item_title(_L("Text")), 1, wxEXPAND);
    auto item_label_clr_sys = create_item_colour(_L("Colour of system-value labels"),
        _L("Settings that still hold the value of the system preset are labelled in this colour."),
        []() { return wxGetApp().get_label_clr_sys(); },
        [](const wxColour &c) { wxGetApp().set_label_clr_sys(c); },
        []() { wxGetApp().reset_label_clrs(); });
    g_sizer->Add(item_label_clr_sys);

    auto item_label_clr_modified = create_item_colour(_L("Colour of modified-value labels"),
        _L("Settings changed from the preset are labelled in this colour."),
        []() { return wxGetApp().get_label_clr_modified(); },
        [](const wxColour &c) { wxGetApp().set_label_clr_modified(c); },
        []() { wxGetApp().reset_label_clrs(); });
    g_sizer->Add(item_label_clr_modified);
    {
        // Part of the theme, not an app-config key.
        const wxString tip = _L("Recolour text that would fall below 4.5:1 contrast on its background, for example after changing a background colour above. "
                                "Text colours you set yourself are left alone.");
        wxBoxSizer* row = create_item_label(_L("Auto-contrast text"), tip);
        auto* checkbox = new ::CheckBox(m_parent);
        checkbox->SetValue(ThemeManager::get().active().auto_contrast_text);
        checkbox->SetToolTip(tip);
        checkbox->Bind(wxEVT_TOGGLEBUTTON, [this, checkbox](wxCommandEvent& e) { ThemeManager::get().set_auto_contrast_text(checkbox->GetValue()); refresh_theme_items(); e.Skip(); });
        m_theme_refresh.push_back([checkbox]() { checkbox->SetValue(ThemeManager::get().active().auto_contrast_text); });
        row->Add(checkbox, 0, wxALIGN_CENTER);
        g_sizer->Add(row);
    }

    g_sizer->Add(create_item_title(_L("Keyboard focus")), 1, wxEXPAND);
    auto item_focus_ring = create_item_colour(_L("Keyboard focus outline"),
        _L("Colour of the outline drawn around the control that has keyboard focus. It appears after Tab and hides on a mouse click."),
        []() { const std::string& c = ThemeManager::get().active().focus_ring; return wxColour(c.empty() ? "#3B8BF5" : c.c_str()); },
        [](const wxColour& c) { ThemeManager::get().set_focus_ring(c); },
        []() { ThemeManager::get().set_focus_ring(wxNullColour); });
    g_sizer->Add(item_focus_ring);

    g_sizer->AddSpacer(FromDIP(10));
    sizer_page->Add(g_sizer, 0, wxEXPAND);

    /////////////////////////////////////
    //////////////////////////

    //// CONTROL TAB
    /////////////////////////////////////
    m_pref_tabs->AppendItem(_L("Control"));
    f_sizers.push_back(new wxFlexGridSizer(1, 1, v_gap, 0));
    g_sizer = f_sizers.back();
    g_sizer->AddGrowableCol(0, 1);

    //// CONTROL > Behaviour
    g_sizer->Add(create_item_title(_L("Behaviour")), 1, wxEXPAND);

    std::vector<wxString> FlushOptionLabels = {_L("All"),_L("Color"),_L("None")};
    std::vector<std::string> FlushOptionValues = { "all","color change","disabled" };
    auto item_auto_flush = create_item_combobox(_L("Auto flush after changing..."), _L("Auto calculate flushing volumes when selected values changed"), "auto_calculate_flush", FlushOptionLabels, FlushOptionValues);
    g_sizer->Add(item_auto_flush);

    auto item_auto_arrange     = create_item_checkbox(_L("Auto arrange plate after cloning"), "", "auto_arrange");
    g_sizer->Add(item_auto_arrange);

    //// CONTROL > Slicing
    g_sizer->Add(create_item_title(_L("Slicing")), 1, wxEXPAND);

    auto item_auto_reslice = create_item_auto_reslice(
        _L("Auto slice after changes"),
        _L("If enabled, Unlayered Slicer will re-slice automatically whenever slicing-related settings change."),
        _L("Delay in seconds before auto slicing starts, allowing multiple edits to be grouped. Use 0 to slice immediately."));
    g_sizer->Add(item_auto_reslice);

    auto item_mix_print_high_low_temperature = create_item_checkbox(_L("Remove mixed temperature restriction"), _L("With this option enabled, you can print materials with a large temperature difference together."), "enable_high_low_temp_mixed_printing");
    g_sizer->Add(item_mix_print_high_low_temperature);
 
    //// CONTROL > Camera
    g_sizer->Add(create_item_title(_L("Camera")), 1, wxEXPAND);

    std::vector<wxString> CameraNavStyle = {_L("Default"), _L("Touchpad")};
    auto item_camera_nav_style = create_item_combobox(_L("Camera style"), _L("Select camera navigation style.\nDefault: LMB+move for rotation, RMB/MMB+move for panning.\nTouchpad: Alt+move for rotation, Shift+move for panning."), "camera_navigation_style", CameraNavStyle);
    g_sizer->Add(item_camera_nav_style);

    auto camera_orbit_mult     = create_camera_orbit_mult_input(_L("Orbit speed multiplier"), _L("Multiplies the orbit speed for finer or coarser camera movement."));
    g_sizer->Add(camera_orbit_mult);

    auto item_zoom_to_mouse    = create_item_checkbox(_L("Zoom to mouse position"), _L("Zoom in towards the mouse pointer's position in the 3D view, rather than the 2D window center."), "zoom_to_mouse");
    g_sizer->Add(item_zoom_to_mouse);

    auto item_use_free_camera  = create_item_checkbox(_L("Use free camera"), _L("If enabled, use free camera. If not enabled, use constrained camera."), "use_free_camera");
    g_sizer->Add(item_use_free_camera);

    auto reverse_mouse_zoom    = create_item_checkbox(_L("Reverse mouse zoom"), _L("If enabled, reverses the direction of zoom with mouse wheel."), "reverse_mouse_wheel_zoom");
    g_sizer->Add(reverse_mouse_zoom);

    std::vector<wxString> ButtonDragActions = {_L("None"), _L("Pan"), _L("Rotate")};
    auto item_left_mouse_drag  = create_item_combobox(_L("Left Mouse Drag"), _L("Set the action that dragging the left mouse button should perform."), "left_mouse_drag_action", ButtonDragActions);
    g_sizer->Add(item_left_mouse_drag);
    auto item_middle_mouse_drag  = create_item_combobox(_L("Middle Mouse Drag"), _L("Set the action that dragging the middle mouse button should perform."), "middle_mouse_drag_action", ButtonDragActions);
    g_sizer->Add(item_middle_mouse_drag);
    auto item_right_mouse_drag  = create_item_combobox(_L("Right Mouse Drag"), _L("Set the action that dragging the right mouse button should perform."), "right_mouse_drag_action", ButtonDragActions);
    g_sizer->Add(item_right_mouse_drag);

    //// CONTROL > Clear my choice on ...
    g_sizer->Add(create_item_title(_L("Clear my choice on...")), 1, wxEXPAND);

    auto item_save_choise      = create_item_button(_L("Unsaved projects"), _L("Clear"), "", _L("Clear my choice on the unsaved projects."), []() {
        wxGetApp().app_config->set("save_project_choise", "");
    });
    g_sizer->Add(item_save_choise);

    auto item_save_presets     = create_item_button(_L("Unsaved presets"), _L("Clear"), "", _L("Clear my choice on the unsaved presets."), []() {
        wxGetApp().app_config->set("save_preset_choise", "");
    });
    g_sizer->Add(item_save_presets);

    auto item_restore_hide_pop_ups = create_item_button(_L("Synchronizing printer preset"), _L("Clear"), L"", _L("Clear my choice for synchronizing printer preset after loading the file."), []() {
        wxGetApp().app_config->erase("app", "sync_after_load_file_show_flag");
    });
    g_sizer->Add(item_restore_hide_pop_ups);

    g_sizer->AddSpacer(FromDIP(10));
    sizer_page->Add(g_sizer, 0, wxEXPAND);

    //////////////////////////
    //// GRAPHICS TAB
    /////////////////////////////////////
    m_pref_tabs->AppendItem(_L("Graphics"));
    f_sizers.push_back(new wxFlexGridSizer(1, 1, v_gap, 0));
    g_sizer = f_sizers.back();
    g_sizer->AddGrowableCol(0, 1);

    //// GRAPHICS > General
    g_sizer->Add(create_item_title(_L("General")), 1, wxEXPAND);

    auto smooth_normals = create_item_checkbox(
        _L("Smooth normals"),
        _L("Applies smooth normals to the model.\n\nRequires manual scene reload to take effect "
                                "(right-click on 3D view → \"Reload All\")."),
        SETTING_OPENGL_PHONG_SMOOTH_NORMALS
    );
    g_sizer->Add(smooth_normals);

    auto item_cut_chunk_colors = create_item_checkbox(
        _L("Color cut chunks"),
        _L("Draw each chunk of an object that has cut planes in its own color in the Prepare view, "
           "without opening the cut planes tool. Filament colors are not shown for those objects "
           "while this is on."),
        "show_cut_chunk_colors"
    );
    g_sizer->Add(item_cut_chunk_colors);

    //// GRAPHICS > Realistic view
    g_sizer->Add(create_item_title(_L("Realistic View")), 1, wxEXPAND);

    auto item_realistic_phong = create_item_checkbox(
        _L("Phong shading"),
        _L("Uses Phong shading inside realistic view.")
        , SETTING_OPENGL_REALISTIC_PHONG
    );
    g_sizer->Add(item_realistic_phong);

    auto item_realistic_ssao = create_item_checkbox(
        _L("SSAO ambient occlusion"),
        _L("Applies SSAO in realistic view."),
        SETTING_OPENGL_PHONG_SSAO
    );
    g_sizer->Add(item_realistic_ssao);

    auto item_realistic_shadows = create_item_checkbox(
        _L("Shadows"),
        _L("Renders cast shadows on the plate, other objects, and each object onto itself in realistic view."),
        SETTING_OPENGL_PHONG_BASIC_PLATE_SHADOWS
    );
    g_sizer->Add(item_realistic_shadows);

    //// GRAPHICS > Anti-aliasing
    g_sizer->Add(create_item_title(_L("Anti-aliasing")), 1, wxEXPAND);

    auto item_antialiasing = create_item_combobox(
        _L("MSAA Multiplier"),
        _L("Set the Multi-Sample Anti-Aliasing level.\n"
           "Higher values result in smoother edges, but the impact on performance is exponential.\n"
           "Lower values improve performance, at the cost of jagged edges.\n"
           "If disabled, its recommended to enable FXAA to reduce jagged edges with minimal performance impact.\n\n"
           "Requires application restart."),
        SETTING_OPENGL_AA_SAMPLES,
        {_L("Disabled"), "2x", "4x", "8x", "16x"},
        {"0", "2", "4", "8", "16"}
    );
    g_sizer->Add(item_antialiasing);

    auto item_fxaa = create_item_checkbox(
        _L("FXAA post-processing"),
        _L("Applies Fast Approximate Anti-Aliasing as a screen-space pass.\n"
           "Useful for disabling or reducing the MSAA setting to improve performance.\n\n"
           "Takes effect immediately."),
        SETTING_OPENGL_FXAA_ENABLED
    );
    g_sizer->Add(item_fxaa);

    //// GRAPHICS > FPS
    g_sizer->Add(create_item_title(_L("FPS")), 1, wxEXPAND);

    auto item_fps_cap = create_item_spinctrl(
        _L("FPS cap"),
        _L("(0 = unlimited)"),
        _L("FPS"),
        _L("Limits viewport frame rate to reduce GPU load and power usage.\n"
           "Set to 0 for unlimited frame rate."),
        SETTING_OPENGL_FPS_CAP,
        0,
        240
    );
    g_sizer->Add(item_fps_cap);

    auto item_fps_overlay = create_item_checkbox(
        _L("Show FPS overlay"),
        _L("Displays current viewport FPS in the top-right corner."),
        SETTING_OPENGL_SHOW_FPS_OVERLAY
    );
    g_sizer->Add(item_fps_overlay);

    //// GRAPHICS > G-code Preview
    g_sizer->Add(create_item_title(_L("G-code Preview")), 1, wxEXPAND);

    auto item_dim_previous_layers = create_item_checkbox(
        _L("Dim lower layers"),
        _L("When scrubbing the layer slider in the sliced preview, render the layers below the current one darkened so that only the layer being viewed is shown at full brightness."),
        "preview_dim_previous_layers"
    );
    g_sizer->Add(item_dim_previous_layers);

    auto item_dim_previous_layers_brightness = create_item_spinctrl(
        _L("Dimmed layer brightness"),
        "",
        _L("%"),
        _L("How brightly the dimmed layers are rendered when \"Dim lower layers\" is enabled.\n"
           "99% is barely darkened, 0% renders them black. Capped at 99% because 100% would be the same as disabling the option."),
        "preview_dim_previous_layers_brightness",
        0,
        99,
        // ORCA: apply the new brightness immediately to the currently loaded preview
        [](int value) {
            if (Plater* plater = wxGetApp().plater()) {
                if (GLCanvas3D* canvas = plater->get_preview_canvas3D()) {
                    canvas->get_gcode_viewer().set_dim_previous_layers_brightness(0.01f * value);
                    canvas->set_as_dirty();
                    canvas->request_extra_frame();
                }
            }
        }
    );
    g_sizer->Add(item_dim_previous_layers_brightness);

    g_sizer->AddSpacer(FromDIP(10));
    sizer_page->Add(g_sizer, 0, wxEXPAND);

    //////////////////////////
    //// ONLINE TAB
    /////////////////////////////////////
    m_pref_tabs->AppendItem(_L("Online"));
    f_sizers.push_back(new wxFlexGridSizer(1, 1, v_gap, 0));
    g_sizer = f_sizers.back();
    g_sizer->AddGrowableCol(0, 1);

    //// ONLINE > Connection
    g_sizer->Add(create_item_title(_L("Connection")), 1, wxEXPAND);

    // Unlayered: the cloud-account settings (login region, the home page's login panel) are never
    // shown. This build has no Orca Cloud and no Bambu cloud account, so they could only advertise
    // a service it does not talk to. Until 0.1.6 they were hidden only while stealth mode was on,
    // which stopped covering everyone once the first-run page asked about stealth mode with the
    // box unticked. The saved login region is kept as it is.
    const bool show_cloud_ui = false;

    if (show_cloud_ui) {
        auto item_region = create_item_region_combobox(_L("Login region"), "");
        g_sizer->Add(item_region);
    }

    auto item_stealth_mode     = create_item_checkbox(_L("Stealth mode"), _L("Disables all outbound cloud features.\nThe check for new versions of Unlayered Slicer still runs, and connections to printers on your own network are unaffected. Usage diagnostics have their own setting below."), "stealth_mode");
    g_sizer->Add(item_stealth_mode);

    if (show_cloud_ui) {
        // Only meaningful when there is a panel to hide; stealth mode already hides it.
        auto item_hide_login_side_panel = create_item_checkbox(_L("Hide login side panel"), _L("Hide the login side panel on the home page."), "hide_login_side_panel");
        g_sizer->Add(item_hide_login_side_panel);
    }

    auto item_network_test     = create_item_button(_L("Network test"), _L("Test") + " " + dots, "", _L("Open Network Test"), []() {
        NetworkTestDialog dlg(wxGetApp().mainframe);
        dlg.ShowModal();
    });
    g_sizer->Add(item_network_test);

    // Unlayered 0.1.6: usage diagnostics follow the privacy choice (slic3r/Utils/PrivacyConsent.hpp),
    // made in setup or here. Its own switch, NOT part of stealth mode. Changes apply at once.
    g_sizer->Add(create_item_title(_L("Privacy")), 1, wxEXPAND);
    wxString diagnostics_tip = _L("Help us improve reliability and features by sharing application usage, settings, and performance information. Never includes your name, email, account, or 3D models.");
    if (PrivacyConsent::organization_opt_out())
        diagnostics_tip += "\n" + _L("Switched off on this computer by your organization (the UNLAYERED_NO_TELEMETRY environment variable).");
    auto item_usage_diagnostics = create_item_checkbox(_L("Send usage diagnostics"), diagnostics_tip, "usage_diagnostics");
    g_sizer->Add(item_usage_diagnostics);
    const wxString program_tip = _L("Get early access to new features. Your shared data, never linked to your name, email, or account, may be used to develop, train, test, evaluate, and improve AI, CAD, slicing, software, manufacturing, and hardware systems.");
    auto item_improvement_program = create_item_checkbox(_L("Slicing improvement program"), program_tip, "usage_improvement_program");
    g_sizer->Add(item_improvement_program);
    {
        auto add_note = [this, g_sizer](const wxString &text, wxStaticText **out) {
            auto *line = new wxBoxSizer(wxHORIZONTAL);
            line->AddSpacer(FromDIP(DESIGN_LEFT_MARGIN));
            auto *label = new wxStaticText(m_parent, wxID_ANY, text);
            label->SetFont(::Label::Body_13);
            label->SetForegroundColour(DESIGN_GRAY600_COLOR);
            label->Wrap(FromDIP(560));
            line->Add(label, 0, wxTOP | wxBOTTOM, FromDIP(2));
            g_sizer->Add(line);
            if (out)
                *out = label;
        };
        add_note(diagnostics_tip, nullptr);
        add_note(wxEmptyString, &m_privacy_status);
        g_sizer->Add(create_item_button(_L("Where you live"), _L("Change") + " " + dots,
                                        _L("Choose where you live again. You then make your privacy choice again for it."), "",
                                        [this]() {
                                            run_privacy_setup(this, "preferences");
                                            update_privacy_controls();
                                        }));
        g_sizer->Add(create_item_button(_L("Privacy policy"), _L("Read Full Privacy Policy"), "", "", []() { PrivacyFlow::open_policy(); }));
        add_note(_L("Your 3D models, CAD files, and project geometry are not uploaded through usage diagnostics. Unlayered asks you separately, for each model, before uploading one."), nullptr);
        update_privacy_controls();
    }

    // Unlayered: the "Enable Bambu Cloud" option was removed with the Bambu account surface.
    // Bambu printers are used over LAN only; there is no Bambu cloud to opt into.

    //// ONLINE > Update & sync
    g_sizer->Add(create_item_title(_L("Update & sync")), 1, wxEXPAND);

    auto item_stable_updates   = create_item_checkbox(_L("Check for stable updates only"), "", "check_stable_update_only");
    g_sizer->Add(item_stable_updates);

    auto item_user_sync        = create_item_checkbox(_L("Auto sync user presets (Printer/Filament/Process)"), "", "sync_user_preset");
    g_sizer->Add(item_user_sync);

    if (app_config->get_stealth_mode()) {
        if (m_bambu_cloud_checkbox)      m_bambu_cloud_checkbox->Enable(false);
        if (m_sync_user_preset_checkbox) m_sync_user_preset_checkbox->Enable(false);
    }

    // Unlayered: the "Update built-in Presets automatically." checkbox is gone. Built-in profiles
    // ship with the application and are installed on startup; there is nothing to switch off.

    auto item_token_storage    = create_item_checkbox(_L("Use encrypted file for token storage"),
                                                      _L("Store authentication tokens in an encrypted file instead of the system keychain. (Requires restart)"),
                                                      SETTING_USE_ENCRYPTED_TOKEN_FILE);
    g_sizer->Add(item_token_storage);

    //// ONLINE > Filament Sync Options
    g_sizer->Add(create_item_title(_L("Filament Sync Options")), 1, wxEXPAND);

    auto item_filament_sync_mode = create_item_combobox(
        _L("Filament sync mode"),
        _L("Choose whether sync updates both filament preset and color, or only color."),
        "sync_ams_filament_mode",
        {_L("Filament & Color"), _L("Color only")});
    g_sizer->Add(item_filament_sync_mode);

    //// ONLINE > Network plugin
    // Unlayered: opt-in, off by default. Outside show_cloud_ui on purpose - LAN printing to a Bambu
    // printer needs the plug-in but not the cloud, so it must stay reachable in stealth mode.
    g_sizer->Add(create_item_title(_L("Bambu network plug-in")), 1, wxEXPAND);

    auto item_enable_plugin  = create_item_checkbox(_L("Enable Bambu network plug-in"),
                                                    _L("Downloads Bambu Lab's closed-source network plug-in from Bambu Lab's servers. Required to send prints to Bambu Lab printers over LAN or Bambu Cloud."),
                                                    "installed_networking");
    g_sizer->Add(item_enable_plugin);

    auto item_plugin_version = create_item_network_plugin_version(_L("Network plug-in version"), _L("Select the network plug-in version to use"));
    g_sizer->Add(item_plugin_version);

    g_sizer->AddSpacer(FromDIP(10));
    sizer_page->Add(g_sizer, 0, wxEXPAND);

    //////////////////////////
    //// ASSOCIATE TAB 
    /////////////////////////////////////
#ifdef _WIN32
    // MSIX: associations are declared in the package manifest and defaults are
    // managed by Windows Settings; the runtime registry toggles below cannot work.
    // Show a minimal page that sends the user to Windows' Default Apps settings instead.
    if (is_running_in_msix()) {
        m_pref_tabs->AppendItem(_L("Associate"));
        f_sizers.push_back(new wxFlexGridSizer(1, 1, v_gap, 0));
        g_sizer = f_sizers.back();
        g_sizer->AddGrowableCol(0, 1);

        g_sizer->Add(create_item_title(_L("Associate files to Unlayered Slicer")), 1, wxEXPAND);

        auto item_open_default_apps = create_item_button(
            _L("File associations for the Microsoft Store version are managed by Windows Settings."),
            _L("Open Windows Default Apps Settings"), "", "",
            []() { wxLaunchDefaultBrowser("ms-settings:defaultapps"); });
        g_sizer->Add(item_open_default_apps);

        g_sizer->AddSpacer(FromDIP(10));
        sizer_page->Add(g_sizer, 0, wxEXPAND);
    } else {
    m_pref_tabs->AppendItem(_L("Associate"));
    f_sizers.push_back(new wxFlexGridSizer(1, 1, v_gap, 0));
    g_sizer = f_sizers.back();
    g_sizer->AddGrowableCol(0, 1);

    //// ASSOCIATE > Extensions
    g_sizer->Add(create_item_title(_L("Associate files to Unlayered Slicer")), 1, wxEXPAND);

    auto item_associate_3mf    = create_item_checkbox(_L("Associate 3MF files to Unlayered Slicer"), _L("If enabled, this sets Unlayered Slicer as the default application to open 3MF files.") , "associate_3mf");
    g_sizer->Add(item_associate_3mf);

    auto item_associate_drc = create_item_checkbox(_L("Associate DRC files to Unlayered Slicer"), _L("If enabled, sets Unlayered Slicer as default application to open DRC files."), "associate_drc");
    g_sizer->Add(item_associate_drc);

    auto item_associate_stl    = create_item_checkbox(_L("Associate STL files to Unlayered Slicer"), _L("If enabled, this sets Unlayered Slicer as the default application to open STL files.") , "associate_stl");
    g_sizer->Add(item_associate_stl);

    auto item_associate_step   = create_item_checkbox(_L("Associate STEP files to Unlayered Slicer"), _L("If enabled, this sets Unlayered Slicer as the default application to open STEP files."), "associate_step");
    g_sizer->Add(item_associate_step);

    //// ASSOCIATE > WebLinks
    g_sizer->Add(create_item_title(_L("Associate web links to Unlayered Slicer")), 1, wxEXPAND);

    auto associate_url_prusa   = create_item_link_association(L"prusaslicer", "Printables.com");
    g_sizer->Add(associate_url_prusa);

    auto associate_url_bambu   = create_item_link_association(L"bambustudio", "Makerworld.com");
    g_sizer->Add(associate_url_bambu);

    auto associate_url_cura    = create_item_link_association(L"cura", "Thingiverse.com");
    g_sizer->Add(associate_url_cura);

    g_sizer->AddSpacer(FromDIP(10));
    sizer_page->Add(g_sizer, 0, wxEXPAND);
    }
#endif // _WIN32

    //////////////////////////
    //// ADVANCED TAB
    /////////////////////////////////////
    m_pref_tabs->AppendItem(_L("Advanced"));
    f_sizers.push_back(new wxFlexGridSizer(1, 1, v_gap, 0));
    g_sizer = f_sizers.back();
    g_sizer->AddGrowableCol(0, 1);

    //// ADVANCED > Web apps & services
    // Every external page and service this program talks to, as an address the user can change or
    // blank (= off). Nothing here is specific to a provider: the side panel and the web-app tab
    // speak WEB_APP_BRIDGE.md, the slicing service SLICE_SERVICE_PROTOCOL.md, and the account
    // provider the provider section of the latter. The shipped addresses are seeded only where a
    // key is absent, so what is typed here always stands.
    g_sizer->Add(create_item_title(_L("Web apps & services")), 1, wxEXPAND);
    {
        auto *warning_sizer = new wxBoxSizer(wxHORIZONTAL);
        auto *warning       = new wxStaticText(m_parent, wxID_ANY,
                                               _L("Pages at these addresses can read the plate and, where the slicer allows it, change "
                                                  "settings. Only enter addresses you trust. Leave a field empty to turn it off."));
        warning->SetForegroundColour(DESIGN_GRAY900_COLOR);
        warning->SetFont(::Label::Body_13);
        warning->Wrap(FromDIP(520));
        warning_sizer->AddSpacer(FromDIP(DESIGN_LEFT_MARGIN));
        warning_sizer->Add(warning, 0, wxALIGN_CENTER_VERTICAL);
        g_sizer->Add(warning_sizer);
    }

    // A slot's tab or sidebar exists only if it was enabled at start-up (MainFrame creates it
    // then), so an address set here for a slot that had none takes effect after a restart; one that
    // had an address reloads at once.
    auto web_slot_commit = [](const WebAppSlot &slot) {
        return [&slot]() {
            AppConfig *cfg = wxGetApp().app_config;
            if (cfg == nullptr)
                return;
            cfg->set_bool(slot.key_enabled, cfg->has(slot.key_url));
            cfg->save();
            if (wxGetApp().mainframe != nullptr)
                wxGetApp().mainframe->refresh_web_app(slot);
        };
    };
    g_sizer->Add(create_item_url_input(_L("Side panel URL"),
                                       _L("A web page shown as a panel beside the tabs (View > Show Side Panel). "
                                          "Empty = no side panel. A newly set address appears after a restart."),
                                       SETTING_SIDE_PANEL_URL, web_slot_commit(WebAppSlot::side_panel())));
    g_sizer->Add(create_item_url_input(_L("Web-app tab URL"),
                                       _L("A web page shown as its own tab. Empty = no tab. A newly set address "
                                          "appears after a restart."),
                                       SETTING_WEB_APP_URL, web_slot_commit(WebAppSlot::generic())));
    g_sizer->Add(create_item_url_input(_L("Slicing service URL"),
                                       _L("A slicing service the Slice button can send the plate to (its modes "
                                          "appear in the Slice dropdown). Empty = slice on this computer only."),
                                       SETTING_NONPLANAR_SLICE_URL, []() { SliceService::refresh_schema_async(); }));
    g_sizer->Add(create_item_url_input(_L("Account provider URL"),
                                       _L("Where this program signs in (Help > Sign in): the account used for updates and, "
                                          "where allowed, for the slicing service. Empty = no account. Takes effect "
                                          "after a restart."),
                                       SETTING_ACCOUNT_PROVIDER_URL, []() {}));
    g_sizer->Add(create_item_input(_L("Service token origins"), "",
                                   _L("Comma-separated origins (https://host) besides the account provider's own site "
                                      "that may receive a sign-in token for the slicing service. Any other service "
                                      "gets none and does its own sign-in."),
                                   SETTING_SERVICE_TOKEN_ORIGINS, {}, wxEmptyString, wxFILTER_NONE));

    g_sizer->AddSpacer(FromDIP(10));
    sizer_page->Add(g_sizer, 0, wxEXPAND);

    //////////////////////////
    //// DEVELOPER TAB
    /////////////////////////////////////
    m_pref_tabs->AppendItem(_L("Developer"));
    f_sizers.push_back(new wxFlexGridSizer(1, 1, v_gap, 0));
    g_sizer = f_sizers.back();
    g_sizer->AddGrowableCol(0, 1);

    //// DEVELOPER > Settings
    g_sizer->Add(create_item_title(_L("Settings")), 1, wxEXPAND);

    auto item_develop_mode     = create_item_checkbox(_L("Developer mode"), "", "developer_mode", wxEmptyString, "option_mode#developer+mode");
    g_sizer->Add(item_develop_mode);

    auto item_ams_blacklist    = create_item_checkbox(_L("Skip AMS blacklist check"), "", "skip_ams_blacklist_check");
    g_sizer->Add(item_ams_blacklist);
  
    auto item_show_unsupported = create_item_checkbox(_L("Show unsupported presets"), _L("Show incompatible/unsupported presets in the printer and filament dropdown lists. These presets cannot be selected."), "show_unsupported_presets");
    g_sizer->Add(item_show_unsupported);

    auto item_plugin_printer_agents = create_item_checkbox(
        _L("(Experimental) Use printer agents instead of print hosts"), _L(
            "Route print jobs for non-Bambu printers through printer plug-in agents instead of the classic print-host upload flow.\nWhen disabled, Unlayered Slicer uses the legacy print-host behavior."),
        "use_printer_agents");
    g_sizer->Add(item_plugin_printer_agents);

    //// DEVELOPER > Experimental Features
    g_sizer->Add(create_item_title(_L("Experimental Features")), 1, wxEXPAND);

    auto item_keep_painting    = create_item_checkbox(_L("Keep painted feature after mesh change"), _L("Attempt to keep painted features (color/seam/support/fuzzy etc.) after changing the object mesh (such as cut/reload from disk/simplify/fix etc.)\nHighly experimental! Slow and may create artifact."), "keep_painting");
    g_sizer->Add(item_keep_painting);

    //// DEVELOPER > Storage

    g_sizer->Add(create_item_title(_L("Storage")), 1, wxEXPAND);
    auto item_allow_abnormal_storage = create_item_checkbox(_L("Allow Abnormal Storage"), _L("This allows the use of Storage that is marked as abnormal by the Printer.\nUse at your own risk, can cause issues!"), "allow_abnormal_storage");
    g_sizer->Add(item_allow_abnormal_storage);

    //// DEVELOPER > Log Level
    g_sizer->Add(create_item_title(_L("Log Level")), 1, wxEXPAND);
    auto log_level_list  = std::vector<wxString>{_L("fatal"), _L("error"), _L("warning"), _L("info"), _L("debug"), _L("trace")};
    auto loglevel_combox = create_item_loglevel_combobox(_L("Log Level"), _L("Log Level"), log_level_list);
    g_sizer->Add(loglevel_combox);

    // Unlayered: the developer-tab "Reload network plug-in" button is gone along with the rest of the
    // plug-in path.

    //// DEVELOPER > Debug
#if !BBL_RELEASE_TO_PUBLIC
    g_sizer->Add(create_item_title(_L("Debug")), 1, wxEXPAND);
    auto debug_page            = create_debug_page();
    g_sizer->Add(debug_page, 1, wxEXPAND);
#endif

    g_sizer->AddSpacer(FromDIP(10));
    sizer_page->Add(g_sizer, 0, wxEXPAND);

    /////////////////////////////////////
    //////////////////////////

    g_sizer = nullptr;

    // Hide all tabs instead first one
    for (size_t i = 1; i < f_sizers.size(); ++i)
        f_sizers[i]->Show(false);

    /////////////////////////////////////
    //////////////////////////

    m_parent->SetSizer(sizer_page);
    m_parent->Layout();
    sizer_page->Fit(m_parent);
}

void PreferencesDialog::create_sync_page()
{
    auto page = new wxWindow(this, wxID_ANY);
    wxBoxSizer *sizer_page = new wxBoxSizer(wxVERTICAL);

    auto title_sync_settingy   = create_item_title(_L("Sync settings"));
    auto item_user_sync        = create_item_checkbox(_L("User sync"), _L("User sync"), "user_sync_switch");
    auto item_preset_sync      = create_item_checkbox(_L("Preset sync"), _L("Preset sync"), "preset_sync_switch");
    auto item_preferences_sync = create_item_checkbox(_L("Preferences sync"), _L("Preferences sync"), "preferences_sync_switch");

    sizer_page->Add(title_sync_settingy, 0, wxTOP, 26);
    sizer_page->Add(item_user_sync, 0, wxTOP, 6);
    sizer_page->Add(item_preset_sync, 0, wxTOP, 6);
    sizer_page->Add(item_preferences_sync, 0, wxTOP, 6);

    page->SetSizer(sizer_page);
    page->Layout();
    sizer_page->Fit(page);
}

wxBoxSizer* PreferencesDialog::create_debug_page()
{
    m_internal_developer_mode_def = app_config->get("internal_developer_mode");
    m_backup_interval_def = app_config->get("backup_interval");
    m_iot_environment_def = app_config->get("iot_environment");

    wxBoxSizer *bSizer = new wxBoxSizer(wxVERTICAL);

    auto enable_ssl_for_mqtt = create_item_checkbox(_L("Enable SSL(MQTT)"), _L("Enable SSL(MQTT)"), "enable_ssl_for_mqtt");
    auto enable_ssl_for_ftp = create_item_checkbox(_L("Enable SSL(FTP)"), _L("Enable SSL(MQTT)"), "enable_ssl_for_ftp");
    auto item_internal_developer = create_item_checkbox(_L("Internal developer mode"), _L("Internal developer mode"), "internal_developer_mode");

    auto title_host = create_item_title(_L("Host Setting"));
    // ORCA RadioGroup
    auto radio_group = new RadioGroup(m_parent, {
        _L("DEV host: api-dev.bambu-lab.com/v1"), // 0
        _L("QA  host: api-qa.bambu-lab.com/v1"),  // 1
        _L("PRE host: api-pre.bambu-lab.com/v1"), // 2
        _L("Product host")                        // 3
    }, wxVERTICAL);

    radio_group->SetRadioTooltip(0, "dev_host");
    radio_group->SetRadioTooltip(1, "qa_host");
    radio_group->SetRadioTooltip(2, "pre_host");
    radio_group->SetRadioTooltip(3, "product_host");

    if (m_iot_environment_def == ENV_DEV_HOST) {
        radio_group->SetSelection(0);
    } else if (m_iot_environment_def == ENV_QAT_HOST) {
        radio_group->SetSelection(1);
    } else if (m_iot_environment_def == ENV_PRE_HOST) {
        radio_group->SetSelection(2);
    } else if (m_iot_environment_def == ENV_PRODUCT_HOST) {
        radio_group->SetSelection(3);
    }

    Button* debug_button = new Button(m_parent, _L("Debug save button"));
    debug_button->SetStyle(ButtonStyle::Confirm, ButtonType::Window);

    debug_button->Bind(wxEVT_LEFT_DOWN, [this, radio_group](wxMouseEvent &e) {
        // success message box
        MessageDialog dialog(this, _L("Save debug settings"), _L("Debug settings have been saved successfully!"), wxNO_DEFAULT | wxYES_NO | wxICON_INFORMATION);
        dialog.SetSize(400,-1);
        switch (dialog.ShowModal()) {
        case wxID_NO: {
            //if (m_developer_mode_def != app_config->get("developer_mode")) {
            //    app_config->set_bool("developer_mode", m_developer_mode_def == "true" ? true : false);
            //    m_developer_mode_ckeckbox->SetValue(m_developer_mode_def == "true" ? true : false);
            //}
            //if (m_internal_developer_mode_def != app_config->get("internal_developer_mode")) {
            //    app_config->set_bool("internal_developer_mode", m_internal_developer_mode_def == "true" ? true : false);
            //    m_internal_developer_mode_ckeckbox->SetValue(m_internal_developer_mode_def == "true" ? true : false);
            //}

            if (m_backup_interval_def != m_backup_interval_time) { m_backup_interval_textinput->GetTextCtrl()->SetValue(m_backup_interval_def); }

            if (m_iot_environment_def == ENV_DEV_HOST) {
                radio_group->SetSelection(0);
            } else if (m_iot_environment_def == ENV_QAT_HOST) {
                radio_group->SetSelection(1);
            } else if (m_iot_environment_def == ENV_PRE_HOST) {
                radio_group->SetSelection(2);
            } else if (m_iot_environment_def == ENV_PRODUCT_HOST) {
                radio_group->SetSelection(3);
            }

            break;
        }

        case wxID_YES: {
            // bbs  domain changed
            auto param = radio_group->GetSelection();

            std::map<wxString, wxString> iot_environment_map;
            iot_environment_map["dev_host"] = ENV_DEV_HOST;
            iot_environment_map["qa_host"]  = ENV_QAT_HOST;
            iot_environment_map["pre_host"] = ENV_PRE_HOST;
            iot_environment_map["product_host"] = ENV_PRODUCT_HOST;

            //if (iot_environment_map[param] != m_iot_environment_def) {
            if (true) {
                NetworkAgent* agent = wxGetApp().getAgent();
                if      (param == 0) { // "dev_host"
                    app_config->set("iot_environment", ENV_DEV_HOST);
                }
                else if (param == 1) { // "qa_host"
                    app_config->set("iot_environment", ENV_QAT_HOST);
                }
                else if (param == 2) { // "pre_host"
                    app_config->set("iot_environment", ENV_PRE_HOST);
                }
                else if (param == 3) { // "product_host"
                    app_config->set("iot_environment", ENV_PRODUCT_HOST);
                }

                AppConfig* config = GUI::wxGetApp().app_config;
                std::string country_code = config->get_country_code();
                if (agent) {
                    wxGetApp().request_user_logout();
                    agent->set_country_code(country_code);
                }
                ConfirmBeforeSendDialog confirm_dlg(this, wxID_ANY, _L("Warning"), ConfirmBeforeSendDialog::VisibleButtons::ONLY_CONFIRM);  // ORCA VisibleButtons instead ButtonStyle 
                confirm_dlg.update_text(_L("Cloud environment switched; please login again!"));
                confirm_dlg.on_show();
            }

            // bbs  backup
            //app_config->set("backup_interval", std::string(m_backup_interval_time.mb_str()));
            app_config->save();
            Slic3r::set_backup_interval(boost::lexical_cast<long>(app_config->get("backup_interval")));

            this->Close();
            break;
        }
        }
    });

    bSizer->Add(enable_ssl_for_mqtt, 0, wxTOP, FromDIP(3));
    bSizer->Add(enable_ssl_for_ftp, 0, wxTOP, FromDIP(3));
    bSizer->Add(item_internal_developer, 0, wxTOP, FromDIP(3));
    bSizer->Add(title_host, 0, wxEXPAND | wxTOP, FromDIP(10));
    bSizer->Add(radio_group, 0, wxEXPAND | wxLEFT, FromDIP(DESIGN_LEFT_MARGIN));
    bSizer->Add(debug_button, 0, wxALIGN_CENTER_HORIZONTAL | wxTOP, FromDIP(15));

    return bSizer;
}

void PreferencesDialog::UpdateSidebarLayout()
{
    Plater* plater = wxGetApp().plater();
    if (!plater) return;

    Sidebar& sidebar = plater->sidebar();

    sidebar.Freeze();

    sidebar.Layout();
    //plater->Layout();
    //wxGetApp().mainframe->Layout();

    sidebar.Thaw();

    plater->PostSizeEvent();
}

}} // namespace Slic3r::GUI
