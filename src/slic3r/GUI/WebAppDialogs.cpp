#include "WebAppDialogs.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <vector>

#include <boost/algorithm/string/case_conv.hpp>
#include <boost/algorithm/string/trim.hpp>
#include <boost/log/trivial.hpp>

#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/dialog.h>
#include <wx/modalhook.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/utils.h>

#include "GUI_App.hpp"
#include "MsgDialog.hpp"
#include "SavePresetDialog.hpp"
#include "UnsavedChangesDialog.hpp"
#include "WebAppBridge.hpp"
#include "Widgets/Button.hpp"

namespace Slic3r { namespace GUI { namespace WebAppDialogs {

using json = nlohmann::json;

namespace {

struct OpenDialog
{
    std::string id;
    wxDialog*   dialog     = nullptr;
    bool        by_bridge  = false; // opened while a page's request was being handled
    bool        human_only = false; // opened inside a HumanOnlyScope
};

std::vector<OpenDialog>   s_open; // outermost first
std::set<std::string>     s_pressed; // ids a page pressed, for dialogClosed's "by"
std::map<int, Listener>   s_listeners;
int                       s_bridge_depth  = 0;
int                       s_human_depth   = 0;
int                       s_next_id       = 0;
int                       s_next_listener = 0;

// Bounds on what one description carries: a dialog is read by a model, not archived.
constexpr size_t MAX_TEXTS      = 60;
constexpr size_t MAX_TEXT_CHARS = 300;
constexpr int    MAX_DEPTH      = 12;

void emit(const std::string& method, const json& data)
{
    const std::map<int, Listener> listeners = s_listeners; // a listener may remove itself
    for (const auto& l : listeners)
        l.second(method, data);
}

std::string utf8(const wxString& s) { return std::string(s.ToUTF8().data()); }

std::string clean_label(const wxString& label)
{
    std::string s = utf8(wxStripMenuCodes(label));
    boost::algorithm::trim(s);
    return s;
}

std::string normalised(const std::string& label)
{
    std::string s = boost::algorithm::to_lower_copy(label);
    boost::algorithm::trim(s);
    while (!s.empty() && (s.back() == '.' || s.back() == ':'))
        s.pop_back();
    return s;
}

// The dialog kinds a page may answer. Everything else is read-only to it, however it was opened.
bool kind_answerable(wxDialog* dlg)
{
    if (dynamic_cast<DownloadDialog*>(dlg) != nullptr)
        return false;
    return dynamic_cast<UnsavedChangesDialog*>(dlg) != nullptr || dynamic_cast<SavePresetDialog*>(dlg) != nullptr ||
           dynamic_cast<MsgDialog*>(dlg) != nullptr;
}

// Why the page may not answer `d`, or "" when it may.
std::string refusal(const OpenDialog& d)
{
    if (d.human_only)
        return "the person's own confirmation";
    if (!d.by_bridge)
        return "opened by the person";
    if (!kind_answerable(d.dialog))
        return "a kind of dialog the page may not answer";
    return {};
}

// A button the page may press: a wxButton or the slicer's own Button widget, shown and labelled.
struct Pressable
{
    wxWindow*   window = nullptr;
    std::string label;
};

void collect(wxWindow* w, int depth, json& texts, std::vector<Pressable>& buttons, json& checks, json& fields)
{
    if (w == nullptr || depth > MAX_DEPTH)
        return;
    for (wxWindow* child : w->GetChildren()) {
        if (child == nullptr || !child->IsShown() || dynamic_cast<wxDialog*>(child) != nullptr)
            continue;
        if (dynamic_cast<Button*>(child) != nullptr || dynamic_cast<wxButton*>(child) != nullptr) {
            const std::string label = clean_label(child->GetLabel());
            if (!label.empty())
                buttons.push_back({child, label});
            continue;
        }
        if (auto* cb = dynamic_cast<wxCheckBox*>(child); cb != nullptr) {
            checks.push_back({{"label", clean_label(cb->GetLabel())}, {"checked", cb->GetValue()}});
            continue;
        }
        if (auto* tc = dynamic_cast<wxTextCtrl*>(child); tc != nullptr) {
            std::string v = utf8(tc->GetValue());
            if (v.size() > MAX_TEXT_CHARS)
                v = v.substr(0, MAX_TEXT_CHARS) + "...";
            fields.push_back({{"value", v}, {"editable", tc->IsEditable()}});
            continue;
        }
        if (auto* st = dynamic_cast<wxStaticText*>(child); st != nullptr) {
            std::string t = clean_label(st->GetLabel());
            if (!t.empty() && texts.size() < MAX_TEXTS)
                texts.push_back(t.size() > MAX_TEXT_CHARS ? t.substr(0, MAX_TEXT_CHARS) + "..." : t);
            continue;
        }
        collect(child, depth + 1, texts, buttons, checks, fields);
    }
}

std::vector<Pressable> buttons_of(wxDialog* dlg)
{
    json                   texts = json::array(), checks = json::array(), fields = json::array();
    std::vector<Pressable> buttons;
    collect(dlg, 0, texts, buttons, checks, fields);
    return buttons;
}

json describe(const OpenDialog& d)
{
    json                   texts = json::array(), checks = json::array(), fields = json::array(), labels = json::array();
    std::vector<Pressable> buttons;
    collect(d.dialog, 0, texts, buttons, checks, fields);
    for (const Pressable& b : buttons)
        labels.push_back({{"label", b.label}, {"enabled", b.window->IsEnabled()}});
    const std::string why = refusal(d);
    json out = {{"id", d.id},
                {"title", clean_label(d.dialog->GetTitle())},
                {"openedBy", d.by_bridge ? "bridge" : "user"},
                {"texts", texts},
                {"buttons", labels},
                {"answerable", why.empty()}};
    if (!checks.empty())
        out["checkboxes"] = checks;
    if (!fields.empty())
        out["fields"] = fields;
    if (!why.empty())
        out["reason"] = why;
    return out;
}

std::string result_name(int code)
{
    switch (code) {
    case wxID_OK: return "ok";
    case wxID_CANCEL: return "cancel";
    case wxID_YES: return "yes";
    case wxID_NO: return "no";
    case wxID_CLOSE: return "close";
    case wxID_APPLY: return "apply";
    default: return std::to_string(code);
    }
}

class Hook : public wxModalDialogHook
{
protected:
    int Enter(wxDialog* dialog) override
    {
        if (dialog == nullptr)
            return wxID_NONE;
        OpenDialog d;
        d.id         = "d" + std::to_string(++s_next_id);
        d.dialog     = dialog;
        d.by_bridge  = s_bridge_depth > 0;
        d.human_only = s_human_depth > 0;
        s_open.push_back(d);
        if (!s_listeners.empty())
            emit("slicer.dialogOpened", {{"dialog", describe(d)}});
        return wxID_NONE;
    }

