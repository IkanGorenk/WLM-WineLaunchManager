#pragma once

// GOG Galaxy integration: OAuth sign-in, the account's game library, and the
// Windows offline installer parts for a chosen game.
//
// The OAuth client id/secret below are GOG's PUBLIC Galaxy client (the same one
// lgogdownloader, Heroic and Minigalaxy use), so there is no per-user secret to
// ship. Tokens are cached in ~/wlm/gog_auth.json and the library in
// ~/wlm/gog_library.json, so the launcher stays signed in between runs.

#include <gtk/gtk.h>

#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "config.h"

namespace gog {

namespace fs = std::filesystem;

struct Game {
    std::string app_id;
    std::string title;
};

// One part of a game's Windows offline installer.
struct InstallerFile {
    std::string downlink;  // GOG "downlink" API url, resolved later
    long long size = 0;
};

// A downlink resolved to the real CDN url plus a usable file name.
struct ResolvedLink {
    std::string url;
    std::string name;
};

// Progress callback for a download: returns false to abort. Called from the
// worker thread only, with bytes done / total (total is 0 when unknown).
using ProgressFn = std::function<bool(long long done, long long total)>;

// ---- HTTP -------------------------------------------------------------------
// The launcher's Proton downloads shell out to curl; the GOG API needs bearer
// auth, redirects and a resumable body, so it gets a real libcurl client here.

struct HttpResponse {
    long status = 0;
    std::string body;
    std::string error;  // transport-level failure (no HTTP response)
    bool ok() const { return error.empty() && status >= 200 && status < 300; }
};

HttpResponse http_get(const std::string& url, const std::string& bearer = "");

// Resumable download to `dest`. Returns false when `progress` aborted it or the
// transfer failed. Already-complete files (matching size) are kept.
bool http_download(const std::string& url, const fs::path& dest, const ProgressFn& progress,
                   std::string* error);

// ---- client -----------------------------------------------------------------

class Client {
public:
    Client();

    bool logged_in();
    std::string username();

    // The URL the user must visit to sign in. With the embedded browser the
    // redirect back to GOG is watched and the code is grabbed automatically.
    static std::string login_url();

    // Accepts the bare code or the whole redirect URL.
    bool login(const std::string& code_or_url, std::string* error);

    void logout();

    // The cached library (~/wlm/gog_library.json), so the window can show
    // something immediately without a network round trip.
    std::vector<Game> cached_library();

    // Fetches the account's games from GOG and refreshes the cache.
    std::vector<Game> fetch_library(std::string* error);

    // The Windows offline installer parts, preferring English.
    std::vector<InstallerFile> installer_files(const std::string& app_id, std::string* error);

    ResolvedLink resolve(const std::string& downlink);

    // Makes sure a token is available, refreshing it when it is about to expire.
    std::string token();

private:
    bool token_request(const std::string& query, std::string* error);
    void save();

    json tok;
    std::mutex mu;
};

}  // namespace gog
