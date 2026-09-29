#include "runner_dialog.h"

#include <gtk/gtk.h>

#include <algorithm>
#include <memory>
#include <vector>

#include "config.h"
#include "dialogs.h"
#include "proton.h"
#include "ui.h"
#include "window_mode.h"

namespace runner_dialog {

namespace {

namespace fs = std::filesystem;

struct DialogCtx;

// Controls the prefix location area for a single runner inside the dialog.
struct PrefixPanel {
    std::string runner_key;
    std::string runner_label;
    DialogCtx* ctx = nullptr;

    GtkWidget* panel = nullptr;
    GtkWidget* info_label = nullptr;
    GtkWidget* move_btn = nullptr;
    GtkWidget* browse_btn = nullptr;
    GtkWidget* default_btn = nullptr;

    std::optional<fs::path> existing_path;
    std::string existing_code;
    fs::path new_base_dir;
};

struct DialogCtx {
    std::string parent_script_name;
    GtkWidget* dialog = nullptr;

    GtkWidget* wine_radio = nullptr;
    GtkWidget* protonge_radio = nullptr;
    GtkWidget* protoncachyos_radio = nullptr;
    GtkWidget* steamproton_radio = nullptr;
    GtkWidget* dynamic_frame = nullptr;

    GtkWidget* wine_checkbox = nullptr;
    GtkWidget* protonge_version_label = nullptr;
    GtkWidget* protonge_version_combo = nullptr;
    GtkWidget* cachyos_version_label = nullptr;
    GtkWidget* cachyos_version_combo = nullptr;
    GtkWidget* steam_version_label = nullptr;
    GtkWidget* steam_version_combo = nullptr;

    std::unique_ptr<PrefixPanel> wine_panel;
    std::unique_ptr<PrefixPanel> protonge_panel;
    std::unique_ptr<PrefixPanel> protoncachyos_panel;
    std::unique_ptr<PrefixPanel> steamproton_panel;
};

std::string trim(const std::string& s) {
    size_t begin = s.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) return "";
    size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(begin, end - begin + 1);
}

void refresh_panel(PrefixPanel* panel) {
    if (panel->existing_path) {
        std::string text = "Prefix: " + panel->existing_code +
                           " (used previously, kept consistent)\nLocation: " +
                           panel->existing_path->string();
        gtk_label_set_text(GTK_LABEL(panel->info_label), text.c_str());
        gtk_widget_show(panel->move_btn);
        gtk_widget_hide(panel->browse_btn);
        gtk_widget_hide(panel->default_btn);
    } else {
        std::string text = "A new prefix will be created automatically at:\n" +
                           panel->new_base_dir.string() + " (e.g. GAMEXXX)";
        gtk_label_set_text(GTK_LABEL(panel->info_label), text.c_str());
        gtk_widget_hide(panel->move_btn);
        gtk_widget_show(panel->browse_btn);
        gtk_widget_show(panel->default_btn);
    }
}

void on_move_clicked(GtkButton*, gpointer data) {
    auto* panel = static_cast<PrefixPanel*>(data);
    if (!panel->existing_path) return;

    fs::path old_path = *panel->existing_path;
    proton::move_prefix_folder_dialog(
        old_path, panel->existing_code, panel->runner_key,
        [panel, old_path](const fs::path& new_path) {
            panel->existing_path = new_path;

            // The folder really moved on disk, so persist it even if the dialog
            // ends up being cancelled afterwards.
            if (!panel->ctx->parent_script_name.empty()) {
                json all = cfg::load_runner_config();
                if (all.contains(panel->ctx->parent_script_name)) {
                    json entry = all[panel->ctx->parent_script_name];
                    if (json_str(entry, "runner") == panel->runner_key) {
                        entry["prefix_path"] = new_path.string();
                        all[panel->ctx->parent_script_name] = entry;
                        cfg::save_runner_config(all);
                    }
                }
            }
            // Keep the global registry accurate for other games sharing this prefix.
            proton::update_registry_prefix_path(panel->runner_key, panel->existing_code, new_path);

            // Games installed inside the prefix moved with it: rewrite their
            // scripts (and every other script using this prefix) to point at
            // the new location instead of leaving them broken.
            std::vector<std::string> relinked = proton::relink_scripts_after_prefix_move(
                panel->runner_key, panel->existing_code, old_path, new_path);

            refresh_panel(panel);
            if (!relinked.empty()) {
                ui::set_status("Relinked " + std::to_string(relinked.size()) +
                                   " script(s) after prefix move",
                               "success");
            }
        },
        GTK_WINDOW(panel->ctx->dialog));
}

void on_browse_clicked(GtkButton*, gpointer data) {
    auto* panel = static_cast<PrefixPanel*>(data);
    fs::path initial = panel->new_base_dir;
    std::error_code ec;
    if (!fs::is_directory(initial, ec)) initial = cfg::home_dir();

    std::optional<fs::path> chosen = proton::browse_folder_with_create_option(
        "Choose Default Location for New " + panel->runner_label + " Prefixes", initial,
        GTK_WINDOW(panel->ctx->dialog));
    if (chosen) {
        panel->new_base_dir = *chosen;
        proton::set_prefix_base_dir(panel->runner_key, *chosen);
        refresh_panel(panel);
    }
}

void on_default_clicked(GtkButton*, gpointer data) {
    auto* panel = static_cast<PrefixPanel*>(data);
    panel->new_base_dir = cfg::default_prefix_roots().at(panel->runner_key);
    proton::reset_prefix_base_dir(panel->runner_key);
    refresh_panel(panel);
}

std::unique_ptr<PrefixPanel> build_prefix_panel(const std::string& runner_key,
                                                const std::string& runner_label,
                                                const json& existing_cfg, DialogCtx* ctx) {
    auto panel = std::make_unique<PrefixPanel>();
    panel->runner_key = runner_key;
    panel->runner_label = runner_label;
    panel->ctx = ctx;
    panel->new_base_dir = proton::get_prefix_base_dir(runner_key);

    if (existing_cfg.is_object() && json_str(existing_cfg, "runner") == runner_key &&
        !json_str(existing_cfg, "prefix_code").empty() &&
        !json_str(existing_cfg, "prefix_path").empty()) {
        panel->existing_path = fs::path(json_str(existing_cfg, "prefix_path"));
        panel->existing_code = json_str(existing_cfg, "prefix_code");
    }

    panel->panel = gtk_box_new(GTK_ORIENTATION_VERTICAL, winmode::px(4));

    panel->info_label = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(panel->info_label), 0.0f);
    gtk_label_set_line_wrap(GTK_LABEL(panel->info_label), TRUE);
    gtk_box_pack_start(GTK_BOX(panel->panel), panel->info_label, FALSE, FALSE, 2);

