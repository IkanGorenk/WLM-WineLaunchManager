#include "gog_ui.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "actions.h"
#include "config.h"
#include "dialogs.h"
#include "gog.h"
#include "prefix_manager.h"
#include "proton.h"
#include "scripts.h"
#include "task_log.h"
#include "ui.h"
#include "util.h"
#include "window_mode.h"

#ifdef HAVE_WEBKIT
#include <webkit2/webkit2.h>
#endif

namespace gogui {
namespace {

namespace fs = std::filesystem;

enum { COL_TITLE = 0, COL_APP_ID, COL_COUNT };

// The window's state. Lives as long as the window does.
struct Window {
    GtkWidget* window = nullptr;
    GtkWidget* sign_button = nullptr;
    GtkWidget* embedded_button = nullptr;
    GtkWidget* who_label = nullptr;
    GtkWidget* status_label = nullptr;
    GtkWidget* tree = nullptr;
    GtkListStore* store = nullptr;
    GtkWidget* empty_label = nullptr;
    GtkWidget* install_button = nullptr;
    gog::Client gog;
    std::vector<gog::Game> games;
    // An install is running: the window must not start a second one.
    bool installing = false;
};

Window* g_win = nullptr;

// The install in flight, or nullptr. Freed when the flow finishes.
struct Install {
    std::shared_ptr<tasklog::Task> task;
    prefix::PrefixEntry prefix;
    gog::Game game;
    fs::path download_dir;
    std::string installer;
};
std::unique_ptr<Install> g_install;

// ---------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------

std::string sanitize(std::string s) {
    for (char& c : s) {
        if (!g_ascii_isalnum(c) && c != '-' && c != ' ') c = '_';
    }
    while (!s.empty() && g_ascii_isspace(s.front())) s.erase(s.begin());
    while (!s.empty() && g_ascii_isspace(s.back())) s.pop_back();
    if (s.size() > 80) s.resize(80);
    return s.empty() ? std::string("gog-game") : s;
}

// The game name shown in the list, and the launch script's file name. This is
// the same rule actions::add_script() uses (it keeps letters, digits, spaces,
// "_" and "-"), so a GOG game and a hand-added game cannot collide on a name
// that one of them would refuse.
std::string sanitize_script_name(const std::string& input) {
    std::string out;
    for (char c : input) {
        if (std::isalnum(static_cast<unsigned char>(c)) || c == ' ' || c == '_' || c == '-') {
            out.push_back(c);
        }
    }
    const size_t begin = out.find_first_not_of(" \t");
    if (begin == std::string::npos) return "";
    const size_t end = out.find_last_not_of(" \t");
    return out.substr(begin, end - begin + 1);
}

bool ends_with_ci(const std::string& s, const std::string& suffix) {
    if (s.size() < suffix.size()) return false;
    gchar* lowered = g_ascii_strdown(s.c_str() + s.size() - suffix.size(), -1);
    const bool hit = suffix == lowered;
    g_free(lowered);
    return hit;
}

void set_status(const std::string& text) {
    if (g_win != nullptr && g_win->status_label != nullptr) {
        gtk_label_set_text(GTK_LABEL(g_win->status_label), text.c_str());
    }
}

const gog::Game* selected_game() {
    if (g_win == nullptr) return nullptr;
    GtkTreeSelection* sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(g_win->tree));
    GtkTreeModel* model = nullptr;
    GtkTreeIter iter;
    if (!gtk_tree_selection_get_selected(sel, &model, &iter)) return nullptr;

    gchar* app_id = nullptr;
    gtk_tree_model_get(model, &iter, COL_APP_ID, &app_id, -1);
    if (app_id == nullptr) return nullptr;
    const std::string wanted = app_id;
    g_free(app_id);

