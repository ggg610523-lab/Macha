#include "shell_state.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <sys/stat.h>

namespace {

constexpr size_t kTextLimit = 24 * 1024;
constexpr size_t kBinaryLimit = 8 * 1024;

std::string baseName(const std::string &path) {
    const auto slash = path.find_last_of('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

std::string readFile(const std::string &path, size_t limit) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    std::string s(limit, '\0');
    in.read(s.data(), static_cast<std::streamsize>(limit));
    s.resize(static_cast<size_t>(in.gcount()));
    return s;
}

bool looksBinary(const std::string &data) {
    const size_t n = std::min<size_t>(data.size(), 512);
    if (n == 0) return false;
    size_t suspicious = 0;
    for (size_t i = 0; i < n; ++i) {
        const unsigned char c = static_cast<unsigned char>(data[i]);
        if (c == 0) return true;
        if (c < 0x09 || (c > 0x0d && c < 0x20)) ++suspicious;
    }
    return suspicious * 10 > n * 3;
}

std::string hexDump(const std::string &data, size_t limit) {
    std::string out;
    const size_t n = std::min(data.size(), limit);
    char buf[16];
    for (size_t i = 0; i < n; i += 16) {
        std::snprintf(buf, sizeof buf, "%08zx  ", i);
        out += buf;
        std::string ascii;
        for (size_t j = 0; j < 16; ++j) {
            if (i + j < n) {
                const unsigned char c = static_cast<unsigned char>(data[i + j]);
                std::snprintf(buf, sizeof buf, "%02x ", c);
                out += buf;
                ascii.push_back(c >= 0x20 && c < 0x7f ? static_cast<char>(c) : '.');
            } else {
                out += "   ";
            }
            if (j == 7) out += ' ';
        }
        out += " |";
        out += ascii;
        out += "|\n";
    }
    return out;
}

std::string listDirectory(const std::string &path) {
    DIR *d = opendir(path.c_str());
    if (!d) return {};
    std::vector<std::string> names;
    while (struct dirent *e = readdir(d)) {
        const char *name = e->d_name;
        if (!std::strcmp(name, ".") || !std::strcmp(name, "..")) continue;
        std::string entry = name;
        if (e->d_type == DT_DIR) entry += "/";
        names.push_back(std::move(entry));
    }
    closedir(d);
    std::sort(names.begin(), names.end());
    std::string out = "# " + path + "\n";
    for (const std::string &name : names) out += name + "\n";
    return out;
}

bool isDirectory(const std::string &path) {
    struct stat st{};
    return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

std::string join(const std::string &base, const std::string &rel) {
    if (base.empty()) return {};
    return base + "/" + rel;
}

}  // namespace

const ShellEntry *ShellState::entry(const std::string &id) const {
    for (const ShellEntry &e : sidebar) {
        if (!e.header && e.id == id) return &e;
    }
    return nullptr;
}

ShellState shell_state_compose(const std::string &appName,
                               const std::string &exePath,
                               const std::string &bundlePath,
                               const std::string &summary) {
    ShellState s;
    s.appName = appName.empty() ? "appexec" : appName;
    s.exePath = exePath;
    s.exeName = baseName(exePath);
    s.bundlePath = bundlePath;
    s.summary = summary.empty() ? "not loaded" : summary;

    s.sidebar.push_back({"", "LOADED APP", true, false});
    s.sidebar.push_back({"app", s.appName, false, false});
    if (!s.exeName.empty()) s.sidebar.push_back({"exe", s.exeName, false, false});
    s.sidebar.push_back({"plist", "Info.plist", false, false});
    s.sidebar.push_back({"", "BUNDLE", true, false});
    s.sidebar.push_back({"contents", "Contents", false, true});
    s.sidebar.push_back({"macos", "MacOS", false, true});
    s.sidebar.push_back({"resources", "Resources", false, true});
    return s;
}

ShellState shell_state_load() {
    const char *app = std::getenv("PLSR_APP_NAME");
    const char *exe = std::getenv("PLSR_APP_EXE");
    const char *bundle = std::getenv("PLSR_APP_BUNDLE");
    const char *summary = std::getenv("PLSR_APP_SUMMARY");
    ShellState s = shell_state_compose(app ? app : "", exe ? exe : "",
                                       bundle ? bundle : "", summary ? summary : "");
    s.document = shell_entry_content(s, "app");
    return s;
}

void ShellSelection::select(const std::string &id) {
    std::string doc = shell_entry_content(state, id);
    if (doc.empty()) doc = "<!-- " + id + " has no content in " + state.appName + " -->\n";
    if (onDocument) onDocument(std::move(doc));
}

std::string shell_entry_content(const ShellState &state, const std::string &id) {
    if (id == "app" || id == "plist") {
        return readFile(join(state.bundlePath, "Contents/Info.plist"), kTextLimit);
    }
    if (id == "exe") {
        if (state.exePath.empty() || isDirectory(state.exePath)) return {};
        const std::string data = readFile(state.exePath, kBinaryLimit);
        if (data.empty()) return {};
        std::string out = "// " + state.exeName + " - " + std::to_string(data.size()) +
                          " bytes from " + state.exePath + "\n";
        if (looksBinary(data)) {
            out += "// binary; hex dump of the first " + std::to_string(data.size()) +
                   " bytes\n\n";
            out += hexDump(data, kBinaryLimit);
        } else {
            out += "\n";
            out += data;
        }
        return out;
    }
    if (id == "contents") return listDirectory(join(state.bundlePath, "Contents"));
    if (id == "macos") return listDirectory(join(state.bundlePath, "Contents/MacOS"));
    if (id == "resources") return listDirectory(join(state.bundlePath, "Contents/Resources"));
    return {};
}