    GtkWidget* btn_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, winmode::px(5));
    gtk_box_pack_start(GTK_BOX(panel->panel), btn_row, FALSE, FALSE, 4);

    panel->move_btn = gtk_button_new_with_label("Move Prefix...");
    panel->browse_btn = gtk_button_new_with_label("Browse Other Folder/Disk...");
    panel->default_btn = gtk_button_new_with_label("Use Default (WLM Folder)");

    gtk_box_pack_start(GTK_BOX(btn_row), panel->move_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(btn_row), panel->browse_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(btn_row), panel->default_btn, FALSE, FALSE, 0);

    g_signal_connect(panel->move_btn, "clicked", G_CALLBACK(on_move_clicked), panel.get());
    g_signal_connect(panel->browse_btn, "clicked", G_CALLBACK(on_browse_clicked), panel.get());
    g_signal_connect(panel->default_btn, "clicked", G_CALLBACK(on_default_clicked), panel.get());

    refresh_panel(panel.get());
    return panel;
}

void update_runner_widgets(gpointer data) {
    auto* ctx = static_cast<DialogCtx*>(data);

    std::string chosen = "wine";
    if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(ctx->protonge_radio))) {
        chosen = "protonge";
    } else if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(ctx->protoncachyos_radio))) {
        chosen = "protoncachyos";
    } else if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(ctx->steamproton_radio))) {
        chosen = "steamproton";
    }

    bool wine_active = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(ctx->wine_checkbox));

    gtk_widget_set_visible(ctx->wine_checkbox, chosen == "wine");
    gtk_widget_set_visible(ctx->wine_panel->panel, chosen == "wine" && wine_active);
    gtk_widget_set_visible(ctx->protonge_version_label, chosen == "protonge");
    gtk_widget_set_visible(ctx->protonge_version_combo, chosen == "protonge");
    gtk_widget_set_visible(ctx->protonge_panel->panel, chosen == "protonge");
    gtk_widget_set_visible(ctx->cachyos_version_label, chosen == "protoncachyos");
    gtk_widget_set_visible(ctx->cachyos_version_combo, chosen == "protoncachyos");
    gtk_widget_set_visible(ctx->protoncachyos_panel->panel, chosen == "protoncachyos");
    gtk_widget_set_visible(ctx->steam_version_label, chosen == "steamproton");
    gtk_widget_set_visible(ctx->steam_version_combo, chosen == "steamproton");
    gtk_widget_set_visible(ctx->steamproton_panel->panel, chosen == "steamproton");
}

