#include "SysFilesUpdateDialog.hpp"

#include <algorithm>

#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/checklst.h>
#include <wx/dirdlg.h>
#include <wx/filedlg.h>
#include <wx/sizer.h>

#include <boost/algorithm/string.hpp>
#include <boost/format.hpp>
#include <boost/log/trivial.hpp>

#include "libslic3r/PrintConfig.hpp"
#include "GUI.hpp"
#include "GUI_App.hpp"
#include "I18N.hpp"
#include "MsgDialog.hpp"
#include "PrinterUpdateOffer.hpp"
#include "Jobs/BoostThreadWorker.hpp"
#include "Jobs/PlaterWorker.hpp"
#include "slic3r/Utils/DuetRrfClient.hpp"
#include "slic3r/Utils/SysFilesBundle.hpp"

namespace Slic3r {
namespace GUI {

// Drives this dialog's gauge and status line. Deliberately a copy of the firmware dialog's
// indicator rather than a shared one: it is a handful of forwarding calls, and hoisting it would
// mean editing the firmware updater for no functional gain.
class SysFilesProgressIndicator : public ProgressIndicator
{
public:
    SysFilesProgressIndicator(wxGauge *gauge, wxStaticText *status) : m_gauge(gauge), m_status(status) {}

    void clear_percent() override { if (m_gauge) m_gauge->SetValue(0); }
    void show_error_info(wxString, int, wxString, wxString) override {}
    void set_range(int range) override { m_range = range; if (m_gauge) m_gauge->SetRange(range); }
    void set_cancel_callback(CancelFn fn = CancelFn()) override { m_cancel = std::move(fn); }
    void set_progress(int pr) override { if (m_gauge) m_gauge->SetValue(std::min(pr, m_range)); }
    void set_status_text(const char *txt) override
    {
        if (m_status && txt)
            m_status->SetLabel(wxString::FromUTF8(txt));
    }
    int get_range() const override { return m_range; }

