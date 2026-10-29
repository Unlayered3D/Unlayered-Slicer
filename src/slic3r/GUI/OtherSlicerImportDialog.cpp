#include "OtherSlicerImportDialog.hpp"

#include <algorithm>
#include <functional>
#include <map>
#include <set>

#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>

#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/timer.h>
#include <wx/utils.h>

#include "libslic3r_version.h"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Semver.hpp"
#include "libslic3r/Utils.hpp"
#include "GUI.hpp"
#include "GUI_App.hpp"
#include "I18N.hpp"
#include "MainFrame.hpp"
#include "slic3r/Utils/UsageTelemetry.hpp"

namespace Slic3r {
namespace GUI {

namespace fs = boost::filesystem;
using OtherSlicers::App;
using OtherSlicers::Kind;

namespace {

// App config: empty = never asked; a version = asked (or offered by the setup wizard) in that
// version; "never" = Don't ask again.
constexpr const char *ASKED_KEY = "import_other_slicers_asked";

// What was imported before, so importing again adds only what is new. Local only, never sent.
fs::path history_file() { return fs::path(data_dir()) / "other_slicers_import.json"; }

PresetCollection &collection_of(PresetBundle &bundle, Kind kind)
{
    if (kind == Kind::Printer)
        return bundle.printers;
    if (kind == Kind::Filament)
        return bundle.filaments;
    return bundle.prints;
}

// The printer models and system filaments this application ships (resources/profiles). Read once:
// the resources do not change while the application runs.
struct ShippedProfiles
{
    OtherSlicers::VendorModels         models;
    std::map<std::string, std::string> filaments; // name -> vendor
};

const ShippedProfiles &shipped_profiles()
{
    static const ShippedProfiles shipped = [] {
        ShippedProfiles s;
        const fs::path  dir = fs::path(resources_dir()) / "profiles";
        s.models            = OtherSlicers::read_vendor_models(dir);
        s.filaments         = OtherSlicers::read_vendor_filaments(dir);
        return s;
    }();
    return shipped;
}

// `with_shipped`: a system preset the application ships but has not installed counts as available
// (it is installed before the import, as File > Import does).
OtherSlicers::Target make_target(PresetBundle &bundle, bool with_shipped)
{
    OtherSlicers::Target t;
    t.name_taken = [&bundle](Kind kind, const std::string &name) { return collection_of(bundle, kind).find_preset(name, false) != nullptr; };
    t.has_system = [&bundle, with_shipped](Kind kind, const std::string &name) {
        if (const Preset *p = collection_of(bundle, kind).find_preset2(name, false); p != nullptr)
            return p->is_system;
        if (!with_shipped)
            return false;
        if (kind == Kind::Filament && shipped_profiles().filaments.count(name) > 0)
            return true;
        return !PresetBundle::find_resource_vendor_for_preset(OtherSlicers::kind_dir(kind), name).empty();
    };
    t.user_config = [&bundle](Kind kind, const std::string &name) -> const DynamicPrintConfig * {
        const Preset *p = collection_of(bundle, kind).find_preset(name, false);
        return p != nullptr && p->is_user() ? &p->config : nullptr;
    };
    // The printers and filaments picked from the other apps' built-in lists.
    t.shipped_models  = shipped_profiles().models;
    t.filament_vendor = [](const std::string &name) {
        const auto &filaments = shipped_profiles().filaments;
        auto        it        = filaments.find(name);
        return it == filaments.end() ? std::string() : it->second;
    };
    if (AppConfig *config = wxGetApp().app_config; config != nullptr) {
        t.enabled_models   = config->vendors();
        t.filament_enabled = [config](const std::string &name) {
            return config->has_section(AppConfig::SECTION_FILAMENTS) && config->get_section(AppConfig::SECTION_FILAMENTS).count(name) > 0;
        };
    }
    // Only the default printer so far: a fresh install, whose selection may follow the other app's.
    t.fresh = bundle.printers.only_default_printers();
    return t;
}

wxString app_label(App app) { return from_u8(OtherSlicers::app_name(app)); }

bool modal_dialog_open()
{
    for (wxWindowList::compatibility_iterator node = wxTopLevelWindows.GetFirst(); node; node = node->GetNext())
        if (auto *dlg = dynamic_cast<wxDialog *>(node->GetData()); dlg != nullptr && dlg->IsShown() && dlg->IsModal())
            return true;
    return false;
}

// Runs `fn` once the app has started and no other dialog (setup wizard, What's new, the privacy
// question, a plug-in download) is open, so two questions never stack. Gives up after 10 minutes.
void when_no_dialog(GUI_App &app, std::function<void()> fn, int attempt = 0)
{
    auto *timer = new wxTimer();
    timer->Bind(wxEVT_TIMER, [&app, fn, attempt, timer](wxTimerEvent &) {
        timer->Stop();
        if (!app.is_closing() && app.mainframe != nullptr) {
            if (!app.is_running_wizard() && !modal_dialog_open())
                fn(); // at once, so a question waiting on its own timer sees this one open and waits
            else if (attempt < 300)
                when_no_dialog(app, fn, attempt + 1);
        }
        // Deleted only after fn returns: fn shows a modal dialog, whose event loop runs queued calls,
        // and deleting the timer destroys this handler -- and the copy of fn it holds -- while fn
        // still runs (the captures read after ShowModal() returned were freed memory: a crash).
        app.CallAfter([timer] { delete timer; });
    });
    timer->StartOnce(attempt == 0 ? 1500 : 2000);
}

// "We found settings from OrcaSlicer, Bambu Studio. Import them?" Import / Not now / Don't ask again.
class AskImportDialog : public DPIDialog
{
public:
    AskImportDialog(wxWindow *parent, const OtherSlicersFound &found)
        : DPIDialog(parent, wxID_ANY, _L("Import your settings from other slicers"), wxDefaultPosition, wxDefaultSize,
                    wxCAPTION | wxCLOSE_BOX | wxSYSTEM_MENU)
    {
        SetBackgroundColour(*wxWHITE);
        const int wrap = FromDIP(420);
        auto     *top  = new wxBoxSizer(wxVERTICAL);
        auto     *head = new wxStaticText(this, wxID_ANY, wxString::Format(_L("We found settings from %s. Import them?"), found.apps()));
        head->SetFont(head->GetFont().Bold());
        head->Wrap(wrap);
        top->Add(head, 0, wxLEFT | wxRIGHT | wxTOP, FromDIP(16));
        for (size_t i = 0; i < found.scans.size(); ++i)
            if (found.counts[i].total() > 0) {
                auto *line = new wxStaticText(this, wxID_ANY,
                                              app_label(found.scans[i].location.app) + ": " + OtherSlicersFound::describe(found.counts[i]));
                top->AddSpacer(FromDIP(6));
                top->Add(line, 0, wxLEFT | wxRIGHT, FromDIP(28));
            }
        auto *body = new wxStaticText(this, wxID_ANY,
                                      _L("Your printers, filaments and presets are copied into Unlayered Slicer. Nothing in the other "
                                         "apps is changed. You can also do this later from File > Import."));
        body->Wrap(wrap);
        top->Add(body, 0, wxALL, FromDIP(16));

        auto *buttons = new wxBoxSizer(wxHORIZONTAL);
        auto *never   = new wxButton(this, wxID_NO, _L("Don't ask again"));
        auto *later   = new wxButton(this, wxID_CANCEL, _L("Not now"));
        auto *yes     = new wxButton(this, wxID_YES, _L("Import"));
        buttons->Add(never, 0);
        buttons->AddStretchSpacer();
        buttons->Add(later, 0, wxRIGHT, FromDIP(8));
        buttons->Add(yes, 0);
        top->Add(buttons, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(16));
        for (wxButton *b : {never, later, yes})
            b->Bind(wxEVT_BUTTON, [this, b](wxCommandEvent &) { EndModal(b->GetId()); });
        yes->SetDefault();
        SetEscapeId(wxID_CANCEL);
        SetSizerAndFit(top);
        CenterOnParent();
        wxGetApp().UpdateDlgDarkUI(this);
    }

protected:
    void on_dpi_changed(const wxRect &) override {}
};

// What one app contributed, for the summary.
struct AppReport
{
    OtherSlicers::Counts                          imported;
    std::set<std::pair<std::string, std::string>> models; // (vendor, model) enabled
    wxArrayString                                 skipped, renamed, keys, notes;
};

wxString join(const wxArrayString &items, const wxString &separator)
{
    wxString out;
    for (const wxString &s : items)
        out += (out.empty() ? wxString() : separator) + s;
    return out;
}

// The built-in printer models, filaments and (on a fresh install) the current selection of the
// planned apps, enabled the way the setup wizard and install_preset_parents_from_resources() do it:
// in the app config, the vendor profiles installed from the application's resources, and the
// presets loaded again. True when anything changed.
bool apply_system_part(wxWindow *parent, const OtherSlicers::Plan &plan, OtherSlicers::ImportHistory &history)
{
    GUI_App      &app    = wxGetApp();
    PresetBundle &bundle = *app.preset_bundle;
    AppConfig    &cfg    = *app.app_config;
    if (plan.models.empty() && plan.filaments.empty() && !plan.select)
        return false;

    std::vector<std::string> to_install;
    auto need_vendor = [&](const std::string &vendor) {
        if (!vendor.empty() && bundle.vendors.find(vendor) == bundle.vendors.end() &&
            std::find(to_install.begin(), to_install.end(), vendor) == to_install.end())
            to_install.push_back(vendor);
    };
    for (const OtherSlicers::EnableModel &m : plan.models) {
        cfg.set_variant(m.vendor, m.model, m.nozzle, true);
        need_vendor(m.vendor);
        history.add_system(m.app, Kind::Printer, m.vendor + ";" + m.model + ";" + m.nozzle);
    }
    for (const OtherSlicers::EnableFilament &f : plan.filaments) {
        cfg.set(AppConfig::SECTION_FILAMENTS, f.name, std::string("true"));
        need_vendor(f.vendor);
        history.add_system(f.app, Kind::Filament, f.name);
    }
    // A fresh install only (plan() checked Target::fresh): the printer, its process and filament
    // load_selections() picks up from the app config.
    if (plan.select) {
        cfg.set("presets", PRESET_PRINTER_NAME, plan.select->machine);
        if (!plan.select->process.empty())
            cfg.set_printer_setting(plan.select->machine, PRESET_PRINT_NAME, plan.select->process);
        if (!plan.select->filament.empty())
            cfg.set_printer_setting(plan.select->machine, PRESET_FILAMENT_NAME, plan.select->filament);
    }
    if (!to_install.empty() && !Slic3r::install_vendor_bundles_from_resources(to_install))
        // Not fatal: whatever did install still loads, and the rest is reported as not loaded.
        show_error(parent, _L("Failed to install the printer profiles from the application resources."));
    cfg.save();
    bundle.load_presets(cfg, ForwardCompatibilitySubstitutionRule::EnableSilent);
    return true;
}

} // namespace

int OtherSlicersFound::total() const
{
    int n = 0;
    for (const OtherSlicers::Counts &c : counts)
        n += c.total();
    return n;
}

wxString OtherSlicersFound::apps() const
{
    wxString out;
    for (size_t i = 0; i < scans.size(); ++i)
        if (counts[i].total() > 0)
            out += (out.empty() ? "" : ", ") + app_label(scans[i].location.app);
    return out;
}

wxString OtherSlicersFound::bambu_printers_in() const
{
    // Bambu Studio first: it is where Bambu Lab printers usually live.
    for (App app : {App::BambuStudio, App::OrcaSlicer})
        for (const OtherSlicers::Scan &scan : scans)
            if (scan.location.app == app && scan.selection.has_vendor("BBL"))
                return app_label(app);
    return {};
}

wxString OtherSlicersFound::describe(const OtherSlicers::Counts &c)
{
    wxString out;
    auto     part = [&out](const wxString &s) { out += (out.empty() ? "" : ", ") + s; };
    if (c.models > 0)
        part(wxString::Format(_L_PLURAL("%d printer", "%d printers", c.models), c.models));
    if (c.system_filaments > 0)
        part(wxString::Format(_L_PLURAL("%d filament", "%d filaments", c.system_filaments), c.system_filaments));
    if (c.user_presets() > 0)
        part(wxString::Format(_L_PLURAL("%d custom preset", "%d custom presets", c.user_presets()), c.user_presets()));
    return out;
}

OtherSlicersFound find_other_slicers()
{
    OtherSlicersFound found;
    PresetBundle     *bundle = wxGetApp().preset_bundle;
    if (bundle == nullptr)
        return found;
    OtherSlicers::ImportHistory history;
    history.load(history_file());
    // With the shipped profiles: a printer whose vendor is not installed yet is installed by the import.
    const OtherSlicers::Target target = make_target(*bundle, true);
    for (const OtherSlicers::AppLocation &location : OtherSlicers::find(fs::path(data_dir()))) {
        found.scans.push_back(OtherSlicers::scan(location));
        found.counts.push_back(OtherSlicers::importable_counts(found.scans.back(), target, history));
        const OtherSlicers::Counts &c = found.counts.back();
        BOOST_LOG_TRIVIAL(info) << "other slicers: " << OtherSlicers::app_name(location.app) << " has " << c.user_presets()
                                << " presets, " << c.models << " printers and " << c.system_filaments << " filaments not imported yet";
    }
    return found;
}

OtherSlicersImportResult import_other_slicers(wxWindow *parent, const std::vector<OtherSlicers::Scan> &selected)
{
    OtherSlicersImportResult result;
    if (selected.empty())
        return result;
    GUI_App      &app    = wxGetApp();
    PresetBundle &bundle = *app.preset_bundle;
    wxBusyCursor  busy;

    OtherSlicers::ImportHistory history;
    history.load(history_file());

    // 1. What the person picked from the apps' built-in lists, and the vendor profiles the user
    //    presets can stay linked to: planned against what the application ships.
    const OtherSlicers::Plan first = OtherSlicers::plan(selected, make_target(bundle, true), history);
    const bool system_changed      = apply_system_part(parent, first, history);
    std::vector<std::pair<std::string, std::string>> parents;
    for (const OtherSlicers::Planned &item : first.items)
        if (!item.target_inherits.empty())
            parents.emplace_back(OtherSlicers::kind_dir(item.source.kind), item.target_inherits);
    if (!parents.empty())
        install_preset_parents_from_resources(parent, parents, false);

    // 2. The person's own presets, planned again against what is loaded now.
    const OtherSlicers::Plan plan = OtherSlicers::plan(selected, make_target(bundle, false), history, false);

    const fs::path            staging = fs::path(data_dir()) / "other_slicers_import.tmp";
    boost::system::error_code ec;
    fs::remove_all(staging, ec);
    std::vector<std::string>   files = OtherSlicers::write(plan, staging);
    PresetBundle::ImportReport report;
    if (!files.empty())
        // Names were chosen so nothing is taken; should one be anyway, never overwrite it ("No to all").
        bundle.import_presets(files, [](const std::string &) { return 2; }, ForwardCompatibilitySubstitutionRule::Enable, *app.app_config,
                              &report);
    fs::remove_all(staging, ec);

    std::map<App, AppReport> per_app;
    for (const OtherSlicers::Scan &scan : selected)
        per_app[scan.location.app]; // every app chosen gets a line, even with nothing new

    for (const OtherSlicers::EnableModel &m : first.models)
        per_app[m.app].models.insert({m.vendor, m.model});
    for (auto &[a, r] : per_app)
        r.imported.models = int(r.models.size());
    for (const OtherSlicers::EnableFilament &f : first.filaments)
        ++per_app[f.app].imported.system_filaments;
    result.printers  = int(first.models.size());
    result.filaments = int(first.filaments.size());

    const std::set<std::string>        imported(report.imported.begin(), report.imported.end());
    std::map<std::string, std::string> missing(report.missing_parents.begin(), report.missing_parents.end());
    int                                dropped_settings = 0;
    for (const OtherSlicers::Planned &item : plan.items) {
        AppReport     &r    = per_app[item.source.app];
        const wxString name = from_u8(item.source.name);
        if (imported.count(item.target_name) > 0) {
            history.add(item.source.app, item.source.kind, item.source.name, item.target_name);
            const Kind k = item.source.kind;
            (k == Kind::Printer ? r.imported.printers : k == Kind::Filament ? r.imported.filaments : r.imported.processes) += 1;
            ++result.presets;
            if (item.target_name != item.source.name)
                r.renamed.Add(wxString::Format(_L("%s is called \"%s\" here"), name, from_u8(item.target_name)));
            if (item.dropped_connection)
                r.keys.Add(wxString::Format(_L("Enter the API key for %s in Printer settings > Connection."), from_u8(item.target_name)));
            dropped_settings += item.dropped_keys;
        } else if (auto it = missing.find(item.target_name); it != missing.end())
            r.skipped.Add(wxString::Format(_L("%s (based on \"%s\", which is not installed)"), name, from_u8(it->second)));
        else
            r.skipped.Add(wxString::Format(_L("%s (could not be loaded)"), name));
    }

    int before = 0, same = 0;
    auto report_skip = [&](const OtherSlicers::Skipped &s) {
        AppReport     &r    = per_app[s.app];
        const wxString name = from_u8(s.name);
        switch (s.why) {
        case OtherSlicers::Skipped::Why::AlreadyImported: ++before; break;
        case OtherSlicers::Skipped::Why::AlreadyPresent:
            ++same;
            history.add(s.app, s.kind, s.name, s.detail);
            break;
        case OtherSlicers::Skipped::Why::MissingParent:
            r.skipped.Add(wxString::Format(_L("%s (based on \"%s\", which neither %s nor Unlayered Slicer has)"), name, from_u8(s.detail),
                                           app_label(s.app)));
            break;
        case OtherSlicers::Skipped::Why::Duplicate: r.skipped.Add(wxString::Format(_L("%s (no free name for it)"), name)); break;
        case OtherSlicers::Skipped::Why::NotShipped:
            if (!s.detail.empty())
                r.skipped.Add(wxString::Format(_L("%s (made for %s printers, and none of them is selected)"), name, from_u8(s.detail)));
            else
                r.skipped.Add(wxString::Format(_L("%s (not available in Unlayered Slicer yet)"), name));
            break;
        case OtherSlicers::Skipped::Why::NoPrinter:
            r.skipped.Add(s.detail.empty() ?
                              wxString::Format(_L("printer connection %s (it names no printer profile)"), name) :
                              wxString::Format(_L("printer connection %s (its printer profile \"%s\" is not one of your own, so the address has nowhere to go)"),
                                               name, from_u8(s.detail)));
            break;
        }
        BOOST_LOG_TRIVIAL(info) << "other slicers: skipped " << OtherSlicers::app_name(s.app) << " " << OtherSlicers::kind_dir(s.kind) << " \""
                                << s.name << "\": " << s.reason;
    };
    // The built-in selections from the first plan (the second one has none); everything about the
    // person's own presets and connections from the second.
    for (const OtherSlicers::Skipped &s : first.skipped)
        if (s.why == OtherSlicers::Skipped::Why::NotShipped)
            report_skip(s);
    for (const OtherSlicers::Skipped &s : plan.skipped)
        report_skip(s);
    history.save(history_file());

    // Usage diagnostics, when the person shares them (count() checks): how many presets came from
    // which app. Never their names.
    for (const auto &[a, r] : per_app)
        if (r.imported.user_presets() > 0)
            UsageTelemetry::count("import:presets_" + OtherSlicers::app_key(a), r.imported.user_presets());

    if (system_changed || result.presets > 0) {
        app.load_current_presets();
        bundle.update_compatible(PresetSelectCompatibleType::Always);
        if (app.mainframe != nullptr)
            app.mainframe->update_side_preset_ui();
    }
    if (first.select) {
        const std::string now = bundle.printers.get_selected_preset_name();
        if (now == first.select->machine)
            result.selected_printer = from_u8(now);
        else
            BOOST_LOG_TRIVIAL(warning) << "other slicers: meant to select \"" << first.select->machine << "\", the selected printer is \"" << now << "\"";
    }

    // The summary: per app what came over, what did not and why, and what to set up again.
    for (const OtherSlicers::Scan &scan : selected)
        for (const auto &[why, n] : scan.not_importable) {
            AppReport &r = per_app[scan.location.app];
            if (why == "SLA")
                r.notes.Add(wxString::Format(_L_PLURAL("%d resin (SLA) preset is not imported; this slicer is for FDM printers.",
                                                       "%d resin (SLA) presets are not imported; this slicer is for FDM printers.", n), n));
            else if (why == "duplicates")
                r.notes.Add(wxString::Format(_L_PLURAL("%d preset is in more than one account folder; the newest copy was used.",
                                                       "%d presets are in more than one account folder; the newest copies were used.", n), n));
            else
                r.notes.Add(wxString::Format(_L_PLURAL("%d file could not be read.", "%d files could not be read.", n), n));
        }
    wxString text;
    for (const auto &[a, r] : per_app) {
        text += app_label(a) + "\n";
        const wxString what = OtherSlicersFound::describe(r.imported);
        text += "    " + (what.empty() ? _L("Imported: nothing new") : wxString::Format(_L("Imported: %s"), what)) + "\n";
        if (!r.skipped.empty())
            text += "    " + wxString::Format(_L("Skipped: %s"), join(r.skipped, "; ")) + "\n";
        for (const wxString &line : r.renamed)
            text += "    " + line + "\n";
        for (const wxString &line : r.keys)
            text += "    " + line + "\n";
        for (const wxString &line : r.notes)
            text += "    " + line + "\n";
        text += "\n";
    }
    if (!result.selected_printer.empty())
        text += wxString::Format(_L("Selected printer: %s"), result.selected_printer) + "\n";
    if (before > 0)
        text += wxString::Format(_L_PLURAL("%d preset was imported before and is still here.", "%d presets were imported before and are still here.",
                                           before), before) + "\n";
    if (same > 0)
        text += wxString::Format(_L_PLURAL("%d preset is already here with the same settings.", "%d presets are already here with the same settings.",
                                           same), same) + "\n";
    if (dropped_settings > 0)
        text += wxString::Format(_L_PLURAL("%d PrusaSlicer setting has no equivalent here and was left at its default.",
                                           "%d PrusaSlicer settings have no equivalent here and were left at their defaults.", dropped_settings),
                                 dropped_settings) + "\n";
    result.summary = text.Strip(wxString::both);
    BOOST_LOG_TRIVIAL(info) << "other slicers: imported " << result.presets << " presets, " << result.printers << " printer variants, "
                            << result.filaments << " filaments; imported before " << before << ", already here " << same;
    return result;
}

OtherSlicerImportDialog::OtherSlicerImportDialog(wxWindow *parent, OtherSlicersFound found, bool start_now)
    : DPIDialog(parent, wxID_ANY, _L("Import your settings from other slicers"), wxDefaultPosition, wxDefaultSize,
                wxCAPTION | wxCLOSE_BOX | wxSYSTEM_MENU | wxRESIZE_BORDER)
    , m_found(std::move(found))
{
    SetBackgroundColour(*wxWHITE);
    const int wrap = FromDIP(480);
    auto     *top  = new wxBoxSizer(wxVERTICAL);

    auto *intro = new wxStaticText(this, wxID_ANY,
                                   _L("Bring your printers, filaments and presets over from the other slicers on this computer. "
                                      "Nothing in those apps is changed, and nothing here is overwritten: a preset whose name is already "
                                      "taken gets the app's name added."));
    intro->Wrap(wrap);
    top->Add(intro, 0, wxALL, FromDIP(16));

    m_choice_sizer = new wxBoxSizer(wxVERTICAL);
    bool any       = false;
    for (size_t i = 0; i < m_found.scans.size(); ++i) {
        const App                   app = m_found.scans[i].location.app;
        const OtherSlicers::Counts &c   = m_found.counts[i];
        if (c.total() > 0) {
            auto *check = new wxCheckBox(this, wxID_ANY, app_label(app) + ": " + OtherSlicersFound::describe(c));
            check->SetValue(true);
            check->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent &) {
                bool on = false;
                for (wxCheckBox *cb : m_checks)
                    on |= cb != nullptr && cb->GetValue();
                m_import->Enable(on);
            });
            m_checks.push_back(check);
            m_choice_sizer->Add(check, 0, wxBOTTOM, FromDIP(8));
            any = true;
        } else {
            m_checks.push_back(nullptr);
            auto *none = new wxStaticText(this, wxID_ANY, wxString::Format(_L("%s: nothing new to import"), app_label(app)));
            none->SetForegroundColour(wxColour(128, 128, 128));
            m_choice_sizer->Add(none, 0, wxBOTTOM, FromDIP(8));
        }
    }
    if (m_found.scans.empty())
        m_choice_sizer->Add(new wxStaticText(this, wxID_ANY, _L("No other slicers were found on this computer.")));
    top->Add(m_choice_sizer, 0, wxLEFT | wxRIGHT, FromDIP(24));

