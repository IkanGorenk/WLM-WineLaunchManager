#include "gog.h"

#include <curl/curl.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <sstream>
#include <utility>

#include <glib.h>

#include "config.h"
#include "util.h"

namespace gog {

namespace {

// GOG's public Galaxy OAuth client - the same one lgogdownloader / Heroic /
// Minigalaxy use. It is not a secret: it ships inside those projects.
constexpr const char* kClientId = "46899977096215655";
constexpr const char* kClientSecret = "9d85c43b1482497dbbce61f6e4aa173a433796eeae2ca8c5f6129f2dc4de46d9";
constexpr const char* kRedirectUri = "https%3A%2F%2Fembed.gog.com%2Fon_login_success%3Forigin%3Dclient";

constexpr const char* kUserAgent = "wine-launcher-manager";

// curl is not thread safe unless it is initialized once, and the GOG calls run
// on worker threads.
void curl_global_init_once() {
    static const bool done = [] {
        curl_global_init(CURL_GLOBAL_DEFAULT);
        return true;
    }();
    (void)done;
}

size_t write_to_string(char* ptr, size_t size, size_t nmemb, void* userdata) {
    static_cast<std::string*>(userdata)->append(ptr, size * nmemb);
    return size * nmemb;
}

struct TransferProgress {
    const ProgressFn* fn;
    curl_off_t already_have;  // bytes on disk before this transfer (resume)
};

int transfer_progress_callback(void* userdata, curl_off_t dl_total, curl_off_t dl_now,
                               curl_off_t /*ul_total*/, curl_off_t /*ul_now*/) {
    auto* p = static_cast<TransferProgress*>(userdata);
    if (p == nullptr || p->fn == nullptr || !*p->fn) return 0;
    // Non-zero aborts the transfer.
    return (*p->fn)(p->already_have + dl_now, dl_total > 0 ? p->already_have + dl_total : 0) ? 0
                                                                                            : 1;
}

// Percent-encodes a query-string value. The auth code and refresh token are
// normally URL-safe already, but a token that picked up a "+" or "/" must not be
// pasted into the query verbatim.
std::string url_encode(const std::string& s) {
    CURL* c = curl_easy_init();
    if (c == nullptr) return s;
    char* escaped = curl_easy_escape(c, s.c_str(), static_cast<int>(s.size()));
    std::string out = escaped != nullptr ? escaped : s;
    curl_free(escaped);
    curl_easy_cleanup(c);
    return out;
}

}  // namespace

// ---------------------------------------------------------------------------
// HTTP
// ---------------------------------------------------------------------------

HttpResponse http_get(const std::string& url, const std::string& bearer) {
    curl_global_init_once();

    HttpResponse out;
    CURL* c = curl_easy_init();
    if (c == nullptr) {
        out.error = "could not initialise libcurl";
        return out;
    }

    curl_slist* headers = nullptr;
    if (!bearer.empty()) {
        headers = curl_slist_append(headers, ("Authorization: Bearer " + bearer).c_str());
    }
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(c, CURLOPT_USERAGENT, kUserAgent);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, write_to_string);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &out.body);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 60L);

    const CURLcode rc = curl_easy_perform(c);
    if (rc != CURLE_OK) {
        out.error = curl_easy_strerror(rc);
    } else {
        curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &out.status);
    }
    curl_slist_free_all(headers);
    curl_easy_cleanup(c);
    return out;
}