    void call_cancel() { if (m_cancel) m_cancel(); }

private:
    wxGauge      *m_gauge  = nullptr;
    wxStaticText *m_status = nullptr;
    int           m_range  = 100;
    CancelFn      m_cancel;
};

// Section in the application config holding, per printer, the card-relative paths to leave out.
static const char *SYS_FILES_SKIP_SECTION = "sys_files_skip";

static wxString format_size(uint64_t bytes)
{
    if (bytes < 1024)
        return wxString::Format("%llu B", (unsigned long long) bytes);
    return wxString::Format("%.1f kB", double(bytes) / 1024.0);
}

// Shown in the notes and in the install confirmation when the zip or folder was a whole SD card.
static wxString full_sd_card_warning()
{
    return _L("This looks like a full SD card. Only its printer configuration (sys and macros) will be "
              "installed; the printer's own config.g, calibration and setup are kept. Firmware, print "
              "files and the web interface were skipped. Update firmware with the Firmware button.");
}

SysFilesUpdateDialog::SysFilesUpdateDialog(wxWindow *parent, DynamicPrintConfig *config)
    : DPIDialog(parent, wxID_ANY, _L("Printer Configuration Files"), wxDefaultPosition, wxDefaultSize,
                wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
{
    if (config) {
        m_host     = config->opt_string("print_host");
        // RepRapFirmware's password shares the generic print host key, as it does for uploads.
        m_password = config->opt_string("printhost_apikey");
    }

    build_ui();
    // The backup option depends on which connection is selected, and build_ui() picks one.
    update_backup_availability();

    m_progress = std::make_shared<SysFilesProgressIndicator>(m_gauge, m_status);
    m_worker   = std::make_unique<PlaterWorker<BoostThreadWorker>>(this, m_progress, "sys_files_worker");
    m_ui_alive = std::make_shared<std::atomic<bool>>(true);

    Bind(EVT_SYS_FILES_UPDATE_MESSAGE, &SysFilesUpdateDialog::on_job_message, this);
    Bind(EVT_SYS_FILES_UPDATE_FINISHED, &SysFilesUpdateDialog::on_job_finished, this);
    Bind(wxEVT_CLOSE_WINDOW, &SysFilesUpdateDialog::on_close, this);

    wxGetApp().UpdateDlgDarkUI(this);
    start_query();
}

SysFilesUpdateDialog::~SysFilesUpdateDialog()
{
    // If the dialog is torn down by some path other than dismiss()/on_close, the job must still
    // stop posting events to this window before the worker is joined.
    if (m_ui_alive)
        *m_ui_alive = false;
}

void SysFilesUpdateDialog::build_ui()
{
    auto     *main = new wxBoxSizer(wxVERTICAL);
    const int pad  = FromDIP(8);

    // How this dialog reaches the printer. A machine with no address is still updatable over
    // USB, so this row comes before anything that describes the printer.
    m_connection = new DuetConnectionPicker(this, m_host, m_password);
    m_connection->on_changed([this]() {
        if (m_phase != Phase::Working) {
            update_backup_availability();
            start_query();
        }
    });
    main->Add(m_connection, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(16));

    m_printer_label = new wxStaticText(this, wxID_ANY,
        m_host.empty() ? _L("Printer on USB")
                       : wxString::Format(_L("Printer at %s"), wxString::FromUTF8(m_host.c_str())));
    auto font = m_printer_label->GetFont();
    font.MakeBold();
    m_printer_label->SetFont(font);
    main->Add(m_printer_label, 0, wxLEFT | wxRIGHT | wxTOP, FromDIP(16));

    m_installed_label = new wxStaticText(this, wxID_ANY, _L("On the printer: checking..."));
    m_available_label = new wxStaticText(this, wxID_ANY, _L("Available: checking..."));
    main->Add(m_installed_label, 0, wxLEFT | wxRIGHT | wxTOP, FromDIP(16));
    main->Add(m_available_label, 0, wxLEFT | wxRIGHT | wxTOP, pad / 2);

    m_notes = new wxTextCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition,
                             wxSize(FromDIP(520), FromDIP(90)),
                             wxTE_MULTILINE | wxTE_READONLY | wxTE_BESTWRAP | wxBORDER_SIMPLE);
    main->Add(m_notes, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(16));

    auto *files_header = new wxBoxSizer(wxHORIZONTAL);
    files_header->Add(new wxStaticText(this, wxID_ANY, _L("Files to write to the printer's SD card:")),
                      0, wxALIGN_CENTER_VERTICAL);
    files_header->AddStretchSpacer();
    m_all_btn  = new wxButton(this, wxID_ANY, _L("All"), wxDefaultPosition, wxDefaultSize, wxBU_EXACTFIT);
    m_none_btn = new wxButton(this, wxID_ANY, _L("None"), wxDefaultPosition, wxDefaultSize, wxBU_EXACTFIT);
    files_header->Add(m_all_btn, 0, wxRIGHT, pad / 2);
    files_header->Add(m_none_btn, 0);
    main->Add(files_header, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(16));

    m_files = new wxCheckListBox(this, wxID_ANY, wxDefaultPosition, wxSize(FromDIP(520), FromDIP(180)));
    main->Add(m_files, 1, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(16));

    m_backup_check = new wxCheckBox(this, wxID_ANY, _L("Save the files being replaced to this computer first"));
    m_backup_check->SetValue(true);
    main->Add(m_backup_check, 0, wxLEFT | wxRIGHT | wxTOP, FromDIP(16));

    m_restart_check = new wxCheckBox(this, wxID_ANY, _L("Restart the printer when finished"));
    m_restart_check->SetValue(true);
    m_restart_check->SetToolTip(_L("config.g is only read when the printer starts up, so a restart is "
                                   "what makes a changed configuration take effect."));
    main->Add(m_restart_check, 0, wxLEFT | wxRIGHT | wxTOP, pad / 2);

    m_gauge = new wxGauge(this, wxID_ANY, 100, wxDefaultPosition, wxSize(-1, FromDIP(10)));
    main->Add(m_gauge, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(16));

    m_status = new wxStaticText(this, wxID_ANY, _L("Contacting the printer..."));
    main->Add(m_status, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(16));

    auto *buttons = new wxBoxSizer(wxHORIZONTAL);
    m_folder_btn  = new wxButton(this, wxID_ANY, _L("From Folder..."));
    m_zip_btn     = new wxButton(this, wxID_ANY, _L("From Zip..."));
    m_install_btn = new wxButton(this, wxID_ANY, _L("Install"));
    m_close_btn   = new wxButton(this, wxID_CANCEL, _L("Close"));
    m_install_btn->Enable(false);
    buttons->Add(m_folder_btn, 0, wxRIGHT, pad);
    buttons->Add(m_zip_btn, 0, wxRIGHT, pad);
    buttons->AddStretchSpacer();
    buttons->Add(m_install_btn, 0, wxRIGHT, pad);
    buttons->Add(m_close_btn, 0);
    main->Add(buttons, 0, wxEXPAND | wxALL, FromDIP(16));

    m_folder_btn->Bind(wxEVT_BUTTON, &SysFilesUpdateDialog::on_choose_folder, this);
    m_zip_btn->Bind(wxEVT_BUTTON, &SysFilesUpdateDialog::on_choose_zip, this);
    m_all_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) {
        for (unsigned i = 0; i < m_files->GetCount(); ++i)
            m_files->Check(i, true);
        save_selection();
    });
    m_none_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) {
        for (unsigned i = 0; i < m_files->GetCount(); ++i)
            m_files->Check(i, false);
        save_selection();
    });
    // Ticking a box is the whole gesture - there is no Apply - so it is saved as it happens.
    m_files->Bind(wxEVT_CHECKLISTBOX, [this](wxCommandEvent &evt) {
        evt.Skip();
        save_selection();
    });
    m_install_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) {
        // A hand-picked folder or zip states no version, and pressing Reinstall is a deliberate
        // choice to write files the printer already claims to have. Both bypass the version check.
        start_install(/* force */ !m_local_path.empty() || m_same_version);
    });
    // Must be EndModal, not Close(): this dialog is run with ShowModal(), and Close() does not
    // break out of the modal event loop, so the window appears frozen shut.
    m_close_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { dismiss(); });

    SetSizer(main);
    Layout();
    Fit();
    SetMinSize(GetSize());
    CentreOnParent();
}