    auto *note = new wxStaticText(this, wxID_ANY,
                                  _L("Printer network addresses are imported; API keys and passwords are not. Enter them again in "
                                     "each printer's connection settings."));
    note->Wrap(wrap);
    note->SetForegroundColour(wxColour(110, 110, 110));
    top->Add(note, 0, wxALL, FromDIP(16));

    m_summary = new wxTextCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(FromDIP(480), FromDIP(240)),
                               wxTE_MULTILINE | wxTE_READONLY | wxTE_DONTWRAP);
    m_summary->Hide();
    top->Add(m_summary, 1, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(16));

    auto *buttons = new wxBoxSizer(wxHORIZONTAL);
    m_close       = new wxButton(this, wxID_CANCEL, _L("Close"));
    m_import      = new wxButton(this, wxID_OK, _L("Import"));
    buttons->AddStretchSpacer();
    buttons->Add(m_close, 0, wxRIGHT, FromDIP(8));
    buttons->Add(m_import, 0);
    top->Add(buttons, 0, wxEXPAND | wxALL, FromDIP(16));
    m_import->Enable(any);
    m_import->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { do_import(); });
    m_close->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { EndModal(m_imported > 0 ? wxID_OK : wxID_CANCEL); });
    if (any)
        m_import->SetDefault();
    SetEscapeId(wxID_CANCEL);
    SetSizerAndFit(top);
    CenterOnParent();
    wxGetApp().UpdateDlgDarkUI(this);

    if (start_now && any)
        CallAfter([this] { do_import(); });
}

