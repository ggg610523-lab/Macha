// Checks that the Mayonnaise shell's sidebar, header and editor content come
// from the loaded bundle's state rather than from fixed sample text.
#include "shell_state.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

static int failures = 0;

static void check(bool ok, const std::string &what) {
    if (!ok) {
        std::fprintf(stderr, "FAIL  %s\n", what.c_str());
        ++failures;
    }
}

static void checkContains(const std::string &haystack, const std::string &needle,
                          const std::string &what) {
    check(haystack.find(needle) != std::string::npos, what + " (missing '" + needle + "')");
}

static void writeFile(const fs::path &path, const std::string &data) {
    std::ofstream out(path, std::ios::binary);
    out.write(data.data(), static_cast<std::streamsize>(data.size()));
}

int main() {
    // ---- pure composition: the sidebar and header follow the given state ----
    const ShellState s = shell_state_compose("Contoso", "/apps/Contoso.app/Contents/MacOS/contoso",
                                             "/apps/Contoso.app",
                                             "7 images, 120 symbols, 0 unresolved");
    check(s.appName == "Contoso", "app name comes from the loader");
    check(s.exeName == "contoso", "executable name is the basename");
    check(s.summary == "7 images, 120 symbols, 0 unresolved", "header summary is the load result");

    const std::vector<std::string> ids = {"app", "exe", "plist", "contents", "macos", "resources"};
    size_t selectable = 0;
    for (const ShellEntry &e : s.sidebar) {
        if (!e.header) ++selectable;
        if (e.header) {
            check(e.id.empty(), "headers have no sidebar id");
        }
    }
    check(selectable == ids.size(), "sidebar has six selectable entries");
    for (const std::string &id : ids) check(s.entry(id) != nullptr, "sidebar has entry " + id);
    check(s.entry("app")->title == "Contoso", "app entry is titled with the app name");
    check(s.entry("exe")->title == "contoso", "exe entry is titled with the executable");
    check(s.entry("plist")->title == "Info.plist", "plist entry is labelled Info.plist");
    check(s.entry("resources")->folder, "bundle entries are folders");
    check(s.entry("nope") == nullptr, "unknown ids have no entry");
    check(shell_state_compose("", "", "", "").summary == "not loaded",
          "empty summary reports not loaded");

    // ---- content: editor text is read from the bundle, per sidebar entry ----
    fs::path root = fs::temp_directory_path() / "plsr-shell-state-test";
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root / "Contoso.app/Contents/MacOS");
    fs::create_directories(root / "Contoso.app/Contents/Resources/Base.lproj/Main.storyboardc");
    writeFile(root / "Contoso.app/Contents/Info.plist",
              "<?xml version=\"1.0\"?>\n<key>CFBundleName</key>\n<string>Contoso</string>\n");
    writeFile(root / "Contoso.app/Contents/MacOS/contoso",
              std::string("MACHO\0\1\2\3binary payload\0", 25));

    const ShellState loaded =
        shell_state_compose("Contoso", (root / "Contoso.app/Contents/MacOS/contoso").string(),
                            (root / "Contoso.app").string(), "7 images, 120 symbols, 0 unresolved");

    const std::string plist = shell_entry_content(loaded, "app");
    checkContains(plist, "CFBundleName", "editor shows the bundle's real Info.plist");
    check(plist == shell_entry_content(loaded, "plist"), "app and Info.plist open the same file");

    const std::string exe = shell_entry_content(loaded, "exe");
    checkContains(exe, "contoso", "executable view names the executable");
    checkContains(exe, "binary; hex dump", "executable view dumps binary contents");
    check(exe.find('\0') == std::string::npos, "hex dump contains no NUL bytes");

    const std::string resources = shell_entry_content(loaded, "resources");
    checkContains(resources, "Base.lproj/", "Resources view lists the bundle's folders");
    checkContains(shell_entry_content(loaded, "contents"), "Info.plist",
                  "Contents view lists Info.plist");
    checkContains(shell_entry_content(loaded, "macos"), "contoso", "MacOS view lists the exe");
    check(shell_entry_content(loaded, "nope").empty(), "unknown ids have no document");

    // ---- load(): reads the loader's environment and opens Info.plist ----
    setenv("PLSR_APP_NAME", "Contoso", 1);
    setenv("PLSR_APP_EXE", (root / "Contoso.app/Contents/MacOS/contoso").c_str(), 1);
    setenv("PLSR_APP_BUNDLE", (root / "Contoso.app").c_str(), 1);
    setenv("PLSR_APP_SUMMARY", "7 images, 120 symbols, 0 unresolved", 1);
    const ShellState fromEnv = shell_state_load();
    check(fromEnv.appName == "Contoso", "load() takes the app name from the environment");
    check(fromEnv.bundlePath == (root / "Contoso.app").string(),
          "load() takes the bundle path from the environment");
    checkContains(fromEnv.document, "CFBundleName", "load() opens the bundle Info.plist");
    check(fromEnv.document == plist, "load() document matches the Info.plist content");

    // ---- selection wiring: the handler the sidebar calls, not just content ----
    ShellSelection selection;
    selection.state = loaded;
    std::vector<std::string> delivered;
    selection.onDocument = [&](const std::string &text) { delivered.push_back(text); };

    selection.select("resources");
    selection.select("exe");
    selection.select("contents");
    check(delivered.size() == 3, "each selection delivers one document");
    checkContains(delivered[0], "Base.lproj/", "resources selection delivers the listing");
    checkContains(delivered[1], "binary; hex dump", "exe selection delivers the hex dump");
    checkContains(delivered[2], "Info.plist", "contents selection delivers its listing");

    selection.select("nope");
    check(delivered.size() == 4, "unknown selections still notify");
    checkContains(delivered[3], "no content", "unknown selections deliver the fallback");

    selection.onDocument = nullptr;
    selection.select("app");   /* no callback installed: must not crash */
    check(true, "selection without a callback is safe");

    fs::remove_all(root, ec);

    if (failures == 0) {
        std::printf("PASS  shell sidebar/header/editor derive from bundle state\n");
        return 0;
    }
    std::printf("shell_state_test: %d failure(s)\n", failures);
    return 1;
}