SysFilesUpdateJob::SourceFactory SysFilesUpdateDialog::make_source_factory() const
{
    const std::string local = m_local_path;
    // Read on the UI thread and captured by value: the factory runs on the job's worker, where
    // touching wxGetApp() is not safe.
    const std::string api_base = wxGetApp().app_config->github_api_base();
    // The configuration feed needs the customer's Unlayered account sign-in, which identifies who
    // is asking. Read here on the UI thread with api_base, for the same reason: the factory below
    // runs on the job's worker thread, where wxGetApp() is not safe to touch. Empty is normal
    // (signed out) and simply sends no Authorization header.
    //
    // The credential is bound to the Unlayered site and the source attaches it per request, only
    // to URLs on that site - so "github_api_base" pointed at GitHub, or a release asset hosted
    // elsewhere, never receives it.
    const FeedCredential credential = printer_feed_credential();
    return [local, api_base, credential]() -> std::unique_ptr<SysFileSource> {
        if (!local.empty())
            return std::make_unique<LocalSysFileSource>(local);
        GitHubSysFileSource::Options opts;
        opts.api_base   = api_base;
        opts.credential = credential;
        return std::make_unique<GitHubSysFileSource>(std::move(opts));
    };
}

void SysFilesUpdateDialog::start_query()
{
    set_phase(Phase::Querying);
    m_job_result = std::make_shared<SysFilesUpdateJob::Result>();

    SysFilesUpdateJob::Options opts;
    opts.query_only = true;

    auto job = std::make_unique<SysFilesUpdateJob>(m_connection->make_client_factory(), make_source_factory(), std::move(opts));
    job->set_event_handle(this);
    job->set_result_slot(m_job_result);
    job->set_ui_alive(m_ui_alive);
    replace_job(*m_worker, std::move(job));
}

