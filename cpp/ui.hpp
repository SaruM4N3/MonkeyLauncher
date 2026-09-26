#pragma once
#include <gtkmm.h>

#include <functional>
#include <optional>
#include <string>

#include "common.hpp"

namespace ml {

// GLib.idle_add from a worker thread: runs `fn` once on the GTK main loop.
void run_on_main(std::function<void()> fn);

// pack_start shorthand mirroring PyGObject's pack_start(child, expand, fill, padding).
inline void pack(Gtk::Box& box, Gtk::Widget& w, bool expand = false, bool fill = false,
                 int padding = 0) {
    box.pack_start(w, expand, fill, static_cast<guint>(padding));
}

// PyGObject's margin=N: all four margins at once.
inline void set_margin_all(Gtk::Widget& w, int m) {
    w.set_margin_top(m);
    w.set_margin_bottom(m);
    w.set_margin_start(m);
    w.set_margin_end(m);
}

Gtk::Label* make_label(const Glib::ustring& text, float xalign = 0.5f);

// Gtk.MessageDialog(...): error box with an OK button (parent may be null).
void show_error(Gtk::Window* parent, const std::string& msg);

// Dialog with a bold-less primary text and a secondary line; runs it and
// returns the response id.
int run_message(Gtk::Window* parent, Gtk::MessageType type, Gtk::ButtonsType buttons,
                const Glib::ustring& text, const Glib::ustring& secondary = "");

// Select-folder dialog starting at `start`; nullopt if cancelled.
std::optional<fs::path> choose_folder(Gtk::Window* parent, const Glib::ustring& title,
                                      const fs::path& start);

}  // namespace ml