    for (const gog::Game& g : g_win->games) {
        if (g.app_id == wanted) return &g;
    }
    return nullptr;
}

void update_sign_in_ui() {
    if (g_win == nullptr) return;
    Window* w = g_win;
    const bool signed_in = w->gog.logged_in();
    gtk_button_set_label(GTK_BUTTON(w->sign_button), signed_in ? "Sign out" : "Sign in to GOG");

    const std::string who = w->gog.username();
    const std::string who_text = !signed_in   ? "Not signed in to GOG"
                                 : who.empty() ? "Signed in to GOG"
                                               : "Signed in as " + who;
    gtk_label_set_text(GTK_LABEL(w->who_label), who_text.c_str());

    gtk_widget_set_sensitive(GTK_WIDGET(w->install_button),
                             signed_in && !w->games.empty() && !w->installing);
    if (w->embedded_button != nullptr) {
        // Meaningless while signed in, and pointless when WebKit is not compiled in.
#ifdef HAVE_WEBKIT
        gtk_widget_set_sensitive(GTK_WIDGET(w->embedded_button), !signed_in);
#else
        gtk_widget_set_sensitive(GTK_WIDGET(w->embedded_button), FALSE);
        gtk_widget_set_tooltip_text(w->embedded_button,
                                    "This build has no WebKit, so the embedded page is not "
                                    "available. The normal browser works.");
#endif
    }
}

void set_installing(bool busy) {
    if (g_win == nullptr) return;
    g_win->installing = busy;
    update_sign_in_ui();
}

void show_library(const std::vector<gog::Game>& games) {
    if (g_win == nullptr) return;
    g_win->games = games;
    gtk_list_store_clear(g_win->store);
    for (const gog::Game& g : games) {
        GtkTreeIter row;
        gtk_list_store_append(g_win->store, &row);
        gtk_list_store_set(g_win->store, &row, COL_TITLE, g.title.c_str(), COL_APP_ID,
                           g.app_id.c_str(), -1);
    }
    gtk_widget_set_visible(g_win->empty_label, games.empty());
    update_sign_in_ui();
}

// ---------------------------------------------------------------------------
// sign-in
// ---------------------------------------------------------------------------

void refresh_library_async();  // forward

// The sign-in without the embedded browser: GOG's page opens in the normal
// browser, and the redirect address is pasted back here. This is the fallback
// whenever the embedded page cannot be used - no WebKit at build time, a
// renderer the driver cannot handle, or GOG serving its own error page.
void login_via_browser(GtkWindow* parent) {
    if (parent == nullptr) return;

    dialogs::show_info(
        "Sign in to GOG",
        "A GOG page will open in your normal browser.\n\n"
        "Sign in there, then copy the address bar - it starts with\n"
        "  https://embed.gog.com/on_login_success?code=...\n"
        "and paste it into the next box.", parent);

    gtk_show_uri_on_window(parent, gog::Client::login_url().c_str(), GDK_CURRENT_TIME, nullptr);

    const std::optional<std::string> pasted =
        dialogs::ask_string("Paste the redirect address", "Address after signing in:", "", parent);
    if (!pasted || pasted->empty()) return;

    if (g_win != nullptr) set_status("Signing in...");
    // Exchange the code on a worker so the window stays responsive.
    std::thread([pasted] {
        gog::Client c;
        std::string err;
        const bool ok = c.login(*pasted, &err);
        g_idle_add(
            [](gpointer data) -> gboolean {
                auto* pack = static_cast<std::pair<bool, std::string>*>(data);
                if (g_win != nullptr) {
                    if (pack->first) {
                        set_status("Signed in. Loading your library...");
                        refresh_library_async();
                    } else {
                        update_sign_in_ui();
                        dialogs::show_error("Sign-in failed",
                                            "GOG did not accept that code.\n\n" + pack->second);
                    }
                }
                delete pack;
                return G_SOURCE_REMOVE;
            },
            new std::pair<bool, std::string>(ok, err));
    }).detach();
}

// WebKit keeps a separate web process alive for as long as the application
// runs, and it is by far the largest thing this launcher would ever hold -
// typically a couple of hundred megabytes. The login page is the only thing
// that needs it, so it is shut down the moment that window goes away and starts
// again on the next sign-in.
void release_webkit_processes(GtkWidget* login_window) {
#ifdef HAVE_WEBKIT
    if (login_window == nullptr) return;
    // The view is the handle WebKit provides for exactly this.
    auto* view = WEBKIT_WEB_VIEW(g_object_get_data(G_OBJECT(login_window), "gog-webview"));
    if (view != nullptr) webkit_web_view_terminate_web_process(view);
#endif
}

#ifdef HAVE_WEBKIT
// GOG ends the sign-in by redirecting to embed.gog.com/on_login_success?code=…
// Watching for that redirect is what makes the embedded browser automatic.
//
// `win` is the sign-in window and "done" is set the first time the redirect is
// seen. Both decide-policy AND load-changed report the same navigation, so
// without that guard the sign-in starts twice - and the authorisation code is
// single-use, so the second attempt fails and the launcher ends up looking
// signed out. Same guard glauncher uses.
void on_login_redirect(GtkWidget* win, const std::string& uri) {
    if (win == nullptr || g_object_get_data(G_OBJECT(win), "done") != nullptr) return;
    if (uri.find("on_login_success") == std::string::npos) return;
    if (uri.find("code=") == std::string::npos) return;
    g_object_set_data(G_OBJECT(win), "done", GINT_TO_POINTER(1));

    // Close the window on the main thread, where its widgets live.
    g_object_ref(win);
    g_idle_add(
        [](gpointer data) -> gboolean {
            auto* win = GTK_WIDGET(data);
            gtk_widget_destroy(win);
            g_object_unref(win);
            return G_SOURCE_REMOVE;
        },
        win);

    std::thread([uri] {
        gog::Client client;
        std::string error;
        const bool ok = client.login(uri, &error);
        g_idle_add(
            [](gpointer data) -> gboolean {
                auto* pack = static_cast<std::pair<bool, std::string>*>(data);
                if (pack->first) {
                    set_status("Signed in. Loading your library...");
                    refresh_library_async();
                } else {
                    update_sign_in_ui();
                    dialogs::show_error("Sign-in failed",
                                        "GOG did not accept that code.\n\n" + pack->second);
                }
                delete pack;
                return G_SOURCE_REMOVE;
            },
            new std::pair<bool, std::string>(ok, error));
    }).detach();
}

void wk_check_uri(GtkWidget* win, const char* uri) {
    if (uri != nullptr) on_login_redirect(win, uri);
}

void on_decide_policy(WebKitWebView*, WebKitPolicyDecision* decision,
                      WebKitPolicyDecisionType type, gpointer win) {
    if (type != WEBKIT_POLICY_DECISION_TYPE_NAVIGATION_ACTION) return;
    auto* nav = webkit_navigation_policy_decision_get_navigation_action(
        WEBKIT_NAVIGATION_POLICY_DECISION(decision));
    const char* uri = webkit_uri_request_get_uri(webkit_navigation_action_get_request(nav));
    wk_check_uri(GTK_WIDGET(win), uri);
}

// GOG answers with its own error page when its login backend is having trouble
// ("Error 503 - Backend is unhealthy", a Cloudflare 54113 page). That is a
// server-side problem, not a broken launcher, and reloading usually does not
// help - so say so instead of leaving the user staring at GOG's error page.
void on_load_changed(WebKitWebView* view, WebKitLoadEvent event, gpointer win) {
    wk_check_uri(GTK_WIDGET(win), webkit_web_view_get_uri(view));
    if (event != WEBKIT_LOAD_FINISHED) return;

    const char* title = webkit_web_view_get_title(view);
    if (title == nullptr) return;
    const std::string t = title;
    if (t.find("503") == std::string::npos && util::lower(t).find("unhealthy") == std::string::npos) {
        return;
    }

    GtkWidget* top = gtk_widget_get_toplevel(GTK_WIDGET(view));
    if (top == nullptr || g_object_get_data(G_OBJECT(top), "gog-error-shown") != nullptr) return;
    g_object_set_data(G_OBJECT(top), "gog-error-shown", GINT_TO_POINTER(1));

    dialogs::show_info(
        "GOG's servers are not responding",
        "GOG's login page is showing its own error:\n\n  " + t +
            "\n\nThis is a problem on GOG's side, not with this launcher - it often clears by "
            "itself after a short while.\n\n"
            "You can close this window and try again in a moment, or press\n"
            "\"Use my normal browser instead\" below to sign in through your usual browser.",
        GTK_WINDOW(top));
}

void on_browser_button_clicked(GtkButton*, gpointer data) {
    GtkWidget* win = GTK_WIDGET(data);
    if (win == nullptr) return;
    GtkWindow* parent = GTK_WINDOW(g_object_get_data(G_OBJECT(win), "gog-parent"));
    gtk_widget_destroy(win);
    login_via_browser(parent);
}

void web_login(GtkWindow* parent) {
    GtkWidget* win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(win), "Sign in to GOG");
    gtk_window_set_default_size(GTK_WINDOW(win), winmode::px(900), winmode::px(700));
    gtk_window_set_transient_for(GTK_WINDOW(win), parent);
    gtk_window_set_modal(GTK_WINDOW(win), TRUE);
    gtk_window_set_position(GTK_WINDOW(win), GTK_WIN_POS_CENTER_ON_PARENT);
    g_object_set_data(G_OBJECT(win), "gog-parent", parent);