void SysFilesUpdateDialog::start_install(bool force)
{
    if (!m_connection->has_connection()) {
        show_error(this, _L("Enter the printer's address, or connect it to this computer with a USB cable."));
        return;
    }

    const std::vector<std::string> selected = checked_files();
    if (selected.empty()) {
        show_error(this, _L("Tick at least one file to write."));
        return;
    }

    // Last chance to back out: this is the point after which the printer's configuration is
    // rewritten, and there is no undo on the card.
    wxString body = wxString::Format(
        _L("%d file(s) will be written to the printer's SD card, replacing what is there now."),
        int(selected.size()));
    if (m_job_result && m_job_result->from_full_sd_card)
        body += "\n\n" + full_sd_card_warning();
    if (m_backup_check->GetValue())
        body += "\n\n" + _L("The files being replaced will be saved to this computer first.");
    else
        body += "\n\n" + _L("The files being replaced will NOT be saved anywhere.");
    if (m_restart_check->GetValue())
        body += "\n\n" + _L("The printer will then restart and be unavailable for a minute.");
    body += "\n\n" + _L("Continue?");

    MessageDialog confirm(this, body, _L("Update Printer Configuration"), wxYES_NO | wxNO_DEFAULT | wxICON_WARNING);
    if (confirm.ShowModal() != wxID_YES)
        return;

    set_phase(Phase::Working);
    m_job_result = std::make_shared<SysFilesUpdateJob::Result>();

    SysFilesUpdateJob::Options opts;
    opts.selected = selected;
    opts.backup   = m_backup_check->GetValue();
    opts.restart  = m_restart_check->GetValue();
    opts.force    = force;

    auto job = std::make_unique<SysFilesUpdateJob>(m_connection->make_client_factory(), make_source_factory(), std::move(opts));
    job->set_event_handle(this);
    job->set_result_slot(m_job_result);
    job->set_ui_alive(m_ui_alive);
    replace_job(*m_worker, std::move(job));
}

// Backing a file up means reading it off the card first, and not every connection can do that.
// A backup that silently differs from what was on the printer would be worse than none, so when
// the transport cannot read files the option is disabled and says why.
void SysFilesUpdateDialog::update_backup_availability()
{
    const DuetClient::Capabilities caps = m_connection->selected_capabilities();
    m_backup_available = caps.read_files;
    if (!m_backup_available) {
        m_backup_check->SetValue(false);
        m_backup_check->SetToolTip(_L("Backups need the printer to send its files back. This "
                                      "connection cannot, so the files will be replaced without a copy being kept."));
    } else {
        m_backup_check->SetToolTip(wxString());
    }
    m_backup_check->Enable(m_backup_available && m_phase != Phase::Working && m_phase != Phase::Querying);
}

void SysFilesUpdateDialog::set_phase(Phase phase)
{
    m_phase = phase;
    const bool busy = (phase == Phase::Querying || phase == Phase::Working);

    m_install_btn->Enable(!busy && m_files->GetCount() > 0);
    m_folder_btn->Enable(!busy);
    m_zip_btn->Enable(!busy);
    m_all_btn->Enable(!busy);
    m_none_btn->Enable(!busy);
    m_files->Enable(!busy);
    m_backup_check->Enable(!busy && m_backup_available);
    if (m_connection)
        m_connection->set_enabled(!busy);
    m_restart_check->Enable(!busy);
    // Closing mid-write would not stop the board, so only offer it when nothing is in flight.
    m_close_btn->SetLabel(busy ? _L("Cancel") : _L("Close"));
    if (phase == Phase::Querying)
        m_gauge->Pulse();
    Layout();
}

std::vector<std::string> SysFilesUpdateDialog::checked_files() const
{
    std::vector<std::string> out;
    for (unsigned i = 0; i < m_files->GetCount() && i < m_file_paths.size(); ++i)
        if (m_files->IsChecked(i))
            out.push_back(m_file_paths[i]);
    return out;
}

// What identifies this printer for the purpose of remembering its file selection.
//
// The board's own unique id, which is the only thing here that is genuinely per-machine and
// survives everything that matters: renaming the printer, changing its address, and switching
// between the network and USB. Deliberately not the connection - an address changes, and a COM
// port is not an identity at all, since it moves with the USB socket and changes again when a
// firmware update alters how many ports the board presents.
//
// network.name is only a fallback, because it is not necessarily unique: two machines on this
// fleet answer to "Unlayered 5X", and they would otherwise share one remembered selection.
std::string SysFilesUpdateDialog::selection_key_for(const SysFilesUpdateJob::Result &result) const
{
    std::string key;
    if (result.board_known) {
        if (!result.board.unique_id.empty())
            key = result.board.unique_id;
        else if (!result.board.machine_name.empty())
            key = result.board.machine_name;
    }
    if (key.empty())
        key = m_host;

    // The store is one line per printer, 'key = a;b;c', so neither separator may appear in a key.
    for (char &c : key)
        if (c == '=' || c == ';' || c == '\n' || c == '\r')
            c = '_';
    return key;
}

