#ifndef slic3r_NonplanarSliceClient_hpp_
#define slic3r_NonplanarSliceClient_hpp_

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include <boost/filesystem/path.hpp>

#include "Http.hpp"
#include "ServiceRefusal.hpp"

#include <optional>

namespace Slic3r {

class UnlayeredAccount;

// Client for an EXTERNAL slicing service: the plate goes out as a 3MF, G-code comes back. The
// protocol is specified in SLICE_SERVICE_PROTOCOL.md and this file implements only that -- it
// names no service, and the address is configuration, exactly as a print host's is.
//
// Synchronous on purpose: every call is made from NonplanarSliceJob's worker thread, where
// blocking is the point, and Http::perform_sync() there keeps the control flow readable. Never
// call these from the GUI thread.
class NonplanarSliceClient
{
public:
    // The four status spellings the protocol defines.
    enum class Status { Queued, Running, Succeeded, Failed, Unknown };

    struct Job
    {
        std::string              id;
        Status                   status = Status::Unknown;
        std::string              status_text;   // as the service spelled it, for messages
        std::string              error;         // set when status == Failed
        std::string              filename;      // result.filename, if the service gave one
        std::string              sha256;
        uint64_t                 size = 0;
        std::vector<std::string> warnings;
        // Optional (protocol v2): how far along the service says it is, 0..1, or < 0 when it
        // reports nothing; and the label of the step it is on, for the progress text.
        double                   progress = -1.;
        std::string              stage;

        bool finished() const { return status == Status::Succeeded || status == Status::Failed; }
    };

    // ---- address + transport ------------------------------------------------------------
    // The configured service root without a trailing slash, or empty when none is configured.
    // ORCA_NONPLANAR_SLICE_URL wins over the stored setting and is never written back.
    static std::string configured_url();
    static bool        is_configured();
    // https anywhere; plain http only for a loopback host. Same rule as the web app tab, and
    // for the same reason: a cleartext address on the network is not offered an override.
    static bool        is_secure_url(const std::string &url);
    // Why `url` is unusable, as a sentence for the user; empty when it is fine.
    static std::string url_problem(const std::string &url);

    explicit NonplanarSliceClient(std::string url);

    // True for a service on this machine (loopback).
    static bool is_local_url(const std::string &url);

    // Sent as `Authorization: Bearer` on every request from now on. Empty sends none.
    void set_bearer(std::string token) { m_bearer = std::move(token); }

    // Attaches a short-lived token from the account provider when the rules allow one for this
    // service (UnlayeredAccount::may_receive_token with AppConfig service_token_origins) and the
    // account is signed in. Otherwise sends none: the service does its own authentication and may
    // answer with a refusal. NotNeeded and Attached are both fine to go on with; Refused means the
    // PROVIDER refused (refusal() says why); Failed is anything else (`error`).
    // `refresh_after_s` is when to ask again on a long job.
    enum class TokenResult { NotNeeded, Attached, Refused, Failed };
    TokenResult authorize(UnlayeredAccount *account, std::string &error, long &refresh_after_s);

    // The last refusal (HTTP 401/402/403, SLICE_SERVICE_PROTOCOL.md "Refusals") any call on this
    // client received, from the service or, via authorize(), from the account provider.
    const std::optional<ServiceRefusal> &refusal() const { return m_refusal; }

    // What the service says about itself (GET <root>/, protocol v2): its `display_name` and its
    // slicing `modes`. Raw JSON; false (with `error`) when it has none or cannot be reached.
    bool fetch_info(std::string &body, std::string &error) const;

    // The selected mode's id (sent as the `mode` form field; empty sends none) and the number of
    // machine axes the slicer works out from the printer preset (sent as `machine_axes`; 0 sends
    // none), with /slice and /prepare.
    void set_mode(std::string mode) { m_mode = std::move(mode); }
    void set_machine_axes(int axes) { m_machine_axes = axes; }

    // The plate the mesh came from, 1-based, sent as the `plate` form field with /slice and
    // /prepare. The mesh holds only that plate's objects (Plater::export_slice_service_handoff),
    // so a service may also read the file as a one-plate project; 0 sends no field.
    void set_plate(int plate_1based) { m_plate = plate_1based; }