    // Every way out of this window - the sign-in redirect, the browser fallback
    // button, the WM's close button - has to let go of WebKit's processes, or
    // they stay resident for the rest of the session.
    g_signal_connect(win, "destroy",
                     G_CALLBACK(+[](GtkWidget* w, gpointer) { release_webkit_processes(w); }),
                     nullptr);
    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, winmode::px(6));
    gtk_container_set_border_width(GTK_CONTAINER(box), winmode::px(8));
    gtk_box_set_spacing(GTK_BOX(box), winmode::px(6));
    gtk_container_add(GTK_CONTAINER(win), box);

    GtkWidget* label = gtk_label_new("Signing you in to GOG - the window closes by itself.");
    gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
    gtk_box_pack_start(GTK_BOX(box), label, FALSE, FALSE, 0);

    // Both handlers need the window: it owns the "already handled" flag, and it
    // is the window that gets closed on a successful redirect.
    GtkWidget* view = webkit_web_view_new();
    g_signal_connect(view, "decide-policy", G_CALLBACK(on_decide_policy), win);
    g_signal_connect(view, "load-changed", G_CALLBACK(on_load_changed), win);
    // Kept so the window's destroy handler can shut down WebKit's web process
    // when this window goes away.
    g_object_set_data(G_OBJECT(win), "gog-webview", view);
    gtk_box_pack_start(GTK_BOX(box), view, TRUE, TRUE, 0);

    // The embedded page is a convenience, not a requirement: if it will not
    // cooperate (old driver, GOG's error page, anything else) the user can sign
    // in through their own browser instead.
    GtkWidget* fallback_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, winmode::px(6));
    gtk_widget_set_halign(fallback_row, GTK_ALIGN_CENTER);
    gtk_box_pack_start(GTK_BOX(box), fallback_row, FALSE, FALSE, 0);

    GtkWidget* fallback_hint = gtk_label_new("Nothing loading, or an error page?");
    gtk_label_set_xalign(GTK_LABEL(fallback_hint), 1.0f);
    gtk_box_pack_start(GTK_BOX(fallback_row), fallback_hint, FALSE, FALSE, 0);

    GtkWidget* fallback_btn = gtk_button_new_with_label("Use my normal browser instead");
    g_signal_connect(fallback_btn, "clicked", G_CALLBACK(on_browser_button_clicked), win);
    gtk_box_pack_start(GTK_BOX(fallback_row), fallback_btn, FALSE, FALSE, 0);

    gtk_widget_show_all(win);
    webkit_web_view_load_uri(WEBKIT_WEB_VIEW(view), gog::Client::login_url().c_str());
}
#endif  // HAVE_WEBKIT

