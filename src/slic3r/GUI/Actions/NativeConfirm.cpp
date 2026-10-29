#include "NativeConfirm.hpp"

#include <algorithm>
#include <cstdlib>

#include <boost/algorithm/string/case_conv.hpp>
#include <boost/algorithm/string/trim.hpp>
#include <boost/nowide/fstream.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/log/trivial.hpp>

#include <wx/filedlg.h>

#include "../GUI_App.hpp"
#include "../I18N.hpp"
#include "../MainFrame.hpp"
#include "../MsgDialog.hpp"
#include "../WebAppDialogs.hpp"

namespace Slic3r { namespace GUI { namespace Actions {

AutoConfirm e2e_autoconfirm()
{
    return AutoConfirm::Off;
}

std::string e2e_save_dir()
{
    return {};
}

bool native_confirm(const ConfirmRequest& request)
{
    switch (e2e_autoconfirm()) {
    case AutoConfirm::Accept:
        BOOST_LOG_TRIVIAL(warning) << "Actions: E2E auto-confirm ACCEPTED '" << request.title << "'";
        return true;
    case AutoConfirm::Cancel:
        BOOST_LOG_TRIVIAL(warning) << "Actions: E2E auto-confirm CANCELLED '" << request.title << "'";
        return false;
    default: break;
    }

    wxString text = wxString::FromUTF8(request.message);
    if (!request.details.empty()) {
        text += "\n";
        const size_t shown = std::min<size_t>(request.details.size(), 40);
        for (size_t i = 0; i < shown; ++i)
            text += "\n  " + wxString::FromUTF8(request.details[i]);
        if (request.details.size() > shown)
            text += "\n  " + wxString::Format(_L("... and %d more"), int(request.details.size() - shown));
    }
    text += "\n\n" + _L("An embedded web application asked for this.");

    wxWindow*     parent = wxGetApp().mainframe;
    WebAppDialogs::HumanOnlyScope persons_confirmation; // this dialog IS the consent: no page answers it
    MessageDialog dlg(parent, text, wxString::FromUTF8(request.title),
                      wxYES_NO | wxNO_DEFAULT | (request.destructive ? wxICON_WARNING : wxICON_QUESTION));
    dlg.SetButtonLabel(wxID_YES, request.accept_label.empty() ? _L("Continue") : wxString::FromUTF8(request.accept_label));
    dlg.SetButtonLabel(wxID_NO, _L("Cancel"), true);
    const bool accepted = dlg.ShowModal() == wxID_YES;
    BOOST_LOG_TRIVIAL(info) << "Actions: native confirmation '" << request.title << "' " << (accepted ? "accepted" : "declined");
    return accepted;
}

bool native_save_path(const std::string& title, const std::string& default_name, const std::string& wildcard,
                      std::string& out_path)
{
    out_path.clear();
    if (const std::string dir = e2e_save_dir(); !dir.empty()) {
        if (e2e_autoconfirm() == AutoConfirm::Cancel) {
            BOOST_LOG_TRIVIAL(warning) << "Actions: E2E save dialog CANCELLED '" << title << "'";
            return false;
        }
        boost::system::error_code ec;
        boost::filesystem::create_directories(dir, ec);
        out_path = (boost::filesystem::path(dir) / boost::filesystem::path(default_name).filename()).string();
        BOOST_LOG_TRIVIAL(warning) << "Actions: E2E save dialog answered " << out_path;
        return true;
    }
    WebAppDialogs::HumanOnlyScope persons_choice;
    wxFileDialog dlg(wxGetApp().mainframe, wxString::FromUTF8(title), wxEmptyString, wxString::FromUTF8(default_name),
                     wxString::FromUTF8(wildcard), wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
    if (dlg.ShowModal() != wxID_OK)
        return false;
    out_path = std::string(dlg.GetPath().ToUTF8().data());
    return !out_path.empty();
}

}}} // namespace Slic3r::GUI::Actions
