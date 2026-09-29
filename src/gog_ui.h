#pragma once

// The "GOG Library" window (SETTINGS -> "GOG Library...").
//
// Shows the games in the signed-in GOG account and installs one of them into a
// prefix of this launcher: the Windows offline installer is downloaded, then
// run as an ordinary installer inside the chosen prefix, and once it finishes
// the user points at the game's .exe so it joins the normal game list - from
// then on it is an ordinary game (backup, prefix manager, runner choice, all of
// it works exactly like the others).

namespace gogui {

// Opens (or raises) the window.
void open_window();

}  // namespace gogui