void on_sign_clicked(GtkButton*, gpointer) {
    if (g_win == nullptr) return;
    if (g_win->gog.logged_in()) {
        g_win->gog.logout();
        show_library({});
        set_status("Signed out.");
        return;
    }

    // The normal browser, not the embedded page: WebKitGTK maps roughly 90 MB
    // into this process as soon as a web view exists, and its web and network
    // child processes add up to ~300 MB more while that page is open. The
    // embedded page is a convenience, so it is the opt-in one.
    login_via_browser(GTK_WINDOW(g_win->window));
}

// The opt-in embedded sign-in page. It costs a few hundred megabytes while it is
// open, which is why it is not the default.
void on_embedded_sign_clicked(GtkButton*, gpointer) {
    if (g_win == nullptr) return;
    if (g_win->gog.logged_in()) {
        on_sign_clicked(nullptr, nullptr);
        return;
    }
#ifdef HAVE_WEBKIT
    web_login(GTK_WINDOW(g_win->window));
#endif
}

void on_refresh_library(GtkButton*, gpointer) { refresh_library_async(); }

void refresh_library_async() {
    if (g_win == nullptr) return;
    set_status("Loading your GOG library...");
    gtk_widget_set_sensitive(GTK_WIDGET(g_win->sign_button), FALSE);

    std::thread([] {
        gog::Client client;
        std::string error;
        const std::vector<gog::Game> games = client.fetch_library(&error);
        g_idle_add(
            [](gpointer data) -> gboolean {
                auto* pack = static_cast<std::pair<std::vector<gog::Game>, std::string>*>(data);
                if (g_win == nullptr) {
                    delete pack;
                    return G_SOURCE_REMOVE;
                }
                gtk_widget_set_sensitive(GTK_WIDGET(g_win->sign_button), TRUE);
                if (pack->first.empty()) {
                    update_sign_in_ui();
                    set_status("Could not load the library.");
                    dialogs::show_error(
                        "Could not load your GOG library",
                        pack->second.empty()
                            ? "GOG returned no games. The sign-in may have expired - sign out and "
                              "sign in again."
                            : pack->second,
                        GTK_WINDOW(g_win->window));
                } else {
                    show_library(pack->first);
                    set_status(std::to_string(pack->first.size()) + " games in your library.");
                }
                delete pack;
                return G_SOURCE_REMOVE;
            },
            new std::pair<std::vector<gog::Game>, std::string>(games, error));
    }).detach();
}

// ---------------------------------------------------------------------------
// choosing a prefix
// ---------------------------------------------------------------------------

// Only prefixes that can actually run something are offered: a Proton prefix
// needs its build recorded, otherwise there is nothing to launch.
std::optional<prefix::PrefixEntry> choose_prefix(GtkWindow* parent) {
    const std::vector<prefix::PrefixEntry> all = prefix::list_all_known_prefixes();
    std::vector<prefix::PrefixEntry> usable;
    for (const prefix::PrefixEntry& e : all) {
        if (e.runner != "wine" && e.proton_path.empty()) continue;
        usable.push_back(e);
    }

    if (usable.empty()) {
        dialogs::show_error(
            "No usable prefix",
            "A GOG game has to be installed into a prefix, and none of the known prefixes can run "
            "a game yet.\n\n"
            "Make one first: play any game once and let WLM create a prefix, or add one in "
            "SETTINGS -> \"Prefix Configuration Manager...\".",
            parent);
        return std::nullopt;
    }

    GtkWidget* dialog = gtk_dialog_new_with_buttons(
        "Install into which prefix?", parent,
        static_cast<GtkDialogFlags>(GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT), "_Install",
        GTK_RESPONSE_OK, "_Cancel", GTK_RESPONSE_CANCEL, NULL);
    gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_OK);
    gtk_window_set_position(GTK_WINDOW(dialog), GTK_WIN_POS_CENTER_ON_PARENT);

    GtkWidget* content = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    gtk_container_set_border_width(GTK_CONTAINER(content), winmode::px(15));
    gtk_box_set_spacing(GTK_BOX(content), winmode::px(8));

    GtkWidget* label = gtk_label_new(
        "The GOG installer will run inside this prefix, so the game ends up there.\n"
        "Which prefix should this game live in?");
    gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
    gtk_box_pack_start(GTK_BOX(content), label, FALSE, FALSE, 0);

    GtkWidget* combo = gtk_combo_box_text_new();
    for (const prefix::PrefixEntry& e : usable) {
        std::string text = cfg::runner_display_name(e.runner) + " - " + e.prefix_code;
        if (!e.proton_name.empty()) text += "  (" + e.proton_name + ")";
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo), text.c_str());
    }
    gtk_combo_box_set_active(GTK_COMBO_BOX(combo), 0);
    gtk_box_pack_start(GTK_BOX(content), combo, FALSE, FALSE, 0);

    std::optional<prefix::PrefixEntry> chosen;
    gtk_widget_show_all(dialog);
    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_OK) {
        const int index = gtk_combo_box_get_active(GTK_COMBO_BOX(combo));
        if (index >= 0 && index < static_cast<int>(usable.size())) chosen = usable[index];
    }
    gtk_widget_destroy(dialog);
    return chosen;
}

// ---------------------------------------------------------------------------
// running a program inside a prefix
// ---------------------------------------------------------------------------

// The same command a launch script would run for this prefix: wine with
// WINEPREFIX, or the Proton build with STEAM_COMPAT_DATA_PATH.
std::vector<std::string> launch_argv(const prefix::PrefixEntry& entry, const std::string& exe) {
    if (entry.runner == "wine") return {"wine", exe};
    return {entry.proton_path, "run", exe};
}

