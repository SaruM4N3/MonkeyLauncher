#include <memory>

#include "ui.hpp"

namespace ml {

void run_on_main(std::function<void()> fn) {
    auto* heap = new std::function<void()>(std::move(fn));
    g_idle_add_full(G_PRIORITY_DEFAULT_IDLE,
                    [](gpointer data) -> gboolean {
                        auto* f = static_cast<std::function<void()>*>(data);
                        (*f)();
                        return G_SOURCE_REMOVE;
                    },
                    heap,
                    [](gpointer data) { delete static_cast<std::function<void()>*>(data); });
}

Gtk::Label* make_label(const Glib::ustring& text, float xalign) {
    auto* l = Gtk::manage(new Gtk::Label(text));
    l->set_xalign(xalign);
    return l;
}

int run_message(Gtk::Window* parent, Gtk::MessageType type, Gtk::ButtonsType buttons,
                const Glib::ustring& text, const Glib::ustring& secondary) {
    std::unique_ptr<Gtk::MessageDialog> d;
    if (parent) d = std::make_unique<Gtk::MessageDialog>(*parent, text, false, type, buttons);
    else        d = std::make_unique<Gtk::MessageDialog>(text, false, type, buttons);
    if (!secondary.empty()) d->set_secondary_text(secondary);
    return d->run();
}

void show_error(Gtk::Window* parent, const std::string& msg) {
    run_message(parent, Gtk::MESSAGE_ERROR, Gtk::BUTTONS_OK, msg);
}

std::optional<fs::path> choose_folder(Gtk::Window* parent, const Glib::ustring& title,
                                      const fs::path& start) {
    std::unique_ptr<Gtk::FileChooserDialog> d;
    if (parent) d = std::make_unique<Gtk::FileChooserDialog>(*parent, title, Gtk::FILE_CHOOSER_ACTION_SELECT_FOLDER);
    else        d = std::make_unique<Gtk::FileChooserDialog>(title, Gtk::FILE_CHOOSER_ACTION_SELECT_FOLDER);
    d->add_button("_Cancel", Gtk::RESPONSE_CANCEL);
    d->add_button("Select", Gtk::RESPONSE_OK);
    d->set_current_folder(start.string());
    if (d->run() != Gtk::RESPONSE_OK) return std::nullopt;
    return fs::path(d->get_filename());
}

}  // namespace ml