    void Exit(wxDialog* dialog) override
    {
        auto it = std::find_if(s_open.begin(), s_open.end(), [dialog](const OpenDialog& d) { return d.dialog == dialog; });
        if (it == s_open.end())
            return;
        const std::string id  = it->id;
        const bool        was = s_pressed.erase(id) != 0;
        s_open.erase(it);
        if (!s_listeners.empty())
            emit("slicer.dialogClosed", {{"id", id}, {"result", result_name(dialog->GetReturnCode())}, {"by", was ? "bridge" : "user"}});
    }
};

const OpenDialog* find_open(const std::string& id)
{
    for (const OpenDialog& d : s_open)
        if (d.id == id)
            return &d;
    return nullptr;
}

} // namespace

void install()
{
    static Hook* hook = nullptr; // never freed: it must outlive every dialog, and wx's own statics
    if (hook == nullptr) {
        hook = new Hook();
        hook->Register();
    }
}

BridgeScope::BridgeScope() { ++s_bridge_depth; }
BridgeScope::~BridgeScope() { --s_bridge_depth; }
HumanOnlyScope::HumanOnlyScope() { ++s_human_depth; }
HumanOnlyScope::~HumanOnlyScope() { --s_human_depth; }

json list()
{
    json out = json::array();
    for (const OpenDialog& d : s_open)
        out.push_back(describe(d));
    return {{"dialogs", out}};
}

std::string busy_message()
{
    if (s_open.empty())
        return "a dialog is open";
    const OpenDialog& d = s_open.back();
    std::string       m = "a dialog is open: \"" + clean_label(d.dialog->GetTitle()) + "\"";
    std::string       labels;
    for (const Pressable& b : buttons_of(d.dialog))
        labels += (labels.empty() ? "" : ", ") + b.label;
    if (!labels.empty())
        m += " (" + labels + ")";
    const std::string why = refusal(d);
    m += why.empty() ? "; read it with slicer.getDialogs and answer it with slicer.answerDialog"
                     : "; " + why + ", so only the person can answer it";
    return m;
}

int answer(const json& params, std::string& message, json& data)
{
    data = json::object();
    if (s_open.empty()) {
        message = "no dialog is open";
        return WAB_NOT_FOUND;
    }
    const std::string id = params.contains("dialog") && params["dialog"].is_string() ? params["dialog"].get<std::string>()
                                                                                       : s_open.back().id;
    const OpenDialog* d  = find_open(id);
    if (d == nullptr) {
        message = "no open dialog has that id";
        data    = list();
        return WAB_NOT_FOUND;
    }
    data["dialog"] = d->id;
    if (d->id != s_open.back().id) {
        message = "another dialog is open in front of it; answer that one first";
        data["front"] = s_open.back().id;
        return WAB_BUSY;
    }
    if (const std::string why = refusal(*d); !why.empty()) {
        message = "this dialog is " + why + "; only the person can answer it";
        return WAB_CAPABILITY_DISABLED;
    }
    if (!params.contains("button") || !params["button"].is_string()) {
        message = "\"button\" is required: a button's label, or \"close\"";
        data["buttons"] = describe(*d)["buttons"];
        return WAB_BAD_PARAMS;
    }
    const std::string wanted = normalised(params["button"].get<std::string>());
    const bool        close  = wanted == "close";
    int               wx_id  = wxID_ANY;
    std::string       label  = "close";
    if (!close) {
        const std::vector<Pressable> buttons = buttons_of(d->dialog);
        auto it = std::find_if(buttons.begin(), buttons.end(), [&wanted](const Pressable& b) { return normalised(b.label) == wanted; });
        if (it == buttons.end()) {
            message         = "the dialog has no button \"" + params["button"].get<std::string>() + "\"";
            data["buttons"] = describe(*d)["buttons"];
            return WAB_BAD_PARAMS;
        }
        if (!it->window->IsEnabled()) {
            message = "that button is disabled";
            return WAB_BUSY;
        }
        wx_id = it->window->GetId();
        label = it->label;
    }

    // After the response: a press can open a dialog of its own (Save -> name the preset), whose
    // modal loop would otherwise hold this call's response until a person answered it.
    BOOST_LOG_TRIVIAL(info) << "WebAppDialogs: page pressed '" << label << "' in '" << utf8(d->dialog->GetTitle()) << "' (" << id << ")";
    wxGetApp().CallAfter([id, wx_id, label, close]() {
        const OpenDialog* now = find_open(id);
        if (now == nullptr || now->id != s_open.back().id)
            return; // closed, or covered, since
        wxDialog* dlg = now->dialog;
        s_pressed.insert(id);
        BridgeScope bridge; // whatever the press opens is the page's to answer too
        if (close) {
            dlg->EndModal(wxID_CANCEL);
            return;
        }
        for (const Pressable& b : buttons_of(dlg)) {
            if (b.window->GetId() != wx_id || b.label != label || !b.window->IsEnabled())
                continue;
            wxCommandEvent evt(wxEVT_BUTTON, wx_id);
            evt.SetEventObject(b.window);
            b.window->GetEventHandler()->ProcessEvent(evt);
            return;
        }
        s_pressed.erase(id);
    });
    message         = "pressed";
    data["pressed"] = label;
    return WAB_OK;
}

int add_listener(Listener listener)
{
    const int handle = ++s_next_listener;
    s_listeners[handle] = std::move(listener);
    return handle;
}

void remove_listener(int handle) { s_listeners.erase(handle); }

}}} // namespace Slic3r::GUI::WebAppDialogs