util::EnvMap launch_env(const prefix::PrefixEntry& entry) {
    util::EnvMap env;
    if (entry.runner == "wine") {
        env["WINEPREFIX"] = entry.prefix_path;
        env["WINEDEBUG"] = "-all";
    } else {
        env["STEAM_COMPAT_DATA_PATH"] = entry.prefix_path;
        env["STEAM_COMPAT_CLIENT_INSTALL_PATH"] = proton::find_steam_install_path().string();
    }
    return env;
}

std::string describe_command(const prefix::PrefixEntry& entry, const std::string& exe) {
    if (entry.runner == "wine") return "wine " + exe;
    return (entry.proton_name.empty() ? std::string("proton") : entry.proton_name) + " run " + exe;
}

// ---------------------------------------------------------------------------
// the install flow
// ---------------------------------------------------------------------------

// Downloads every part of the game's Windows offline installer, then hands
// back to the main thread. Worker thread.
void download_worker(Install* install);

// Runs the downloaded setup, then asks for the game's .exe and adds it as a
// normal game. Main thread; the modal dialogs each run their own loop.
void run_installer_flow();

void download_worker(Install* install) {
    const tasklog::TaskPtr& task = install->task;
    gog::Client client;

    try {
        task->append_line("=== " + install->game.title + "  (GOG " + install->game.app_id + ") ===");
        task->append_line("");
        task->append_line("Asking GOG which files make up the installer...");

        std::string error;
        const std::vector<gog::InstallerFile> files =
            client.installer_files(install->game.app_id, &error);
        if (files.empty()) {
            throw std::runtime_error(error.empty() ? "GOG offered no installer files." : error);
        }

        long long total_size = 0;
        for (const gog::InstallerFile& f : files) total_size += f.size;
        task->append_line(std::to_string(files.size()) + " file(s), " +
                          util::human_size(static_cast<double>(total_size)) + " in total.");
        task->append_line("Downloading into " + install->download_dir.string());
        task->append_line("");

        // One percentage across every part, and a byte rate sampled on a short
        // window so the figure settles instead of jumping on every chunk.
        long long bytes_before_this_file = 0;
        double last_sample = 0.0;
        long long last_sample_bytes = 0;
        int last_pct = -1;

        for (size_t i = 0; i < files.size(); ++i) {
            if (task->cancel_requested()) throw std::runtime_error("cancelled");

            const gog::ResolvedLink link = client.resolve(files[i].downlink);
            if (link.url.empty()) throw std::runtime_error("could not get a download link.");

            const fs::path dest = install->download_dir / link.name;
            if (ends_with_ci(link.name, ".exe")) install->installer = dest.string();

            std::error_code ec;
            if (fs::exists(dest, ec)) {
                const long long on_disk = static_cast<long long>(fs::file_size(dest, ec));
                if (on_disk == files[i].size) {
                    task->append_line("  = " + link.name + "  (already downloaded)");
                    bytes_before_this_file += files[i].size;
                    continue;
                }
                if (on_disk > files[i].size) {
                    // Bigger than it should be: a leftover from an older build of
                    // this game that reused the file name. Resuming onto it would
                    // splice two different installers together, so start over.
                    task->append_line("  ! " + link.name +
                                      "  (a larger leftover file - fetching it again)");
                    fs::remove(dest, ec);
                }
            }

            task->append_line("  > " + link.name + "  (" +
                              util::human_size(static_cast<double>(files[i].size)) + ")   [" +
                              std::to_string(i + 1) + " of " + std::to_string(files.size()) + "]");

            long long file_done = 0;
            std::string dl_error;
            const bool ok = gog::http_download(
                link.url, dest,
                [&](long long done, long long /*total*/) {
                    if (task->cancel_requested()) return false;

                    // `done` is this file's bytes; the overall figure adds up
                    // everything already finished, so the bar covers the whole
                    // download rather than restarting per part.
                    file_done = done;
                    const double overall =
                        (static_cast<double>(bytes_before_this_file) +
                         static_cast<double>(file_done)) /
                        static_cast<double>(total_size > 0 ? total_size : 1);
                    const int pct = static_cast<int>(overall * 100.0);
                    if (pct != last_pct) {
                        last_pct = pct;
                        task->set_progress(pct);
                    }

                    const double now = g_get_monotonic_time() / 1000000.0;
                    if (now - last_sample > 0.4) {
                        const long long total_done = bytes_before_this_file + file_done;
                        if (last_sample > 0.0) {
                            const double rate =
                                static_cast<double>(total_done - last_sample_bytes) /
                                (now - last_sample);
                            task->set_speed(rate);
                        }
                        last_sample = now;
                        last_sample_bytes = total_done;
                    }
                    return true;
                },
                &dl_error);

            if (!ok) {
                if (task->cancel_requested()) throw std::runtime_error("cancelled");
                throw std::runtime_error("could not download " + link.name + ": " + dl_error);
            }
            bytes_before_this_file += files[i].size;
            task->append_line("  < " + link.name + "  done");
        }

        task->set_progress(100);
        task->set_speed(0);
        if (install->installer.empty()) {
            throw std::runtime_error("no setup .exe was found in the downloaded files.");
        }
        task->append_line("");
        task->append_line("All " + std::to_string(files.size()) + " file(s) downloaded.");
    } catch (const std::exception& e) {
        const std::string message = e.what();
        g_idle_add(
            [](gpointer data) -> gboolean {
                auto* text = static_cast<std::string*>(data);
                if (g_install != nullptr) {
                    g_install->task->append_line("");
                    g_install->task->append_line("FAILED: " + *text);
                    g_install->task->set_busy();
                    g_install->task->mark_finished();
                }
                if (g_win != nullptr) {
                    set_installing(false);
                    set_status(std::string("Install failed: ") + *text);
                    dialogs::show_error("GOG install failed", *text,
                                        GTK_WINDOW(g_win->window));
                }
                delete text;
                g_install.reset();
                return G_SOURCE_REMOVE;
            },
            new std::string(message));
        return;
    }

    // Downloads are done; the installer itself has to run on the main thread.
    g_idle_add(
        [](gpointer) -> gboolean {
            if (g_install == nullptr) return G_SOURCE_REMOVE;
            run_installer_flow();
            return G_SOURCE_REMOVE;
        },
        nullptr);
}