extern "C" void on_runner_toggled(GtkToggleButton*, gpointer data) {
    update_runner_widgets(data);
}

extern "C" void on_wine_checkbox_toggled(GtkToggleButton*, gpointer data) {
    update_runner_widgets(data);
}

}  // namespace

std::optional<scripts::RunnerChoice> ask_runner_choice(const std::string& parent_script_name,
                                                       const std::string& purpose) {
    proton::BuildList protonge_list = proton::find_protonge_installations();
    proton::BuildList protoncachyos_list = proton::find_protoncachyos_installations();
    proton::BuildList steamproton_list = proton::find_steam_proton_installations();

    json existing_cfg = json::object();
    if (!parent_script_name.empty()) {
        existing_cfg = json_obj(cfg::load_runner_config(), parent_script_name);
    }

    std::unique_ptr<DialogCtx> ctx = std::make_unique<DialogCtx>();
    ctx->parent_script_name = parent_script_name;

    ctx->dialog = gtk_dialog_new_with_buttons(
        purpose == "setup" ? "Select Runner - Setup" : "Select Runner - Play", ui::window,
        GTK_DIALOG_MODAL, "_Cancel", GTK_RESPONSE_CANCEL, "_OK", GTK_RESPONSE_OK, NULL);
    gtk_window_set_resizable(GTK_WINDOW(ctx->dialog), FALSE);
    gtk_dialog_set_default_response(GTK_DIALOG(ctx->dialog), GTK_RESPONSE_OK);

    GtkWidget* content = gtk_dialog_get_content_area(GTK_DIALOG(ctx->dialog));
    gtk_container_set_border_width(GTK_CONTAINER(content), winmode::px(15));
    gtk_box_set_spacing(GTK_BOX(content), winmode::px(4));

    auto add_content = [content](GtkWidget* child) {
        gtk_box_pack_start(GTK_BOX(content), child, FALSE, FALSE, 0);
    };

    GtkWidget* game_label = gtk_label_new("Select Runner - Game:");
    gtk_label_set_xalign(GTK_LABEL(game_label), 0.0f);
    add_content(game_label);

    std::string existing_runner = json_str(existing_cfg, "runner");
    std::string default_runner = "wine";
    if (existing_runner == "protonge" || existing_runner == "protoncachyos" ||
        existing_runner == "steamproton") {
        default_runner = existing_runner;
    }

    // Radio buttons with their indicator turned off, grouped visually with the
    // GTK "linked" style so they look like a row of buttons.
    GtkWidget* runner_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_style_context_add_class(gtk_widget_get_style_context(runner_row), "linked");
    gtk_widget_set_margin_top(runner_row, winmode::px(6));
    gtk_widget_set_margin_bottom(runner_row, winmode::px(6));
    add_content(runner_row);

    ctx->wine_radio = gtk_radio_button_new_with_label(nullptr, "Wine (Vanilla)");
    gtk_toggle_button_set_mode(GTK_TOGGLE_BUTTON(ctx->wine_radio), FALSE);
    ctx->protonge_radio = gtk_radio_button_new_with_label_from_widget(
        GTK_RADIO_BUTTON(ctx->wine_radio), "Proton GE");
    gtk_toggle_button_set_mode(GTK_TOGGLE_BUTTON(ctx->protonge_radio), FALSE);
    ctx->protoncachyos_radio = gtk_radio_button_new_with_label_from_widget(
        GTK_RADIO_BUTTON(ctx->wine_radio), "Proton-CachyOS");
    gtk_toggle_button_set_mode(GTK_TOGGLE_BUTTON(ctx->protoncachyos_radio), FALSE);
    ctx->steamproton_radio = gtk_radio_button_new_with_label_from_widget(
        GTK_RADIO_BUTTON(ctx->wine_radio), "Proton (Steam)");
    gtk_toggle_button_set_mode(GTK_TOGGLE_BUTTON(ctx->steamproton_radio), FALSE);

    for (GtkWidget* radio : {ctx->wine_radio, ctx->protonge_radio, ctx->protoncachyos_radio,
                             ctx->steamproton_radio}) {
        gtk_widget_set_hexpand(radio, TRUE);
        gtk_box_pack_start(GTK_BOX(runner_row), radio, TRUE, TRUE, 0);
    }

    GtkWidget* default_radio = ctx->wine_radio;
    if (default_runner == "protonge") default_radio = ctx->protonge_radio;
    if (default_runner == "protoncachyos") default_radio = ctx->protoncachyos_radio;
    if (default_runner == "steamproton") default_radio = ctx->steamproton_radio;
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(default_radio), TRUE);

    // Container that stays put for every runner-dependent control: the widgets
    // inside are only hidden/shown, so the dialog layout never jumps around.
    ctx->dynamic_frame = gtk_box_new(GTK_ORIENTATION_VERTICAL, winmode::px(4));
    add_content(ctx->dynamic_frame);

    // ---- Wine group ----
    bool existing_wine_prefix = json_str(existing_cfg, "runner") == "wine" &&
                                !json_str(existing_cfg, "prefix_path").empty();
    ctx->wine_checkbox = gtk_check_button_new_with_label(
        "Use an isolated prefix for this game (avoids buildup in home folder)");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(ctx->wine_checkbox), existing_wine_prefix);
    ctx->wine_panel = build_prefix_panel("wine", "Wine", existing_cfg, ctx.get());

    // ---- Proton GE group ----
    ctx->protonge_version_label = gtk_label_new("Proton GE Version:");
    gtk_label_set_xalign(GTK_LABEL(ctx->protonge_version_label), 0.0f);
    ctx->protonge_version_combo = gtk_combo_box_text_new();
    if (!protonge_list.empty()) {
        std::vector<std::string> names;
        for (const auto& build : protonge_list) {
            names.push_back(build.first);
            gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(ctx->protonge_version_combo),
                                           build.first.c_str());
        }
        int default_idx = 0;
        if (json_str(existing_cfg, "runner") == "protonge") {
            auto it = std::find(names.begin(), names.end(),
                                json_str(existing_cfg, "proton_name"));
            if (it != names.end()) default_idx = static_cast<int>(it - names.begin());
        }
        gtk_combo_box_set_active(GTK_COMBO_BOX(ctx->protonge_version_combo), default_idx);
    } else {
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(ctx->protonge_version_combo),
                                       "(None yet - Extract via Settings menu)");
        gtk_combo_box_set_active(GTK_COMBO_BOX(ctx->protonge_version_combo), 0);
        gtk_widget_set_sensitive(ctx->protonge_radio, FALSE);
    }
    ctx->protonge_panel = build_prefix_panel("protonge", "Proton GE", existing_cfg, ctx.get());

    // ---- Proton-CachyOS group ----
    ctx->cachyos_version_label = gtk_label_new("Proton-CachyOS Version:");
    gtk_label_set_xalign(GTK_LABEL(ctx->cachyos_version_label), 0.0f);
    ctx->cachyos_version_combo = gtk_combo_box_text_new();
    if (!protoncachyos_list.empty()) {
        std::vector<std::string> names;
        for (const auto& build : protoncachyos_list) {
            names.push_back(build.first);
            gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(ctx->cachyos_version_combo),
                                           build.first.c_str());
        }
        int default_idx = 0;
        if (json_str(existing_cfg, "runner") == "protoncachyos") {
            auto it = std::find(names.begin(), names.end(),
                                json_str(existing_cfg, "proton_name"));
            if (it != names.end()) default_idx = static_cast<int>(it - names.begin());
        }
        gtk_combo_box_set_active(GTK_COMBO_BOX(ctx->cachyos_version_combo), default_idx);
    } else {
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(ctx->cachyos_version_combo),
                                       "(None yet - Extract via Settings menu)");
        gtk_combo_box_set_active(GTK_COMBO_BOX(ctx->cachyos_version_combo), 0);
        gtk_widget_set_sensitive(ctx->protoncachyos_radio, FALSE);
    }
    ctx->protoncachyos_panel =
        build_prefix_panel("protoncachyos", "Proton-CachyOS", existing_cfg, ctx.get());

    // ---- Proton builds that Steam installed ----
    ctx->steam_version_label = gtk_label_new("Proton build (installed by Steam):");
    gtk_label_set_xalign(GTK_LABEL(ctx->steam_version_label), 0.0f);
    ctx->steam_version_combo = gtk_combo_box_text_new();
    if (!steamproton_list.empty()) {
        std::vector<std::string> steam_names;
        for (const auto& build : steamproton_list) {
            steam_names.push_back(build.first);
            gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(ctx->steam_version_combo),
                                           build.first.c_str());
        }
        int default_idx = 0;
        if (json_str(existing_cfg, "runner") == "steamproton") {
            auto it = std::find(steam_names.begin(), steam_names.end(),
                                json_str(existing_cfg, "proton_name"));
            if (it != steam_names.end()) default_idx = static_cast<int>(it - steam_names.begin());
        }
        gtk_combo_box_set_active(GTK_COMBO_BOX(ctx->steam_version_combo), default_idx);
    } else {
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(ctx->steam_version_combo),
                                       "(None found in Steam's compatibilitytools.d)");
        gtk_combo_box_set_active(GTK_COMBO_BOX(ctx->steam_version_combo), 0);
        gtk_widget_set_sensitive(ctx->steamproton_radio, FALSE);
    }
    ctx->steamproton_panel =
        build_prefix_panel("steamproton", "Proton (Steam)", existing_cfg, ctx.get());

    for (GtkWidget* widget : {ctx->wine_checkbox, ctx->wine_panel->panel,
                              ctx->protonge_version_label, ctx->protonge_version_combo,
                              ctx->protonge_panel->panel, ctx->cachyos_version_label,
                              ctx->cachyos_version_combo, ctx->protoncachyos_panel->panel,
                              ctx->steam_version_label, ctx->steam_version_combo,
                              ctx->steamproton_panel->panel}) {
        gtk_box_pack_start(GTK_BOX(ctx->dynamic_frame), widget, FALSE, FALSE, 0);
    }

    g_signal_connect(ctx->wine_radio, "toggled", G_CALLBACK(on_runner_toggled), ctx.get());
    g_signal_connect(ctx->protonge_radio, "toggled", G_CALLBACK(on_runner_toggled), ctx.get());
    g_signal_connect(ctx->protoncachyos_radio, "toggled", G_CALLBACK(on_runner_toggled), ctx.get());
    g_signal_connect(ctx->wine_checkbox, "toggled", G_CALLBACK(on_wine_checkbox_toggled), ctx.get());

    add_content(gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));

    GtkWidget* launch_label = gtk_label_new("Launch Options / Environment Variable (optional):");
    gtk_label_set_xalign(GTK_LABEL(launch_label), 0.0f);
    add_content(launch_label);

    GtkWidget* launch_options_entry = gtk_entry_new();
    std::string stored_launch_options = json_str(existing_cfg, "launch_options");
    if (!stored_launch_options.empty()) {
        gtk_entry_set_text(GTK_ENTRY(launch_options_entry), stored_launch_options.c_str());
    }
    add_content(launch_options_entry);

    GtkWidget* hint_label = gtk_label_new("Example: PROTON_USE_WINED3D=1 MANGOHUD=1");
    gtk_label_set_xalign(GTK_LABEL(hint_label), 0.0f);
    gtk_style_context_add_class(gtk_widget_get_style_context(hint_label), "dim-label");
    add_content(hint_label);

    GtkWidget* comment_label = gtk_label_new("Comment (optional):");
    gtk_label_set_xalign(GTK_LABEL(comment_label), 0.0f);
    add_content(comment_label);

    GtkWidget* comment_entry = gtk_entry_new();
    std::string stored_comment = json_str(existing_cfg, "comment");
    if (!stored_comment.empty()) {
        gtk_entry_set_text(GTK_ENTRY(comment_entry), stored_comment.c_str());
    }
    add_content(comment_entry);

    gtk_widget_show_all(ctx->dialog);
    update_runner_widgets(ctx.get());

    std::optional<scripts::RunnerChoice> result;
    while (true) {
        gint response = gtk_dialog_run(GTK_DIALOG(ctx->dialog));
        if (response != GTK_RESPONSE_OK) {
            result.reset();
            break;
        }

        std::string chosen = "wine";
        if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(ctx->protonge_radio))) {
            chosen = "protonge";
        } else if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(ctx->protoncachyos_radio))) {
            chosen = "protoncachyos";
        } else if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(ctx->steamproton_radio))) {
            chosen = "steamproton";
        }

        std::string launch_options = trim(gtk_entry_get_text(GTK_ENTRY(launch_options_entry)));
        std::string comment = trim(gtk_entry_get_text(GTK_ENTRY(comment_entry)));

        if (chosen == "wine") {
            scripts::RunnerChoice choice;
            choice.runner = "wine";
            choice.launch_options = launch_options;
            choice.comment = comment;

            if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(ctx->wine_checkbox))) {
                std::string prefix_code;
                fs::path prefix_path;
                if (ctx->wine_panel->existing_path) {
                    prefix_code = ctx->wine_panel->existing_code;
                    prefix_path = *ctx->wine_panel->existing_path;
                } else {
                    prefix_code = proton::generate_next_prefix_code("wine");
                    prefix_path = ctx->wine_panel->new_base_dir / prefix_code;
                }
                std::error_code ec;
                fs::create_directories(prefix_path, ec);
                proton::record_prefix_usage("wine", prefix_code, prefix_path);
                choice.prefix_code = prefix_code;
                choice.prefix_path = prefix_path.string();
                choice.use_prefix = true;
            }
            result = choice;
            break;
        }

        proton::BuildList* build_list = nullptr;
        GtkWidget* version_combo = nullptr;
        std::string build_label;
        PrefixPanel* prefix_state = nullptr;

        if (chosen == "protonge") {
            build_list = &protonge_list;
            version_combo = ctx->protonge_version_combo;
            build_label = "Proton GE";
            prefix_state = ctx->protonge_panel.get();
        } else if (chosen == "steamproton") {
            build_list = &steamproton_list;
            version_combo = ctx->steam_version_combo;
            build_label = "Proton (Steam)";
            prefix_state = ctx->steamproton_panel.get();
        } else {
            build_list = &protoncachyos_list;
            version_combo = ctx->cachyos_version_combo;
            build_label = "Proton-CachyOS";
            prefix_state = ctx->protoncachyos_panel.get();
        }

        if (build_list->empty()) {
            dialogs::show_error("Error",
                                build_label +
                                    " not found. Extract it first via the Settings menu.",
                                GTK_WINDOW(ctx->dialog));
            continue;  // let the user pick something else without closing the dialog
        }

        int idx = gtk_combo_box_get_active(GTK_COMBO_BOX(version_combo));
        if (idx < 0 || idx >= static_cast<int>(build_list->size())) continue;

        const std::string& proton_name = (*build_list)[idx].first;
        const fs::path& proton_path = (*build_list)[idx].second;

        std::string prefix_code;
        fs::path prefix_path;
        if (prefix_state->existing_path) {
            prefix_code = prefix_state->existing_code;
            prefix_path = *prefix_state->existing_path;
        } else {
            prefix_code = proton::generate_next_prefix_code(chosen);
            prefix_path = prefix_state->new_base_dir / prefix_code;
        }
        std::error_code ec;
        fs::create_directories(prefix_path, ec);
        proton::record_prefix_usage(chosen, prefix_code, prefix_path, proton_name,
                                    proton_path.string());

        scripts::RunnerChoice choice;
        choice.runner = chosen;
        choice.proton_name = proton_name;
        choice.proton_path = proton_path.string();
        choice.prefix_code = prefix_code;
        choice.prefix_path = prefix_path.string();
        choice.launch_options = launch_options;
        choice.comment = comment;
        choice.use_prefix = true;
        result = choice;
        break;
    }

    GtkWidget* dialog = ctx->dialog;
    gtk_widget_destroy(dialog);
    ctx.reset();
    return result;
}

}  // namespace runner_dialog
