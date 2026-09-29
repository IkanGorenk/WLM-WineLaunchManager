#pragma once

// Backup / restore of a prefix together with the games that use it
// (~/wlm backups as .tar.gz with a manifest.json inside).
// C++ port of open_backup_prefix_dialog()/run_backup_worker()/
// open_restore_backup_dialog()/run_restore_worker() in launcher.py.

#include <gtk/gtk.h>

#include "prefix_manager.h"

namespace backup {

// Asks for confirmation + a destination file, then archives the prefix folder,
// the game folders (that live outside the prefix) and their icons.
void open_backup_prefix_dialog(const prefix::PrefixEntry& entry, GtkWindow* parent = nullptr);

// Picks a .tar.gz backup, reads its manifest, asks where to restore it and
// then extracts + registers everything as a new prefix / new games.
void open_restore_backup_dialog(GtkWindow* parent = nullptr);

}  // namespace backup