// Adds the installed game to the normal game list, the same way "Add Game" does
// but with the prefix already known.
bool add_installed_game(const std::string& script_name, const fs::path& exe,
                        const prefix::PrefixEntry& prefix) {
    const std::string safe_name = sanitize_script_name(script_name);
    if (safe_name.empty()) {
        dialogs::show_error("Error", "That name cannot be used for a game.");
        return false;
    }

    const fs::path script_path = cfg::bashlaunch_dir / (safe_name + ".sh");
    if (fs::exists(script_path)) {
        if (!dialogs::ask_yes_no("Name Already Exists",
                                 "A game named '" + safe_name + "' already exists.\n\n"
                                 "Replace it? Its old launch script and runner settings will be "
                                 "overwritten.",
                                 ui::window)) {
            return false;
        }
    }

    const fs::path folder = util::resolve_path(exe.parent_path());
    const fs::path exe_resolved = util::resolve_path(exe);

    scripts::RunnerChoice choice;
    choice.runner = prefix.runner;
    choice.prefix_code = prefix.prefix_code;
    choice.prefix_path = prefix.prefix_path;
    choice.proton_name = prefix.proton_name;
    choice.proton_path = prefix.proton_path;
    choice.comment = "Installed from GOG.";

    try {
        {
            std::ofstream out(script_path);
            out << scripts::build_script_content(folder.string(), exe_resolved.string(), choice);
        }
        util::make_executable(script_path);

        json runner_cfg = cfg::load_runner_config();
        runner_cfg[safe_name] = scripts::choice_to_json(choice);
        cfg::save_runner_config(runner_cfg);
    } catch (const std::exception& e) {
        dialogs::show_error("Error", std::string("Could not create the launch script:\n") + e.what(),
                            ui::window);
        return false;
    }

    ui::update_script_list();
    ui::set_status("Added from GOG: " + safe_name, "success");
    return true;
}

void run_installer_flow() {
    if (g_install == nullptr) return;
    Install& install = *g_install;
    const tasklog::TaskPtr& task = install.task;
    GtkWindow* parent = g_win != nullptr ? GTK_WINDOW(g_win->window) : ui::window;

    // Nothing left to count against: the bar pulses for the rest of the flow.
    task->set_busy();

    if (!dialogs::ask_yes_no("Run the GOG installer?",
                             "The installer for \"" + install.game.title + "\" has finished "
                             "downloading.\n\n"
                             "Run it now? It installs the game into:\n  " +
                                 install.prefix.prefix_path + "\n\n"
                                 "It opens as a normal installer, exactly like running setup.exe "
                                 "inside the prefix - nothing is installed silently.",
                             parent)) {
        task->append_line("");
        task->append_line("The installer files were kept in " + install.download_dir.string() +
                          ". Run setup again from here to install.");
        task->mark_finished();
        set_installing(false);
        set_status("Downloaded. Install it later from " + install.download_dir.string() + ".");
        g_install.reset();
        return;
    }

    task->append_line("");
    task->append_line("Running the installer:");
    task->append_line("  " + describe_command(install.prefix, install.installer));
    task->append_line("");
    task->append_line("The GOG installer window should open now. Continue there; this launcher "
                      "waits for it to close.");

    // A normal, visible installer: no silent flags, and it is waited for so the
    // game can be located afterwards.
    int exit_code = -1;
    int spawn_errno = 0;
    util::run_captured(launch_argv(install.prefix, install.installer),
                       launch_env(install.prefix), &exit_code, nullptr, &spawn_errno);
    if (spawn_errno != 0) {
        task->append_line(std::string("Could not even start the installer: ") +
                          std::strerror(spawn_errno));
    } else {
        task->append_line("The installer closed (exit code " + std::to_string(exit_code) + ").");
    }

    // The user picks the game executable - GOG's install layout varies and a
    // silent guess would be wrong more often than not.
    fs::path drive_c = fs::path(install.prefix.prefix_path) / "drive_c";
    std::error_code ec;
    if (!fs::is_directory(drive_c, ec)) {
        const fs::path pfx_layout = fs::path(install.prefix.prefix_path) / "pfx" / "drive_c";
        if (fs::is_directory(pfx_layout, ec)) drive_c = pfx_layout;
    }

    const std::optional<std::string> exe = dialogs::choose_open_file(
        "Select the installed game's executable",
        "Windows Executables (.exe)", {"*.exe"}, parent, drive_c.string());
    if (!exe) {
        task->append_line("");
        task->append_line("No game executable was selected, so nothing was added to the game "
                          "list.");
        task->append_line("The installer files are still in " + install.download_dir.string() + ".");
        task->mark_finished();
        set_installing(false);
        set_status("Installed, but no game was added.");
        dialogs::show_info("No game added",
                           "The game was installed but no executable was chosen, so it was not "
                           "added to your game list.\n\n"
                           "You can add it later with the \"Add Game\" button.",
                           parent);
        g_install.reset();
        return;
    }

    std::string suggested = fs::path(*exe).stem().string();
    std::optional<std::string> name = dialogs::ask_string("Name this game",
                                                          "Name for the game in the list:",
                                                          suggested, parent);
    if (!name || name->empty()) {
        task->append_line("No name was given, so the game was not added.");
        task->mark_finished();
        set_installing(false);
        g_install.reset();
        return;
    }

    const bool added = add_installed_game(*name, fs::path(*exe), install.prefix);

    if (added && dialogs::ask_yes_no("Remove the installer files?",
                                     "\"" + install.game.title + "\" is installed.\n\n"
                                     "Delete the downloaded installer files?\n  " +
                                         install.download_dir.string(),
                                     parent)) {
        std::error_code remove_ec;
        fs::remove_all(install.download_dir, remove_ec);
        task->append_line("Removed the downloaded installer files.");
    }

    task->append_line("");
    task->append_line(added ? "Done - the game is in your game list." : "Finished.");
    task->mark_finished();
    set_installing(false);
    set_status(added ? "Installed " + *name + " from GOG." : "GOG install finished.");
    g_install.reset();
}