void SysFilesUpdateDialog::load_selection()
{
    if (m_selection_key.empty())
        return;
    const std::string stored = wxGetApp().app_config->get(SYS_FILES_SKIP_SECTION, m_selection_key);
    if (stored.empty())
        return;

    std::vector<std::string> skip;
    boost::split(skip, stored, boost::is_any_of(";"), boost::token_compress_on);
    for (unsigned i = 0; i < m_files->GetCount() && i < m_file_paths.size(); ++i)
        for (const std::string &s : skip)
            if (!s.empty() && boost::iequals(s, m_file_paths[i])) {
                m_files->Check(i, false);
                break;
            }
}

void SysFilesUpdateDialog::save_selection() const
{
    if (m_selection_key.empty())
        return;
    std::string skip;
    for (unsigned i = 0; i < m_files->GetCount() && i < m_file_paths.size(); ++i)
        if (!m_files->IsChecked(i)) {
            if (!skip.empty())
                skip += ";";
            skip += m_file_paths[i];
        }
    if (skip.empty())
        wxGetApp().app_config->erase(SYS_FILES_SKIP_SECTION, m_selection_key);
    else
        wxGetApp().app_config->set(SYS_FILES_SKIP_SECTION, m_selection_key, skip);
}

void SysFilesUpdateDialog::populate_files(const SysFilesUpdateJob::Result &result)
{
    m_files->Clear();
    m_file_paths.clear();

    for (const auto &f : result.available_files) {
        m_file_paths.push_back(f.path);
        m_files->Append(wxString::Format("%s  (%s)", wxString::FromUTF8(f.path.c_str()), format_size(f.size)));
        m_files->Check(m_files->GetCount() - 1, true);
    }

    // Everything is ticked above, then the remembered exclusions are unticked. Doing it in that
    // order is what makes a newly added file default to being installed.
    m_selection_key = selection_key_for(result);
    load_selection();

    // Anything the package carried but will not install is called out rather than dropped in
    // silence: "it did not copy my file" is otherwise impossible to explain.
    if (!result.skipped.empty()) {
        wxString text = m_notes->GetValue();
        if (!text.IsEmpty())
            text += "\n\n";
        text += _L("Left out:");
        for (const auto &s : result.skipped)
            text += wxString::Format("\n  %s - %s", wxString::FromUTF8(s.first.c_str()),
                                     wxString::FromUTF8(s.second.c_str()));
        m_notes->SetValue(text);
    }
}

void SysFilesUpdateDialog::on_choose_folder(wxCommandEvent &)
{
    wxDirDialog dlg(this, _L("Select a folder of configuration files"), wxEmptyString,
                    wxDD_DEFAULT_STYLE | wxDD_DIR_MUST_EXIST);
    if (dlg.ShowModal() != wxID_OK)
        return;

    m_local_path = dlg.GetPath().ToUTF8().data();
    // Re-query so the file list, and everything else the dialog shows, describes what was picked.
    start_query();
}

void SysFilesUpdateDialog::on_choose_zip(wxCommandEvent &)
{
    wxFileDialog dlg(this, _L("Select a configuration package"), wxEmptyString, wxEmptyString,
                     _L("Zip archives") + " (*.zip)|*.zip|" + _L("All files") + " (*.*)|*.*",
                     wxFD_OPEN | wxFD_FILE_MUST_EXIST);
    if (dlg.ShowModal() != wxID_OK)
        return;

    m_local_path = dlg.GetPath().ToUTF8().data();
    start_query();
}

void SysFilesUpdateDialog::on_job_message(wxCommandEvent &evt)
{
    if (m_status)
        m_status->SetLabel(evt.GetString());
    if (m_gauge && m_phase != Phase::Querying)
        m_gauge->SetValue(std::min(evt.GetInt(), 100));
}

