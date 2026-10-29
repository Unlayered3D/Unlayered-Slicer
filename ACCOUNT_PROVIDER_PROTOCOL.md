# Account provider protocol

Unlayered Slicer can sign in to an **account provider**: a web service that identifies the person
using the slicer and gives out short-lived tokens to services they use. This document specifies
every request the slicer makes to a provider, so that anyone can run their own.

The provider only does **authentication**. It tells the slicer who the person is. It never tells
the slicer what the person may use, and the slicer has no entitlement checks of its own. A slicing
service or web page that wants to restrict a feature does so on its own server, and its refusals
reach the person through the documented refusal format ([SLICE_SERVICE_PROTOCOL.md](SLICE_SERVICE_PROTOCOL.md) §5).

Unlayered runs a provider at `https://unlayered3d.com`, and that is the default address. You can
point the slicer at any other provider. See [BOUNDARY.md](BOUNDARY.md) for how the provider fits
with the other interfaces.

---

## 1. Configuration

| Setting | Where | Default | Meaning |
|---|---|---|---|
| Account provider URL (`account_provider_url`) | Preferences ▸ Advanced ▸ Web apps & services | `https://unlayered3d.com` | The provider root. Blank means no account: Help ▸ Sign in is absent and no token is ever requested. |
| Service token origins (`service_token_origins`) | Preferences ▸ Advanced ▸ Web apps & services | empty | A comma-separated list of extra slicing-service origins that may receive a provider token (§5). |

The default is written only when the key is absent. A value the user sets is never overwritten.

**Every request** to the provider:

- verifies the TLS certificate (`https` is required, except `http` on a loopback host for
  development);
- sends `User-Agent: UnlayeredSlicer/<version>` and `Accept: application/json`;
- has a 10 s connect timeout and a 30 s total timeout;
- accepts a response body of at most 64 KiB;
- sends JSON bodies with `Content-Type: application/json`. A request with no logical body sends
  `{}`.

**Errors.** Any non-2xx answer may carry a refusal body in the format of
SLICE_SERVICE_PROTOCOL.md §5. The canonical form is
`{"error": {"code", "message", "action_url", "action_label"}}`. The slicer also accepts
`{"error": "<message>"}`, `{"detail": "<message>"}` and `upgradeUrl` (read as `action_url`; a path
is resolved against the provider origin). The message is shown to the person.

**401 on an authenticated request** means the device token is no longer valid. The slicer deletes
it and shows the person as signed out.

---

## 2. Sign-in: device hand-off

Signing in happens in the person's own browser, never in the slicer. The slicer never sees a
password.

```
slicer                                   provider                         browser
  POST /api/login/handoff/start  ----->   {id, secret}
  open <root>/auth/slicer?id=<id>  -------------------------------------> person signs in, confirms
  POST /api/slicer/device/collect  --->   {pending: true}     (every 2 s)
  POST /api/slicer/device/collect  --->   {token, account}
```

### `POST <root>/api/login/handoff/start`

Body `{}`. Answer:

```json
{ "id": "h_8f2c…", "secret": "s_41d9…" }
```

Both must be non-empty strings. `id` is not secret: it goes into the browser URL. `secret` never
leaves the slicer, except in `collect`.

### `GET <root>/auth/slicer?id=<id>` (a page, opened in the system browser)

The provider's own sign-in page. After the person signs in, it asks them to confirm that this
device may act for them, then marks the hand-off as approved. Its design is entirely up to the
provider.

### `POST <root>/api/slicer/device/collect`

```json
{ "id": "h_8f2c…", "secret": "s_41d9…", "label": "Unlayered Slicer 0.2.0 on WORKSHOP-PC" }
```

`label` is for the provider's list of signed-in devices. Answers:

```json
{ "pending": true }
```

```json
{ "token": "dev_…", "account": { "name": "Ada Lovelace", "email": "ada@example.com" } }
```

The slicer polls every 2 seconds for up to 290 seconds, so a provider should keep a hand-off for at
least 5 minutes. A network error (no answer at all) counts as `pending`. Any other non-2xx answer
ends the sign-in, and its message is shown. `token` is the **device token**: long-lived, opaque to
the slicer, and sent as `Authorization: Bearer <token>` on the requests below. `account` is
optional. `email` is used as the display name.

The slicer stores the device token in the operating system's credential store (Windows Credential
Manager, macOS Keychain, Secret Service), under a key that includes the provider's host and port,
so tokens from different providers never mix. If no credential store is available, the token is
kept in memory for the session only and is never written to a file.

---

## 3. `GET <root>/api/slicer/device/me`

Sent at start-up and when the account menu opens. `Authorization: Bearer <device token>`.

```json
{ "name": "Ada Lovelace", "email": "ada@example.com", "edition_label": "" }
```