void OtherSlicerImportDialog::do_import()
{
    std::vector<OtherSlicers::Scan> selected;
    for (size_t i = 0; i < m_checks.size(); ++i)
        if (m_checks[i] != nullptr && m_checks[i]->GetValue())
            selected.push_back(m_found.scans[i]);
    if (selected.empty())
        return;
    m_import->Disable();
    for (wxCheckBox *cb : m_checks)
        if (cb != nullptr)
            cb->Disable();

    const OtherSlicersImportResult result = import_other_slicers(this, selected);
    m_imported                            = result.total();

    m_summary->SetValue(result.summary.empty() ? _L("Nothing was imported.") : result.summary);
    m_summary->Show();
    m_import->Hide();
    m_close->SetLabel(_L("Done"));
    m_close->SetDefault();
    m_close->SetFocus();
    Layout();
    Fit();
    CenterOnParent();
}

bool OtherSlicerImportDialog::run(wxWindow *parent, bool start_now)
{
    OtherSlicersFound found;
    {
        wxBusyCursor busy;
        found = find_other_slicers();
    }
    return run(parent, std::move(found), start_now);
}

bool OtherSlicerImportDialog::run(wxWindow *parent, OtherSlicersFound found, bool start_now)
{
    OtherSlicerImportDialog dlg(parent, std::move(found), start_now);
    dlg.ShowModal();
    return dlg.imported_any();
}