bool http_download(const std::string& url, const fs::path& dest, const ProgressFn& progress,
                   std::string* error) {
    curl_global_init_once();

    std::error_code ec;
    fs::create_directories(dest.parent_path(), ec);

    const curl_off_t already_have =
        fs::exists(dest, ec) ? static_cast<curl_off_t>(fs::file_size(dest, ec)) : 0;

    FILE* f = std::fopen(dest.c_str(), already_have > 0 ? "ab" : "wb");
    if (f == nullptr) {
        if (error) *error = "cannot write to " + dest.string();
        return false;
    }

    CURL* c = curl_easy_init();
    if (c == nullptr) {
        std::fclose(f);
        if (error) *error = "could not initialise libcurl";
        return false;
    }

    TransferProgress tp{&progress, already_have};
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_USERAGENT, kUserAgent);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_FAILONERROR, 1L);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, f);
    curl_easy_setopt(c, CURLOPT_RESUME_FROM_LARGE, already_have);
    curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, transfer_progress_callback);
    curl_easy_setopt(c, CURLOPT_XFERINFODATA, &tp);

    const CURLcode rc = curl_easy_perform(c);
    curl_easy_cleanup(c);
    std::fclose(f);

    if (rc != CURLE_OK) {
        // CURLE_ABORTED_BY_CALLBACK is our own cancel, which is not a failure of
        // the transfer itself.
        if (error) {
            *error = (rc == CURLE_ABORTED_BY_CALLBACK) ? std::string("cancelled")
                                                      : std::string(curl_easy_strerror(rc));
        }
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Client
// ---------------------------------------------------------------------------

Client::Client() {
    std::ifstream in(cfg::gog_auth_file);
    if (!in) return;
    std::stringstream ss;
    ss << in.rdbuf();
    tok = json::parse(ss.str(), nullptr, false);
    if (!tok.is_object()) tok = json::object();
}

void Client::save() {
    std::error_code ec;
    fs::create_directories(cfg::directory, ec);
    std::ofstream out(cfg::gog_auth_file);
    if (!out) return;
    out << tok.dump(4) << "\n";
    out.close();
    // The token is a live credential: keep it owner-only.
    fs::permissions(cfg::gog_auth_file,
                    fs::perms::owner_read | fs::perms::owner_write, ec);
}

bool Client::logged_in() {
    std::lock_guard<std::mutex> lock(mu);
    return !json_str(tok, "refresh_token").empty();
}

std::string Client::username() {
    std::lock_guard<std::mutex> lock(mu);
    return json_str(tok, "username");
}

std::string Client::login_url() {
    return std::string("https://auth.gog.com/auth?client_id=") + kClientId +
           "&redirect_uri=" + kRedirectUri + "&response_type=code&layout=galaxy";
}

bool Client::token_request(const std::string& query, std::string* error) {
    // Caller holds `mu`.
    const std::string url = std::string("https://auth.gog.com/token?client_id=") + kClientId +
                            "&client_secret=" + kClientSecret + "&" + query;
    const HttpResponse r = http_get(url);
    if (!r.ok()) {
        if (error) *error = r.error.empty() ? ("HTTP " + std::to_string(r.status)) : r.error;
        return false;
    }
    const json body = json::parse(r.body, nullptr, false);
    if (!body.is_object() || !body.contains("access_token")) {
        if (error) *error = "unexpected response from auth.gog.com";
        return false;
    }
    tok["access_token"] = body.value("access_token", std::string());
    tok["refresh_token"] = body.value("refresh_token", std::string());
    tok["expires_at"] = static_cast<long long>(std::time(nullptr)) +
                        body.value("expires_in", static_cast<long long>(3600));
    if (body.contains("user_id")) tok["user_id"] = body.value("user_id", std::string());
    // GOG only sends the profile here, on the sign-in exchange. A refresh
    // response carries no username, so this is written once and then read back
    // from the token file on later runs. Nothing can re-fetch it: none of the
    // documented account endpoints return it, so losing this field means the
    // window can only say "Signed in to GOG" until the user signs in again.
    if (body.contains("username")) tok["username"] = body.value("username", std::string());
    save();
    return true;
}

bool Client::login(const std::string& code_or_url, std::string* error) {
    std::string code = code_or_url;
    // Accept the whole redirect address, not just the bare code.
    const size_t p = code.find("code=");
    if (p != std::string::npos) {
        code = code.substr(p + 5);
        code = code.substr(0, code.find('&'));
    }
    code.erase(std::remove_if(code.begin(), code.end(),
                              [](unsigned char c) { return std::isspace(c) != 0; }),
               code.end());
    if (code.empty()) {
        if (error) *error = "no authorisation code was given";
        return false;
    }

    std::lock_guard<std::mutex> lock(mu);
    if (!token_request("grant_type=authorization_code&code=" + url_encode(code) +
                           "&redirect_uri=" + kRedirectUri,
                       error)) {
        return false;
    }
    return true;
}

void Client::logout() {
    std::lock_guard<std::mutex> lock(mu);
    tok = json::object();
    save();
}

std::string Client::token() {
    std::lock_guard<std::mutex> lock(mu);
    const std::string refresh = json_str(tok, "refresh_token");
    if (refresh.empty()) return "";
    // Refresh a minute before it actually expires.
    if (json_int(tok, "expires_at") < static_cast<long long>(std::time(nullptr)) + 60) {
        std::string ignored;
        if (!token_request("grant_type=refresh_token&refresh_token=" + url_encode(refresh),
                           &ignored)) {
            return "";
        }
    }
    return json_str(tok, "access_token");
}

std::vector<Game> Client::cached_library() {
    std::vector<Game> out;
    json cached;
    if (!cfg::read_json(cfg::gog_library_file, cached)) return out;
    for (const json& g : json_arr(cached, "games")) {
        if (!g.is_object()) continue;
        out.push_back({json_str(g, "app_id"), json_str(g, "title")});
    }
    return out;
}

std::vector<Game> Client::fetch_library(std::string* error) {
    std::vector<Game> out;
    const std::string access = token();
    if (access.empty()) {
        if (error) *error = "not signed in";
        return out;
    }

    int pages = 1;
    for (int page = 1; page <= pages; ++page) {
        const std::string url =
            "https://embed.gog.com/account/getFilteredProducts?mediaType=1&page=" +
            std::to_string(page);
        const HttpResponse r = http_get(url, access);
        if (!r.ok()) {
            if (error) {
                *error = r.error.empty() ? ("GOG returned HTTP " + std::to_string(r.status))
                                         : r.error;
            }
            return {};
        }
        const json body = json::parse(r.body, nullptr, false);
        if (!body.is_object()) {
            if (error) *error = "unexpected response from GOG";
            return {};
        }
        pages = static_cast<int>(body.value("totalPages", 1LL));
        for (const json& g : json_arr(body, "products")) {
            if (!g.is_object()) continue;
            // GOG ids are numeric but are used as strings everywhere here.
            out.push_back({std::to_string(json_int(g, "id")), json_str(g, "title")});
        }
    }

    std::sort(out.begin(), out.end(),
              [](const Game& a, const Game& b) { return g_ascii_strcasecmp(a.title.c_str(), b.title.c_str()) < 0; });

    // Cache it so the window can show the library instantly next time.
    json cache = json::object();
    cache["fetched_at"] = static_cast<long long>(std::time(nullptr));
    json games = json::array();
    for (const Game& g : out) {
        json item = json::object();
        item["app_id"] = g.app_id;
        item["title"] = g.title;
        item["certificate"] = "";
        games.push_back(item);
    }
    cache["games"] = games;
    cfg::write_json(cfg::gog_library_file, cache);

    return out;
}

std::vector<InstallerFile> Client::installer_files(const std::string& app_id, std::string* error) {
    std::vector<InstallerFile> out;
    const std::string access = token();
    if (access.empty()) {
        if (error) *error = "not signed in";
        return out;
    }

    const HttpResponse r =
        http_get("https://api.gog.com/products/" + app_id + "?expand=downloads", access);
    if (!r.ok()) {
        if (error) {
            *error = r.error.empty() ? ("GOG returned HTTP " + std::to_string(r.status))
                                     : r.error;
        }
        return out;
    }
    const json body = json::parse(r.body, nullptr, false);
    if (!body.is_object()) {
        if (error) *error = "unexpected response from GOG";
        return out;
    }

    // The Windows offline installer, preferring English. The chosen one is
    // copied out of the loop because a json reference would dangle as soon as
    // the loop's iterator moved on.
    const json installers = json_arr(json_obj(body, "downloads"), "installers");
    json best;
    for (const json& inst : installers) {
        if (!inst.is_object()) continue;
        if (json_str(inst, "os") != "windows") continue;
        const std::string language = json_str(inst, "language");
        if (best.is_null() || language == "en") best = inst;
        if (language == "en") break;
    }
    if (best.is_null()) {
        if (error) *error = "no Windows installer is offered for this game";
        return out;
    }

    for (const json& f : json_arr(best, "files")) {
        if (!f.is_object()) continue;
        const std::string downlink = json_str(f, "downlink");
        if (downlink.empty()) continue;
        out.push_back({downlink, json_int(f, "size")});
    }
    if (out.empty() && error) *error = "the installer has no downloadable files";
    return out;
}

ResolvedLink Client::resolve(const std::string& downlink) {
    ResolvedLink out;
    const HttpResponse r = http_get(downlink, token());
    if (!r.ok()) return out;
    const json body = json::parse(r.body, nullptr, false);
    if (!body.is_object()) return out;
    out.url = json_str(body, "downlink");
    if (out.url.empty()) return out;

    // The file name is the last path segment, URL-decoded.
    const size_t query = out.url.find('?');
    const std::string path = out.url.substr(0, query);
    const size_t slash = path.rfind('/');
    const std::string raw = slash == std::string::npos ? path : path.substr(slash + 1);
    gchar* decoded = g_uri_unescape_string(raw.c_str(), nullptr);
    out.name = decoded != nullptr ? decoded : raw;
    g_free(decoded);
    return out;
}

}  // namespace gog