void on_install_clicked(GtkButton*, gpointer) {
    if (g_win == nullptr || g_install != nullptr) return;

    const gog::Game* game = selected_game();
    if (game == nullptr) {
        dialogs::show_info("Info", "Select a game from the list first.",
                           GTK_WINDOW(g_win->window));
        return;
    }
    const gog::Game chosen = *game;  // the list can change under us

    if (!g_win->gog.logged_in()) {
        dialogs::show_info("Sign in first",
                           "Sign in to GOG to install \"" + chosen.title + "\".",
                           GTK_WINDOW(g_win->window));
        return;
    }

    const std::optional<prefix::PrefixEntry> prefix = choose_prefix(GTK_WINDOW(g_win->window));
    if (!prefix) return;

    auto install = std::make_unique<Install>();
    install->game = chosen;
    install->prefix = *prefix;
    install->download_dir = cfg::gog_downloads_dir / sanitize(chosen.title);

    const std::string cancel_message =
        "Stop the download?\n\nPartly downloaded files are kept, so starting again resumes "
        "instead of fetching them again.";
    auto task = tasklog::open("Downloading " + chosen.title, GTK_WINDOW(g_win->window), true,
                              "Hides this window; the download keeps going.", cancel_message);
    install->task = task;
    g_install = std::move(install);
    set_installing(true);
    set_status("Downloading " + chosen.title + "...");

    Install* raw = g_install.get();
    std::thread([raw] { download_worker(raw); }).detach();
}

void on_selection_changed(GtkTreeSelection*, gpointer) { update_sign_in_ui(); }

void on_window_destroy(GtkWidget*, gpointer) { g_win = nullptr; }

}  // namespace