void SysFilesUpdateDialog::on_job_finished(wxCommandEvent &evt)
{
    const auto     outcome   = static_cast<SysFilesUpdateJob::Outcome>(evt.GetInt());
    const wxString msg       = evt.GetString();
    // A failed opening query belongs in the status line. Stacking a modal error box on a dialog
    // that has only just opened is noise, and it hides the buttons that could fix the problem.
    const bool     was_query = (m_phase == Phase::Querying);

    if (m_job_result && m_job_result->board_known) {
        const auto &board = m_job_result->board;
        // Every Unlayered machine is the same board, so the M550 name is what identifies which
        // printer this is; fall back to the board's own name when it has not been set.
        const std::string who = board.machine_name.empty() ? board.name : board.machine_name;
        m_printer_label->SetLabel(wxString::Format(_L("%s at %s"), wxString::FromUTF8(who.c_str()),
                                                   wxString::FromUTF8(m_host.c_str())));
    }

    if (m_job_result) {
        const auto &inst = m_job_result->installed;
        if (inst.known) {
            wxString line = wxString::FromUTF8(inst.version.c_str());
            if (line.IsEmpty())
                line = _L("unrecorded version");
            if (!inst.installed_at.empty())
                line += wxString::Format(_L(", installed %s"), wxString::FromUTF8(inst.installed_at.c_str()));
            m_installed_label->SetLabel(wxString::Format(_L("On the printer: %s"), line));
        } else {
            m_installed_label->SetLabel(_L("On the printer: no configuration installed by this slicer"));
        }
    }

    switch (outcome) {
    case SysFilesUpdateJob::Outcome::QueryOk: {
        m_gauge->SetValue(0);
        if (!m_job_result) {
            set_phase(Phase::Ready);
            break;
        }

        wxString available = wxString::FromUTF8(m_job_result->available_version.c_str());
        if (available.IsEmpty())
            available = _L("(no version declared)");
        if (!m_job_result->available_date.empty())
            available += wxString::Format(" (%s)", wxString::FromUTF8(m_job_result->available_date.c_str()));
        // Never enforced (see SysFilePackage::machine), so at least said before the install.
        if (!m_job_result->available_machine.empty())
            available += wxString::Format(_L(" for %s"), wxString::FromUTF8(m_job_result->available_machine.c_str()));
        if (!m_job_result->available_origin.empty())
            available += wxString::Format(" - %s", wxString::FromUTF8(m_job_result->available_origin.c_str()));
        m_available_label->SetLabel(wxString::Format(_L("Available: %s"), available));

        {
            wxString notes = wxString::FromUTF8(m_job_result->available_notes.c_str());
            if (m_job_result->from_full_sd_card)
                notes = full_sd_card_warning() + (notes.IsEmpty() ? wxString() : "\n\n" + notes);
            m_notes->SetValue(notes);
        }
        populate_files(*m_job_result);
        set_phase(Phase::Ready);

        const bool same = !m_job_result->available_version.empty() &&
                          m_job_result->installed.known &&
                          m_job_result->available_version == m_job_result->installed.version;
        m_same_version = same;
        if (same) {
            m_install_btn->SetLabel(_L("Reinstall"));
            m_status->SetLabel(_L("The printer already has these files."));
        } else {
            m_install_btn->SetLabel(_L("Install"));
            m_status->SetLabel(wxString::Format(_L("%d file(s) ready to write."),
                                                int(m_job_result->available_files.size())));
        }

        // Said up front rather than only when Install fails. Install stays enabled: the job
        // judges the ticked files, so unticking the file that needs the newer firmware still works.
        if (m_job_result->firmware_too_old) {
            const wxString need    = wxString::FromUTF8(m_job_result->required_firmware.c_str());
            const wxString running = wxString::FromUTF8(m_job_result->board.firmware_version.c_str());
            m_status->SetLabel(wxString::Format(_L("Update the printer's firmware first: these files need %s or newer."), need));
            wxString text = m_notes->GetValue();
            if (!text.IsEmpty())
                text += "\n\n";
            text += wxString::Format(_L("FIRMWARE TOO OLD: these files need printer firmware %s or newer (%s), and the "
                                        "printer is running %s. Installing them now would stop the printer's startup "
                                        "file part way, leaving it with no heaters or tool. Use the Firmware button "
                                        "first."),
                                     need, wxString::FromUTF8(m_job_result->required_firmware_reason.c_str()), running);
            m_notes->SetValue(text);
        }

        // Opened from "Printer update available": go straight to the install's own confirmation,
        // once, when there is something new that this firmware can run. Otherwise the dialog just
        // shows why not, as it would from the Config button.
        if (m_begin_when_ready) {
            m_begin_when_ready = false;
            if (!same && !m_job_result->firmware_too_old && !m_file_paths.empty() && m_local_path.empty())
                CallAfter([this]() {
                    if (m_phase == Phase::Ready)
                        start_install(/* force */ false);
                });
        }
        break;
    }

    case SysFilesUpdateJob::Outcome::UpToDate:
        m_gauge->SetValue(100);
        set_phase(Phase::Finished);
        // Still allow a deliberate rewrite of the same version; the next press forces it through.
        m_same_version = true;
        m_install_btn->SetLabel(_L("Reinstall"));
        m_install_btn->Enable(true);
        m_status->SetLabel(msg);
        break;

    case SysFilesUpdateJob::Outcome::Success: {
        m_gauge->SetValue(100);
        set_phase(Phase::Finished);
        m_status->SetLabel(msg);
        wxString full = msg;
        if (m_job_result && !m_job_result->restarted && !m_restart_check->GetValue())
            full += "\n\n" + _L("The printer is still running its previous configuration until it restarts.");
        show_info(this, full, _L("Configuration updated"));
        // What is on the printer has changed; refresh the dialog from the machine itself.
        start_query();
        break;
    }

    case SysFilesUpdateJob::Outcome::Cancelled:
        set_phase(Phase::Ready);
        m_status->SetLabel(msg.IsEmpty() ? _L("Cancelled") : msg);
        break;

    case SysFilesUpdateJob::Outcome::Failed:
    default: {
        set_phase(Phase::Ready);
        m_status->SetLabel(msg);
        if (was_query) {
            // Nothing was changed on the printer. The most likely cause is that no release exists
            // yet, so point at the buttons that do work.
            m_gauge->SetValue(0);
            m_files->Clear();
            m_file_paths.clear();
            m_install_btn->Enable(false);
            m_available_label->SetLabel(_L("Available: nothing found"));
            m_notes->SetValue(msg + "\n\n" +
                              _L("Choose a folder or a zip of configuration files to install instead."));
            Layout();
            break;
        }
        wxString full = msg;
        if (m_job_result && !m_job_result->backup_dir.empty())
            full += "\n\n" + wxString::Format(_L("The printer's previous files were saved to %s."),
                                              wxString::FromUTF8(m_job_result->backup_dir.c_str()));
        show_error(this, full);
        break;
    }
    }
    Layout();
}

// Shuts the background job down and leaves the modal loop. Safe to call more than once.
void SysFilesUpdateDialog::dismiss()
{
    // Stop the job from touching this window: it posts progress events here, and the worker cannot
    // be interrupted mid-request, so it may still be alive after we are gone.
    if (m_ui_alive)
        *m_ui_alive = false;

    if (m_worker) {
        m_worker->cancel_all();
        // Bounded, so a hung socket cannot wedge the close; the worker's destructor waits again
        // before joining.
        m_worker->wait_for_idle(1500);
    }

    if (IsModal())
        EndModal(wxID_CANCEL);
    else
        Destroy();
}

void SysFilesUpdateDialog::on_close(wxCloseEvent &evt)
{
    // The window-manager close button and Escape both land here.
    if (m_ui_alive)
        *m_ui_alive = false;
    if (m_worker)
        m_worker->cancel_all();

    if (IsModal()) {
        EndModal(wxID_CANCEL);
        return; // Do not Skip(): the modal loop is already unwinding.
    }
    evt.Skip();
}

void SysFilesUpdateDialog::on_dpi_changed(const wxRect &suggested_rect)
{
    Layout();
    Refresh();
}

}} // namespace Slic3r::GUI