| Field | Notes |
|---|---|
| `name`, `email` | Shown in the account menu. |
| `edition_label` | Optional string, appended to the application name in the window title (for example "Pro"). Display only: it unlocks nothing. |
| `softwareAccess` | Optional bool. Read only by the app-update channel (§7). |
| `pro` | **Deprecated.** Read only when `edition_label` is absent, as `edition_label: "Pro"`. It unlocks nothing. |

`401` means the person is signed out (the token is deleted). Any other failure leaves the sign-in
as it is: the provider might be briefly unreachable.

## 4. `POST <root>/api/slicer/device/signout`

`Authorization: Bearer <device token>`, body `{}`. The slicer deletes its own copy of the token
**first**, then sends this without waiting for the answer. A provider should revoke the token.

## 5. `POST <root>/api/slicer/slice-token`

A short-lived token for one slicing service. `Authorization: Bearer <device token>`.

```json
{ "audience": "https://slicing.example.com" }
```

```json
{ "token": "eyJhbGciOi…", "expiresAt": 1792540800000 }
```

| Field | Notes |
|---|---|
| `audience` | The slicing service's origin (`scheme://host[:port]`). A provider should bind the token to it (for example as the JWT `aud`). It may also ignore it. |
| `token` | Required. Sent to the service as `Authorization: Bearer <token>`. |
| `expiresAt` | Expiry in **milliseconds since the Unix epoch**. `expires_in` (seconds from now) is also accepted. The slicer asks again every 10 minutes during a long job, or a minute before the stated expiry if that is sooner. |

**Which services get a token.** The slicer asks for one, and sends it, **only** when the service
origin is same-site with the provider (the same scheme and registrable domain, e.g.
`https://slicing.example.com` for provider `https://example.com`; the registrable domain is
approximated as SLICE_SERVICE_PROTOCOL.md §3 describes) or is listed in `service_token_origins`. Every other service gets requests
with no `Authorization` header and does its own authentication. A token is therefore never handed
to a third-party service just because the user typed its address.

A `401`, `402` or `403` here is a refusal by the provider, in the refusal format. Its message (and
action button) is shown when the person pressed Slice; for a background prepare it silently turns
prepares off for this account. A `401` also signs the person out (§1).

## 6. `POST <root>/api/slicer/web-session`

This signs an embedded web page in as the person, without a second password prompt.
`Authorization: Bearer <device token>`.

```json
{ "next": "/library?host=unlayered-slicer" }
```

```json
{ "path": "/api/slicer/web-session?code=c_77ab…" }
```

`path` must start with exactly `/api/slicer/web-session?code=` and contain no whitespace. The
slicer joins it to the provider root and loads it in the embedded page. The provider then sets its
session cookie and redirects to `next`. `code` should be one-use and short-lived.

**Only pages on the provider's own origin** use this. That means a web-app tab or side panel whose
configured URL has the same `scheme://host[:port]` as `account_provider_url`. A page on any other
origin is never signed in by the slicer, and never sees any token. Two moments trigger it:

- the first load of such a page while the person is signed in, once per panel;
- that page navigating to `<root>/login?next=…`. The slicer intercepts the navigation, runs the
  native sign-in (§2) if needed, then follows a session link for `next`. `next` must be a path
  starting with `/`.

If this request fails, the page loads normally and can still offer its own sign-in.

## 7. App-update download links (optional)

Unlayered's own app-update channel may name a download route on the provider. The slicer then calls
`GET <route>` with the device token, **only if the route's host and port equal the provider's**:

```json
{ "url": "https://downloads.example.com/…", "filename": "Setup.exe",
  "sha256": "…", "size": 123456789, "version": "0.2.0" }
```

A provider that does not offer app updates can ignore this section.

---

## 8. Running your own provider

The smallest provider that works with the slicer implements §2 to §5:

1. `handoff/start` creates `{id, secret}` and keeps it for 5 minutes.
2. `/auth/slicer?id=` signs the person in by any means you like, then marks the hand-off as
   approved.
3. `device/collect` returns `{pending: true}` until the hand-off is approved, then a random device
   token and the account once. Store a hash of the token.
4. `device/me` and `device/signout` look up and revoke that token.
5. `slice-token` signs a short JWT with `sub` = the account and `aud` = `audience`, valid for about
   15 minutes, and returns `{token, expiresAt}`.

Then set **Account provider URL** to your root. Host your slicing service on the same site, or
list its origin in `service_token_origins`, so that it receives the tokens.

---

## 9. Implementation

| File | Role |
|---|---|
| `src/slic3r/Utils/UnlayeredAccount.{hpp,cpp}` | every request above, the credential store, the token-forgetting rules |
| `src/slic3r/GUI/AutoUpdater.cpp` | the sign-in dialog and its polling loop; the update download link |
| `src/slic3r/GUI/WebAppPanel.cpp` | `open_signed_in`, the `/login` intercept, the provider-origin check |
| `src/slic3r/GUI/Jobs/NonplanarSliceJob.cpp`, `SlicePrepare.cpp` | when a slice token is requested and sent |