void open_window() {
    if (g_win != nullptr && GTK_IS_WIDGET(g_win->window)) {
        gtk_window_present(GTK_WINDOW(g_win->window));
        return;
    }

    auto* w = new Window();
    w->window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(w->window), "GOG Library");
    gtk_window_set_default_size(GTK_WINDOW(w->window), winmode::px(760), winmode::px(520));
    gtk_widget_set_size_request(GTK_WIDGET(w->window), winmode::px(520), winmode::px(340));
    gtk_window_set_transient_for(GTK_WINDOW(w->window), ui::window);
    gtk_window_set_position(GTK_WINDOW(w->window), GTK_WIN_POS_CENTER_ON_PARENT);
    g_signal_connect(w->window, "destroy", G_CALLBACK(on_window_destroy), nullptr);
    g_object_set_data_full(G_OBJECT(w->window), "gog-window", w,
                           [](gpointer data) { delete static_cast<Window*>(data); });

    GtkWidget* outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, winmode::px(8));
    gtk_container_set_border_width(GTK_CONTAINER(outer), winmode::px(15));
    gtk_box_set_spacing(GTK_BOX(outer), winmode::px(8));
    gtk_container_add(GTK_CONTAINER(w->window), outer);

    // ---- header ----
    GtkWidget* header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, winmode::px(10));
    gtk_box_pack_start(GTK_BOX(outer), header, FALSE, FALSE, 0);

    GtkWidget* titles = gtk_box_new(GTK_ORIENTATION_VERTICAL, winmode::px(2));
    gtk_box_pack_start(GTK_BOX(header), titles, TRUE, TRUE, 0);

    GtkWidget* heading = gtk_label_new("GOG Library");
    gtk_label_set_xalign(GTK_LABEL(heading), 0.0f);
    gtk_style_context_add_class(gtk_widget_get_style_context(heading), "heading");
    gtk_box_pack_start(GTK_BOX(titles), heading, FALSE, FALSE, 0);

    w->who_label = gtk_label_new("Not signed in to GOG");
    gtk_label_set_xalign(GTK_LABEL(w->who_label), 0.0f);
    gtk_box_pack_start(GTK_BOX(titles), w->who_label, FALSE, FALSE, 0);

    w->sign_button = gtk_button_new_with_label("Sign in to GOG");
    gtk_widget_set_valign(w->sign_button, GTK_ALIGN_CENTER);
    gtk_widget_set_size_request(w->sign_button, winmode::px(150), -1);
    g_signal_connect(w->sign_button, "clicked", G_CALLBACK(on_sign_clicked), nullptr);
    gtk_box_pack_start(GTK_BOX(header), w->sign_button, FALSE, FALSE, 0);

    // The embedded page is offered, but not as the default: it is the one part
    // of the launcher that costs a few hundred megabytes, and signing in
    // through the normal browser costs nothing at all.
    w->embedded_button = gtk_button_new_with_label("Embedded...");
    gtk_widget_set_valign(w->embedded_button, GTK_ALIGN_CENTER);
    gtk_widget_set_size_request(w->embedded_button, winmode::px(110), -1);
    gtk_widget_set_tooltip_text(
        w->embedded_button,
        "Sign in on a page inside this window instead of the normal browser.\n"
        "Saves a copy-and-paste, but uses a few hundred MB of memory while it is open.");
    g_signal_connect(w->embedded_button, "clicked", G_CALLBACK(on_embedded_sign_clicked), nullptr);
    gtk_box_pack_start(GTK_BOX(header), w->embedded_button, FALSE, FALSE, 0);

    // ---- the game list ----
    w->store = gtk_list_store_new(COL_COUNT, G_TYPE_STRING, G_TYPE_STRING);
    w->tree = gtk_tree_view_new_with_model(GTK_TREE_MODEL(w->store));
    g_object_unref(w->store);  // the view owns the model

    {
        GtkCellRenderer* renderer = gtk_cell_renderer_text_new();
        GtkTreeViewColumn* column =
            gtk_tree_view_column_new_with_attributes("Game", renderer, "text", COL_TITLE, NULL);
        gtk_tree_view_column_set_resizable(column, TRUE);
        gtk_tree_view_column_set_expand(column, TRUE);
        gtk_tree_view_append_column(GTK_TREE_VIEW(w->tree), column);
    }
    gtk_tree_selection_set_mode(gtk_tree_view_get_selection(GTK_TREE_VIEW(w->tree)),
                                GTK_SELECTION_SINGLE);
    g_signal_connect(gtk_tree_view_get_selection(GTK_TREE_VIEW(w->tree)), "changed",
                     G_CALLBACK(on_selection_changed), nullptr);

    GtkWidget* scroll = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_AUTOMATIC,
                                   GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(scroll), w->tree);
    gtk_box_pack_start(GTK_BOX(outer), scroll, TRUE, TRUE, 0);

    w->empty_label = gtk_label_new(
        "Your GOG library is empty here.\n\n"
        "Sign in to GOG and press Refresh to load the games in your account.");
    gtk_label_set_xalign(GTK_LABEL(w->empty_label), 0.0f);
    gtk_widget_set_visible(w->empty_label, FALSE);
    gtk_box_pack_start(GTK_BOX(outer), w->empty_label, FALSE, FALSE, 0);

    // ---- status + buttons ----
    w->status_label = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(w->status_label), 0.0f);
    gtk_label_set_ellipsize(GTK_LABEL(w->status_label), PANGO_ELLIPSIZE_END);
    gtk_box_pack_start(GTK_BOX(outer), w->status_label, FALSE, FALSE, 0);

    GtkWidget* row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, winmode::px(6));
    gtk_widget_set_halign(row, GTK_ALIGN_CENTER);
    gtk_widget_set_margin_top(row, winmode::px(6));
    gtk_box_pack_start(GTK_BOX(outer), row, FALSE, FALSE, 0);

    GtkWidget* refresh = gtk_button_new_with_label("Refresh");
    gtk_widget_set_size_request(refresh, winmode::px(110), -1);
    g_signal_connect(refresh, "clicked", G_CALLBACK(on_refresh_library), nullptr);
    gtk_box_pack_start(GTK_BOX(row), refresh, FALSE, FALSE, 0);

    w->install_button = gtk_button_new_with_label("Install Selected...");
    gtk_widget_set_size_request(w->install_button, winmode::px(170), -1);
    g_signal_connect(w->install_button, "clicked", G_CALLBACK(on_install_clicked), nullptr);
    gtk_box_pack_start(GTK_BOX(row), w->install_button, FALSE, FALSE, 0);

    GtkWidget* close = gtk_button_new_with_label("Close");
    gtk_widget_set_size_request(close, winmode::px(100), -1);
    g_signal_connect(close, "clicked", G_CALLBACK(actions::close_window_from_button), nullptr);
    gtk_box_pack_start(GTK_BOX(row), close, FALSE, FALSE, 0);

    g_win = w;
    gtk_widget_show_all(w->window);

    // Show the cached library right away, then refresh from GOG in the
    // background so the list is current without waiting on the network.
    update_sign_in_ui();
    if (w->gog.logged_in()) {
        const std::vector<gog::Game> cached = w->gog.cached_library();
        if (!cached.empty()) {
            show_library(cached);
            set_status(std::to_string(cached.size()) + " games (cached) - refreshing...");
        }
        refresh_library_async();
    } else {
        show_library({});
        set_status("Sign in to load your GOG library.");
    }
}

}  // namespace gogui