void run_when_no_dialog(GUI_App &app, std::function<void()> fn) { when_no_dialog(app, std::move(fn)); }

void ask_import_other_slicers_once(GUI_App &app, bool wizard_showed)
{
    if (app.app_config == nullptr)
        return;
    const std::string asked = app.app_config->get(ASKED_KEY);
    if (asked == "never")
        return;
    if (wizard_showed) {
        // A first run: the setup wizard offered the import before its printer picker.
        app.app_config->set(ASKED_KEY, UNLAYERED_VERSION);
        return;
    }
    if (!asked.empty()) {
        const auto then = Semver::parse(asked);
        const auto now  = Semver::parse(UNLAYERED_VERSION);
        if (!then || !now || !(*then < *now))
            return; // asked in this version already
    }
    when_no_dialog(app, [&app]() {
        OtherSlicersFound found = find_other_slicers();
        if (found.total() == 0)
            return; // nothing to ask about (yet); looked for again at the next launch
        AskImportDialog dlg(app.mainframe, found);
        const int       answer = dlg.ShowModal();
        app.app_config->set(ASKED_KEY, answer == wxID_NO ? std::string("never") : std::string(UNLAYERED_VERSION));
        app.app_config->save();
        BOOST_LOG_TRIVIAL(info) << "other slicers: asked after the update, answer "
                                << (answer == wxID_YES ? "import" : answer == wxID_NO ? "never" : "not now");
        if (answer == wxID_YES)
            // Imports at once and shows the summary (what came over, what was skipped and why).
            OtherSlicerImportDialog::run(app.mainframe, std::move(found), true);
    });
}

} // namespace GUI
} // namespace Slic3r
