#include "NonplanarSliceClient.hpp"

#include "FirmwareBundle.hpp" // sha256_hex
#include "Http.hpp"
#include "libslic3r/AppConfig.hpp"
#include "slic3r/GUI/GUI.hpp" // get_app_config
#include "slic3r/GUI/WebAppSlot.hpp" // the dev-edition switch + its local-web-apps setting
#include "UnlayeredAccount.hpp"

#include <boost/algorithm/string/case_conv.hpp>
#include <boost/algorithm/string/predicate.hpp>
#include <boost/algorithm/string/trim.hpp>
#include <boost/filesystem/fstream.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/format.hpp>
#include <boost/log/trivial.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdlib>

using json = nlohmann::json;
namespace fs = boost::filesystem;

// empty_post_body_note: Http always installs its form-file read callback; a POST with neither a form
// nor a body makes curl call it with no file and it crashes (tellg on null, 2026-10-12). So the
// body-less POSTs here (grant, cancel) send "{}".

namespace Slic3r {

namespace {

// The service is on the far side of a network and slicing takes minutes; only the CONNECT is
// given a short fuse. A submit carries the whole plate, so it gets a generous ceiling.
const long NPS_TIMEOUT_CONNECT = 10;
const long NPS_TIMEOUT_POLL    = 30;
const long NPS_TIMEOUT_SUBMIT  = 600;
// A G-code file is large but not unbounded; refuse to buffer a runaway response.
const size_t NPS_MAX_GCODE_BYTES = 512u * 1024u * 1024u;

std::string trim_trailing_slashes(std::string s)
{
    while (! s.empty() && (s.back() == '/' || s.back() == '\\'))
        s.pop_back();
    return s;
}

// The host part of an http(s) URL, lowercased and without port or credentials.
std::string host_of(const std::string &url)
{
    const auto scheme_end = url.find("://");
    if (scheme_end == std::string::npos)
        return {};
    std::string rest = url.substr(scheme_end + 3);
    const auto  at   = rest.find('@');
    if (at != std::string::npos)
        rest = rest.substr(at + 1);
    rest = rest.substr(0, rest.find_first_of("/?#"));
    if (! rest.empty() && rest.front() == '[') {          // [::1]:8000
        const auto close = rest.find(']');
        return close == std::string::npos ? rest : boost::to_lower_copy(rest.substr(1, close - 1));
    }
    rest = rest.substr(0, rest.find(':'));
    return boost::to_lower_copy(rest);
}

bool is_loopback_host(const std::string &host)
{
    if (host == "localhost" || host == "::1" || host == "0:0:0:0:0:0:0:1")
        return true;
    // The whole 127.0.0.0/8 block, not just 127.0.0.1.
    return boost::starts_with(host, "127.");
}

// `detail` out of an error body, so the user sees the service's sentence rather than its JSON.
std::string readable_error_body(const std::string &body)
{
    std::string text = body;
    try {
        const json j = json::parse(body);
        if (j.contains("detail")) {
            const json &d = j.at("detail");
            text = d.is_string() ? d.get<std::string>() : d.dump();
        }
    } catch (const std::exception &) {
        // not JSON; the raw body it is
    }
    boost::trim(text);
    if (text.size() > 500)
        text = text.substr(0, 500) + "…";
    return text;
}

NonplanarSliceClient::Status status_from(const std::string &s)
{
    if (s == "queued")    return NonplanarSliceClient::Status::Queued;
    if (s == "running")   return NonplanarSliceClient::Status::Running;
    if (s == "succeeded") return NonplanarSliceClient::Status::Succeeded;
    if (s == "failed")    return NonplanarSliceClient::Status::Failed;
    return NonplanarSliceClient::Status::Unknown;
}

} // namespace

// ---------------------------------------------------------------------------------------------
// Address + transport
// ---------------------------------------------------------------------------------------------

std::string NonplanarSliceClient::configured_url()
{
#if !UNLAYERED_SLICE_SERVICE
    // Built with UNLAYERED_SLICE_SERVICE=OFF (a side-panel-only release): no service address, so
    // is_configured() is false and every slicing-service entry point stays hidden -- the Slice dropdown
    // entries, the options dialog, the prepare-on-select step, Plater::slice_nonplanar.
    return {};
#else
    // The development override wins and is never written back, so a session pointed at a local
    // service cannot quietly become the user's stored configuration.
    if (const char *env = std::getenv("ORCA_NONPLANAR_SLICE_URL"); env != nullptr && *env != '\0')
        return trim_trailing_slashes(boost::trim_copy(std::string(env)));

    AppConfig *cfg = GUI::get_app_config();
    if (cfg == nullptr)
        return {};
    std::string url = trim_trailing_slashes(boost::trim_copy(cfg->get(SETTING_NONPLANAR_SLICE_URL)));
    return url;
#endif // UNLAYERED_SLICE_SERVICE
}

bool NonplanarSliceClient::is_configured()
{
    const std::string url = configured_url();
    return ! url.empty() && url_problem(url).empty();
}

bool NonplanarSliceClient::is_secure_url(const std::string &url)
{
    // Mirrors WebAppPanel::is_secure_context deliberately: one transport rule for every address
    // the user can point this program at. https anywhere; plain http only to loopback, which the
    // W3C treats as a trustworthy origin. There is no override -- a cleartext slicing service on
    // the network would carry the user's geometry in the open.
    if (boost::istarts_with(url, "https://"))
        return true;
    return boost::istarts_with(url, "http://") && is_loopback_host(host_of(url));
}

std::string NonplanarSliceClient::url_problem(const std::string &url)
{
    if (url.empty())
        return "no address is configured";
    if (! boost::istarts_with(url, "http://") && ! boost::istarts_with(url, "https://"))
        return "the address must start with http:// or https://";
    if (host_of(url).empty())
        return "the address has no host";
    if (! is_secure_url(url))
        return "plain http is only allowed for a service on this machine (localhost); "
               "use https for anything else";
    return {};
}

// ---------------------------------------------------------------------------------------------

NonplanarSliceClient::NonplanarSliceClient(std::string url)
    : m_url(trim_trailing_slashes(std::move(url)))
{}

std::string NonplanarSliceClient::endpoint(const std::string &rel) const
{
    return m_url + "/" + rel;
}

Http NonplanarSliceClient::authed(Http http) const
{
    if (! m_bearer.empty())
        http.header("Authorization", "Bearer " + m_bearer);
    return http;
}

bool NonplanarSliceClient::is_local_url(const std::string &url)
{
    return is_loopback_host(host_of(url));
}

std::string NonplanarSliceClient::note_failure(unsigned status, const std::string &body, const std::string &rel) const
{
    if (auto r = parse_service_refusal(status, body, endpoint(rel))) {
        m_refusal = *r;
        return r->message;
    }
    return readable_error_body(body);
}

NonplanarSliceClient::TokenResult NonplanarSliceClient::authorize(UnlayeredAccount *account, std::string &error,
                                                                  long &refresh_after_s)
{
    // Provider tokens are short-lived; ask again well before a typical lifetime runs out.
    refresh_after_s = 10 * 60;
    if (account == nullptr || !account->has_provider() || !account->signed_in())
        return TokenResult::NotNeeded;
    AppConfig        *cfg     = GUI::get_app_config();
    const std::string origins = cfg != nullptr ? cfg->get(SETTING_SERVICE_TOKEN_ORIGINS) : std::string();
    if (!account->may_receive_token(m_url, origins))
        return TokenResult::NotNeeded;

    std::string    token;
    long           expires_in = 0;
    unsigned       status     = 0;
    ServiceRefusal refusal;
    if (!account->fetch_service_token(url_origin(m_url), token, expires_in, status, error, &refusal)) {
        if (refusal.status != 0) {
            m_refusal = refusal;
            return TokenResult::Refused;
        }
        return TokenResult::Failed;
    }
    set_bearer(token);
    if (expires_in > 0)
        refresh_after_s = std::max(30L, std::min(refresh_after_s, expires_in - 60));
    return TokenResult::Attached;
}

bool NonplanarSliceClient::fetch_info(std::string &out, std::string &error) const
{
    std::string body;
    unsigned    status = 0;
    std::string net_err;
    authed(Http::get(m_url + "/"))
        .timeout_connect(NPS_TIMEOUT_CONNECT)
        .timeout_max(NPS_TIMEOUT_POLL)
        .size_limit(256u * 1024u)
        .on_complete([&](std::string b, unsigned s) { body = std::move(b); status = s; })
        .on_error([&](std::string b, std::string e, unsigned s) {
            body    = std::move(b);
            status  = s;
            net_err = std::move(e);
        })
        .perform_sync();
    if (status == 0) {
        error = net_err.empty() ? "the slicing service did not answer" : net_err;
        return false;
    }
    if (status >= 400) {
        error = (boost::format("the slicing service did not describe itself (HTTP %1%)") % status).str();
        return false;
    }
    try {
        if (!json::parse(body).is_object()) {
            error = "the slicing service's description is not a JSON object";
            return false;
        }
    } catch (const std::exception &e) {
        error = std::string("the slicing service's description could not be read: ") + e.what();
        return false;
    }
    out = std::move(body);
    return true;
}

unsigned NonplanarSliceClient::post_form(const std::string &rel,
                                         const fs::path    &mesh,
                                         const fs::path    &settings,
                                         bool               settings_from_mesh,
                                         const std::string &options_json,
                                         const std::string &printer,
                                         const std::string &process,
                                         const std::string &filament,
                                         ProgressFn         progress,
                                         std::string       &body,
                                         std::string       &net_err,
                                         bool              &aborted) const
{
    unsigned status = 0;
    auto     http   = authed(Http::post(endpoint(rel)));
    // form_add_file, never set_post_body: the former streams the file in BINARY mode, which a
    // .3mf (a zip) requires. set_post_body(path) reads in text mode and would corrupt it.
    http.form_add_file("mesh", mesh.string(), mesh.filename().string());
    if (! settings.empty())
        http.form_add_file("settings", settings.string(), "settings.json");
    if (settings_from_mesh)
        http.form_add("settings_from_mesh", "1");
    if (! options_json.empty())
        http.form_add("options", options_json);
    if (! printer.empty())  http.form_add("printer", printer);
    if (! process.empty())  http.form_add("process", process);
    if (! filament.empty()) http.form_add("filament", filament);
    if (m_plate > 0)        http.form_add("plate", std::to_string(m_plate));
    if (! m_mode.empty())   http.form_add("mode", m_mode);
    if (m_machine_axes > 0) http.form_add("machine_axes", std::to_string(m_machine_axes));

    // The upload can take minutes on a slow uplink; without this hook Stop waited for all of it.
    http.timeout_connect(NPS_TIMEOUT_CONNECT)
        .timeout_max(NPS_TIMEOUT_SUBMIT)
        .on_progress([&](Http::Progress p, bool &cancel) {
            if (progress && ! progress(uint64_t(p.ulnow), uint64_t(p.ultotal))) {
                cancel  = true;
                aborted = true;
            }
        })
        .on_complete([&](std::string b, unsigned s) { body = std::move(b); status = s; })
        .on_error([&](std::string b, std::string e, unsigned s) {
            body    = std::move(b);
            status  = s;
            net_err = std::move(e);
        })
        .perform_sync();
    return status;
}

unsigned NonplanarSliceClient::prepare(const fs::path    &mesh,
                                       bool               settings_from_mesh,
                                       const std::string &options_json,
                                       const std::string &printer,
                                       const std::string &process,
                                       const std::string &filament,
                                       std::string       &status_text,
                                       std::string       &error,
                                       ProgressFn         progress) const
{
    std::string body;
    std::string net_err;
    bool        aborted = false;
    const unsigned status = post_form("prepare", mesh, fs::path(), settings_from_mesh, options_json, printer,
                                      process, filament, progress, body, net_err, aborted);
    if (aborted) {
        error = "cancelled";
        return 0;
    }
    if (status == 0) {
        error = net_err.empty() ? "the slicing service did not answer" : net_err;
        return 0;
    }
    if (status >= 400) {
        error = note_failure(status, body, "prepare");
        return status;
    }
    try {
        status_text = json::parse(body).value("status", std::string());
    } catch (const std::exception &) {
        // The status is informational; a 2xx is all that matters.
    }
    return status;
}

bool NonplanarSliceClient::submit(const fs::path   &mesh,
                                  const fs::path   &settings,
                                  bool              settings_from_mesh,
                                  const std::string &options_json,
                                  const std::string &printer,
                                  const std::string &process,
                                  const std::string &filament,
                                  Job               &job,
                                  std::string       &error,
                                  ProgressFn         progress) const
{
    boost::system::error_code ec;
    if (! fs::is_regular_file(mesh, ec)) {
        error = "the exported plate could not be read back";
        return false;
    }
    if (! settings.empty() && ! fs::is_regular_file(settings, ec)) {
        error = "the exported print settings could not be read back";
        return false;
    }

    std::string body;
    std::string net_err;
    bool        aborted = false;
    const unsigned status = post_form("slice", mesh, settings, settings_from_mesh, options_json, printer, process,
                                      filament, progress, body, net_err, aborted);

    if (aborted) {
        error = "cancelled";
        return false;
    }
    if (status == 0) {
        error = net_err.empty() ? "the slicing service did not answer" : net_err;
        return false;
    }
    if (status >= 400) {
        const std::string detail = note_failure(status, body, "slice");
        error = m_refusal ? detail
                          : (boost::format("the slicing service refused the job (HTTP %1%)%2%") % status %
                             (detail.empty() ? "" : ": " + detail)).str();
        return false;
    }

    try {
        const json j = json::parse(body);
        job.id = j.value("job_id", std::string());
        job.status_text = j.value("status", std::string("queued"));
        job.status = status_from(job.status_text);
        if (j.contains("warnings") && j.at("warnings").is_array())
            for (const auto &w : j.at("warnings"))
                if (w.is_string())
                    job.warnings.push_back(w.get<std::string>());
    } catch (const std::exception &e) {
        error = std::string("the slicing service's answer could not be read: ") + e.what();
        return false;
    }
    if (job.id.empty()) {
        error = "the slicing service accepted the job but returned no job id";
        return false;
    }
    BOOST_LOG_TRIVIAL(info) << "NonplanarSlice: submitted job " << job.id;
    return true;
}

bool NonplanarSliceClient::poll(const std::string &job_id, Job &job, std::string &error) const
{
    std::string body;
    unsigned    status = 0;
    std::string net_err;

    authed(Http::get(endpoint("jobs/" + job_id)))
        .timeout_connect(NPS_TIMEOUT_CONNECT)
        .timeout_max(NPS_TIMEOUT_POLL)
        .size_limit(4u * 1024u * 1024u)
        .on_complete([&](std::string b, unsigned s) { body = std::move(b); status = s; })
        .on_error([&](std::string b, std::string e, unsigned s) {
            body    = std::move(b);
            status  = s;
            net_err = std::move(e);
        })
        .perform_sync();

    if (status == 0) {
        error = net_err.empty() ? "the slicing service stopped answering" : net_err;
        return false;
    }
    if (status >= 400) {
        const std::string detail = note_failure(status, body, "jobs/" + job_id);
        error = m_refusal ? detail
                          : (boost::format("the slicing service lost the job (HTTP %1%)%2%") % status %
                             (detail.empty() ? "" : ": " + detail)).str();
        return false;
    }

    try {
        const json j = json::parse(body);
        job.id          = job_id;
        job.status_text = j.value("status", std::string());
        job.status      = status_from(job.status_text);
        job.error       = j.value("error", std::string());
        job.warnings.clear();
        if (j.contains("warnings") && j.at("warnings").is_array())
            for (const auto &w : j.at("warnings"))
                if (w.is_string())
                    job.warnings.push_back(w.get<std::string>());
        job.progress = -1.;
        if (j.contains("progress") && j.at("progress").is_number())
            job.progress = std::clamp(j.at("progress").get<double>(), 0., 1.);
        job.stage.clear();
        if (j.contains("stages") && j.at("stages").is_array())
            for (const auto &st : j.at("stages"))
                if (st.is_object() && st.value("state", std::string()) == "active") {
                    job.stage = st.value("label", std::string());
                    break;
                }
        if (j.contains("result") && j.at("result").is_object()) {
            const json &r = j.at("result");
            job.filename = r.value("filename", std::string());
            job.sha256   = r.value("sha256", std::string());
            if (r.contains("size") && r.at("size").is_number_unsigned())
                job.size = r.at("size").get<uint64_t>();
        }
    } catch (const std::exception &e) {
        error = std::string("the slicing service's answer could not be read: ") + e.what();
        return false;
    }

    if (job.status == Status::Unknown) {
        error = job.status_text.empty()
                    ? std::string("the slicing service reported no status")
                    : "the slicing service reported an unknown status \"" + job.status_text + "\"";
        return false;
    }
    return true;
}

void NonplanarSliceClient::cancel(const std::string &job_id) const
{
    if (job_id.empty())
        return;
    unsigned status = 0;
    authed(Http::post(endpoint("jobs/" + job_id + "/cancel")))
        .set_post_body(std::string("{}")) // never body-less: see empty_post_body_note
        .timeout_connect(NPS_TIMEOUT_CONNECT)
        .timeout_max(NPS_TIMEOUT_POLL)
        .on_complete([&](std::string, unsigned s) { status = s; })
        .on_error([&](std::string, std::string, unsigned s) { status = s; })
        .perform_sync();
    BOOST_LOG_TRIVIAL(info) << "NonplanarSlice: cancel " << job_id << " -> HTTP " << status;
}

bool NonplanarSliceClient::fetch_schema(std::string &out, std::string &error) const
{
    std::string body;
    unsigned    status = 0;
    std::string net_err;

    authed(Http::get(endpoint("ui-schema")))
        .timeout_connect(NPS_TIMEOUT_CONNECT)
        .timeout_max(NPS_TIMEOUT_POLL)
        .size_limit(1024u * 1024u)
        .on_complete([&](std::string b, unsigned s) { body = std::move(b); status = s; })
        .on_error([&](std::string b, std::string e, unsigned s) {
            body    = std::move(b);
            status  = s;
            net_err = std::move(e);
        })
        .perform_sync();

    if (status == 0) {
        error = net_err.empty() ? "the slicing service did not answer" : net_err;
        return false;
    }
    if (status >= 400) {
        const std::string detail = note_failure(status, body, "ui-schema");
        error = m_refusal ? detail : (boost::format("the slicing service has no options form (HTTP %1%)") % status).str();
        return false;
    }
    try {
        const json j = json::parse(body);
        if (! j.is_object() || ! j.contains("fields") || ! j.at("fields").is_array()) {
            error = "the slicing service's options form is not in the expected shape";
            return false;
        }
    } catch (const std::exception &e) {
        error = std::string("the slicing service's options form could not be read: ") + e.what();
        return false;
    }
    out = std::move(body);
    return true;
}

std::string NonplanarSliceClient::grant_path(const std::string &job_id) const
{
    std::string body;
    unsigned    status = 0;
    authed(Http::post(endpoint("jobs/" + job_id + "/grant")))
        .set_post_body(std::string("{}")) // never body-less: see empty_post_body_note
        .timeout_connect(NPS_TIMEOUT_CONNECT)
        .timeout_max(NPS_TIMEOUT_POLL)
        .size_limit(64u * 1024u)
        .on_complete([&](std::string b, unsigned s) { body = std::move(b); status = s; })
        .on_error([&](std::string b, std::string, unsigned s) { body = std::move(b); status = s; })
        .perform_sync();
    if (status != 200)
        return {};
    try {
        const json        j   = json::parse(body);
        const std::string url = j.value("url", std::string());
        // Relative to the root, and only ever a path under jobs/: a grant must not be able to
        // send the download (and the user's bytes) anywhere else.
        if (boost::starts_with(url, "jobs/" + job_id + "/"))
            return url;
    } catch (const std::exception &) {
    }
    return {};
}

bool NonplanarSliceClient::download_gcode(const std::string &job_id,
                                          const fs::path    &dest,
                                          std::string       &error,
                                          ProgressFn         progress,
                                          const std::string &expected_sha256) const
{
    std::string body;
    unsigned    status = 0;
    std::string net_err;
    bool        aborted = false;

    // A service that guards its downloads hands out a one-use grant; one that does not answers
    // the plain path. Asking costs one round trip either way.
    std::string rel = grant_path(job_id);
    if (rel.empty())
        rel = "jobs/" + job_id + "/gcode";

    authed(Http::get(endpoint(rel)))
        .timeout_connect(NPS_TIMEOUT_CONNECT)
        .size_limit(NPS_MAX_GCODE_BYTES)
        .on_progress([&](Http::Progress p, bool &cancel) {
            if (progress && ! progress(uint64_t(p.dlnow), uint64_t(p.dltotal))) {
                cancel  = true;
                aborted = true;
            }
        })
        .on_complete([&](std::string b, unsigned s) { body = std::move(b); status = s; })
        .on_error([&](std::string b, std::string e, unsigned s) {
            body    = std::move(b);
            status  = s;
            net_err = std::move(e);
        })
        .perform_sync();

    if (aborted) {
        error = "cancelled";
        return false;
    }
    if (status == 0) {
        error = net_err.empty() ? "the G-code download failed" : net_err;
        return false;
    }
    if (status >= 400) {
        const std::string detail = note_failure(status, body, rel);
        error = m_refusal ? detail
                          : (boost::format("the G-code could not be downloaded (HTTP %1%)%2%") % status %
                             (detail.empty() ? "" : ": " + detail)).str();
        return false;
    }
    if (body.empty()) {
        error = "the slicing service returned an empty G-code file";
        return false;
    }
    if (! expected_sha256.empty()) {
        const std::string actual = sha256_hex(std::vector<uint8_t>(body.begin(), body.end()));
        if (! boost::iequals(actual, expected_sha256)) {
            error = "the downloaded G-code does not match the checksum the slicing service quoted";
            return false;
        }
    }

    boost::system::error_code ec;
    fs::create_directories(dest.parent_path(), ec);
    // Binary: the bytes are written exactly as they arrived, so a sha256 the service quoted
    // still matches and no line ending is rewritten under the printer's feet.
    fs::ofstream out(dest, std::ios::out | std::ios::binary | std::ios::trunc);
    if (! out.good()) {
        error = "the downloaded G-code could not be written to " + dest.string();
        return false;
    }
    out.write(body.data(), std::streamsize(body.size()));
    out.close();
    if (! out.good()) {
        error = "the downloaded G-code could not be written to " + dest.string();
        return false;
    }
    BOOST_LOG_TRIVIAL(info) << "NonplanarSlice: wrote " << body.size() << " bytes to " << dest;
    return true;
}

} // namespace Slic3r
