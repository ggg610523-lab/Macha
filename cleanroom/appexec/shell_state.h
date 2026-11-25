// State behind the Mayonnaise shell.
//
// Everything the shell shows is derived from the bundle the loader just ran:
// names and paths come from the loader, file contents are read from inside the
// bundle.  Keeping the derivation here (rather than in mayo_ui.cpp) lets the
// unit test check it without a compositor or the drawing backend.
#pragma once

#include <functional>
#include <string>
#include <vector>

struct ShellEntry {
    std::string id;     // sidebar id: "app", "exe", "plist", "contents", ...
    std::string title;  // text drawn in the sidebar
    bool header = false;
    bool folder = false;
};

struct ShellState {
    std::string appName;
    std::string exePath;    // absolute path of the app's executable
    std::string exeName;    // last path component
    std::string bundlePath; // .../CotEditor.app
    std::string summary;    // "84 images, 45548 symbols, 0 unresolved"
    std::string document;   // editor content selected at startup
    std::vector<ShellEntry> sidebar;

    const ShellEntry *entry(const std::string &id) const;
};

// Pure derivation from the loader's recovered state (no I/O).
ShellState shell_state_compose(const std::string &appName,
                               const std::string &exePath,
                               const std::string &bundlePath,
                               const std::string &summary);

// Reads the loader's PLSR_APP_* environment and the initial document.
ShellState shell_state_load();

// Editor content for a sidebar entry, read from the loaded bundle.  Returns
// empty for headers and unknown ids.
std::string shell_entry_content(const ShellState &state, const std::string &id);

// The sidebar's selection handler.  The shell wires it to the editor exactly
// once; the test drives this same path instead of re-implementing it.
struct ShellSelection {
    ShellState state;
    std::function<void(const std::string &)> onDocument;

    void select(const std::string &id);
};