    // Each returns false and fills `error` with something worth showing a user.
    //
    // `settings` is the resolved print configuration as JSON -- the values, not preset names,
    // so a service that cannot see the user's preset library still slices correctly. The three
    // names go along as labels. An empty `settings` path sends none: with `settings_from_mesh`
    // the service reads the same values from the 3MF's own Metadata/project_settings.config,
    // together with the per-object overrides only the project carries (protocol v2).
    // `options_json` is a JSON object of the service's own options (from its /ui-schema), or
    // empty for none.
    // `progress` gets (sent, total) bytes of the upload and returns false to abort it; the error
    // is then "cancelled", as in download_gcode.
    using ProgressFn = std::function<bool(uint64_t /*done*/, uint64_t /*total*/)>;
    bool submit(const boost::filesystem::path &mesh,
                const boost::filesystem::path &settings,
                bool               settings_from_mesh,
                const std::string &options_json,
                const std::string &printer,
                const std::string &process,
                const std::string &filament,
                Job               &job,
                std::string       &error,
                ProgressFn         progress = nullptr) const;

    // POST <root>/prepare: the same fields as submit(), but the service only prepares and caches
    // the mesh for this geometry (the slow step) and makes no G-code, so a later submit of the same
    // plate skips it. Returns the HTTP status, 0 for no answer or an abort; `status_text` is the
    // service's "queued" / "running" / "skipped". 404/405 = a service without the endpoint.
    unsigned prepare(const boost::filesystem::path &mesh,
                     bool               settings_from_mesh,
                     const std::string &options_json,
                     const std::string &printer,
                     const std::string &process,
                     const std::string &filament,
                     std::string       &status_text,
                     std::string       &error,
                     ProgressFn         progress = nullptr) const;

    bool poll(const std::string &job_id, Job &job, std::string &error) const;

    // Best effort (protocol v2): asks the service to stop a job the user cancelled. A service
    // without the endpoint just finishes it; nothing waits for the answer.
    void cancel(const std::string &job_id) const;

    // GET <root>/ui-schema (protocol v2): the service's own options form, as raw JSON. False
    // (with `error`) when the service has none or cannot be reached.
    bool fetch_schema(std::string &body, std::string &error) const;

    // Streams the result to `dest`. `progress` gets (received, total) and returns false to
    // abort; total is 0 when the service sends no Content-Length. Asks for a one-use download
    // grant first and falls back to the plain path for a service without grants; when
    // `expected_sha256` is given the bytes must match it.
    bool download_gcode(const std::string             &job_id,
                        const boost::filesystem::path &dest,
                        std::string                   &error,
                        ProgressFn                     progress        = nullptr,
                        const std::string             &expected_sha256 = {}) const;

private:
    std::string m_url;   // no trailing slash
    std::string m_bearer;
    int         m_plate = 0;
    std::string m_mode;
    int         m_machine_axes = 0;
    mutable std::optional<ServiceRefusal> m_refusal;

    // Records a refusal in `body` when `status` is one; returns the text to show either way.
    std::string note_failure(unsigned status, const std::string &body, const std::string &rel) const;

    // `http` with the bearer attached when one is set.
    Http authed(Http http) const;

    std::string endpoint(const std::string &rel) const;
    // The multipart POST that /slice and /prepare share, so the two can never send different fields.
    unsigned post_form(const std::string             &rel,
                       const boost::filesystem::path &mesh,
                       const boost::filesystem::path &settings,
                       bool                           settings_from_mesh,
                       const std::string             &options_json,
                       const std::string             &printer,
                       const std::string             &process,
                       const std::string             &filament,
                       ProgressFn                     progress,
                       std::string                   &body,
                       std::string                   &net_err,
                       bool                          &aborted) const;
    // The one-use download path from POST jobs/{id}/grant, relative to the root, or empty.
    std::string grant_path(const std::string &job_id) const;
};

} // namespace Slic3r

#endif // slic3r_NonplanarSliceClient_hpp_
