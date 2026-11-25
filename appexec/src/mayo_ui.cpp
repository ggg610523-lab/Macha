// CotEditor-style front-end built on Mayonnaise.
//
// This is the UI half of the clean-room runtime: a native macOS-looking window,
// document sidebar, unified toolbar, syntax-coloured editor, line numbers and a
// status bar.  It is deliberately self-contained so it can be rendered with
// MAYO_SCREENSHOT even when no compositor is available.

#include <mayonnaise/mayonnaise.hpp>

#include "shell_state.h"
#include "widget_spec.h"

extern "C" void plsr_runtime_refresh_app_windows(void);

extern "C" {
#include "engine.h"
}

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstdio>
#include <fstream>
#include <cmath>
#include <functional>
#include <string>
#include <string_view>
#include <condition_variable>
#include <chrono>
#include <mutex>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {

using namespace mayo;

constexpr float kLineHeight = 21.0f;
constexpr float kGutter = 52.0f;
constexpr float kTextInset = 12.0f;
constexpr float kEditorTop = 8.0f;

Icon icon_from_name(std::string_view name) {
    std::string key(name);
    std::transform(key.begin(), key.end(), key.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (key.find("plus") != std::string::npos || key.find("add") != std::string::npos)
        return Icon::Plus;
    if (key.find("folder") != std::string::npos || key.find("open") != std::string::npos)
        return Icon::Folder;
    if (key.find("document") != std::string::npos || key.find("new") != std::string::npos)
        return Icon::Document;
    if (key.find("pencil") != std::string::npos || key.find("edit") != std::string::npos)
        return Icon::Pencil;
    if (key.find("magnif") != std::string::npos || key.find("search") != std::string::npos ||
        key.find("find") != std::string::npos) return Icon::Magnifier;
    if (key.find("sidebar") != std::string::npos) return Icon::Sidebar;
    if (key.find("paint") != std::string::npos || key.find("format") != std::string::npos)
        return Icon::Paintbrush;
    if (key.find("window") != std::string::npos) return Icon::Windows;
    if (key.find("info") != std::string::npos || key.find("help") != std::string::npos)
        return Icon::Info;
    if (key.find("undo") != std::string::npos || key.find("back") != std::string::npos)
        return Icon::ArrowLeft;
    if (key.find("redo") != std::string::npos || key.find("forward") != std::string::npos)
        return Icon::ArrowRight;
    if (key.find("save") != std::string::npos || key.find("disk") != std::string::npos)
        return Icon::HardDrive;
    if (key.find("share") != std::string::npos) return Icon::Share;
    if (key.find("close") != std::string::npos || key.find("stop") != std::string::npos)
        return Icon::Xmark;
    return Icon::None;
}

enum class Token { Plain, Keyword, Type, String, Comment, Number };

bool isKeyword(std::string_view word) {
    static const std::unordered_set<std::string_view> keywords = {
        "import", "final", "class", "override", "func", "let", "var", "if", "else",
        "return", "struct", "enum", "init", "self", "nil", "true", "false", "for",
        "in", "while", "guard", "extension", "protocol", "public", "private",
        "internal", "static", "weak", "unowned", "try", "catch", "throw", "throws",
        "case", "switch", "default", "break", "continue", "defer", "where", "as",
        "is", "some", "any", "open", "fileprivate", "subscript", "typealias",
    };
    return keywords.contains(word);
}

struct Run {
    std::string text;
    Token token = Token::Plain;
};

class EditorView : public Widget {
public:
    explicit EditorView(FontFamily family, std::string text = {})
        : m_family(family), m_text(std::move(text)) {
        m_caret = 0;
    }

    std::function<void(size_t, size_t)> onCaretChanged;
    std::function<void(const std::string&)> onTextChanged;
    std::function<void()> onFocus;

    bool synchronizeText(const std::string& text) {
        if (m_text == text) return false;
        m_text = text;
        m_caret = std::min(m_caret, m_text.size());
        m_selectionAnchor = std::min(m_selectionAnchor, m_text.size());
        m_scroll = std::min(m_scroll, maxScroll());
        m_undo.clear();
        m_redo.clear();
        notifyCaret();
        invalidate();
        return true;
    }

    Size measureContent(Size available) override {
        return {available.width, available.height};
    }
    [[nodiscard]] bool expandsContent(Axis) const override { return true; }
    [[nodiscard]] bool acceptsFocus() const override { return true; }
    [[nodiscard]] bool isInteractive() const override { return true; }
    [[nodiscard]] bool focusOnClick() const override { return true; }

    bool onPointerDown(const PointerEvent& e) override {
        if (auto* h = host()) h->setFocus(this, true);
        if (onFocus) onFocus();
        const Rect r = contentRect();
        const int line = std::clamp(
            static_cast<int>((e.position.y - r.y - kEditorTop + m_scroll) / kLineHeight),
            0, lineCount() - 1);
        const size_t start = lineStart(static_cast<size_t>(line));
        const size_t end = lineEnd(static_cast<size_t>(line));
        const float textX = r.x + kGutter + kTextInset;
        const int column = std::max(0, static_cast<int>(
            (e.position.x - textX + 4.0f) / 8.0f));
        m_caret = std::min(start + static_cast<size_t>(column), end);
        m_selectionAnchor = m_caret;
        notifyCaret();
        updateTextInputArea();
        invalidate();
        return true;
    }

    void onFocusChanged(bool focused) override {
        if (auto* h = host())
            h->setTextInput(focused, focused ? caretRect() : Rect{});
    }

    bool onScroll(const ScrollEvent& e) override {
        m_scroll = std::clamp(m_scroll - e.delta.y, 0.0f, maxScroll());
        invalidate();
        return true;
    }

    bool onKeyDown(const KeyEvent& e) override {
        char menuKey = 0;
        if (e.key >= Key::A && e.key <= Key::Z)
            menuKey = static_cast<char>('a' +
                static_cast<unsigned>(e.key) - static_cast<unsigned>(Key::A));
        else if (e.key >= Key::Num0 && e.key <= Key::Num9)
            menuKey = static_cast<char>('0' +
                static_cast<unsigned>(e.key) - static_cast<unsigned>(Key::Num0));
        else {
            switch (e.key) {
                case Key::Space: menuKey = ' '; break;
                case Key::Comma: menuKey = ','; break;
                case Key::Period: menuKey = '.'; break;
                case Key::Minus: menuKey = '-'; break;
                case Key::Equals: menuKey = '='; break;
                case Key::Slash: menuKey = '/'; break;
                default: break;
            }
        }
        if (std::getenv("PLSR_REEF_TRACE"))
            std::fprintf(stderr,
                "reef: host key raw=%u translated='%c' modifiers=%u primary=%d\n",
                static_cast<unsigned>(e.key), menuKey ? menuKey : '?',
                static_cast<unsigned>(e.modifiers), e.primary() ? 1 : 0);
        if (menuKey) {
            unsigned modifiers = 0;
            if (e.shift()) modifiers |= 1u << 0;
            if (e.primary()) modifiers |= 1u << 1; // platform primary -> Command
            if (e.alt()) modifiers |= 1u << 2;
            if (hasModifier(e.modifiers, Modifiers::Super)) modifiers |= 1u << 3;
            if (plsr_widget_dispatch_menu_shortcut(menuKey, modifiers))
                return true;
        }
        if (e.primary()) {
            switch (e.key) {
                case Key::A:
                    m_selectionAnchor = 0;
                    m_caret = m_text.size();
                    invalidate();
                    return true;
                case Key::C:
                    if (hasSelection() && host())
                        host()->setClipboardText(selectedText());
                    return true;
                case Key::X:
                    if (hasSelection() && host()) {
                        host()->setClipboardText(selectedText());
                        recordEdit();
                        eraseSelection();
                        changed();
                    }
                    return true;
                case Key::V:
                    if (host()) insert(host()->clipboardText());
                    return true;
                case Key::Z:
                    if (e.shift()) redo(); else undo();
                    return true;
                default:
                    return false;
            }
        }
        switch (e.key) {
            case Key::Enter: insert("\n"); return true;
            case Key::Tab: insert("    "); return true;
            case Key::Backspace:
                if (hasSelection()) {
                    recordEdit();
                    eraseSelection();
                    changed();
                } else if (m_caret > 0) {
                    recordEdit();
                    m_text.erase(m_caret - 1, 1);
                    --m_caret;
                    m_selectionAnchor = m_caret;
                    changed();
                }
                return true;
            case Key::Delete:
                if (hasSelection()) {
                    recordEdit();
                    eraseSelection();
                    changed();
                } else if (m_caret < m_text.size()) {
                    recordEdit();
                    m_text.erase(m_caret, 1);
                    m_selectionAnchor = m_caret;
                    changed();
                }
                return true;
            case Key::Left:
                moveHorizontal(-1, e.shift());
                return true;
            case Key::Right:
                moveHorizontal(1, e.shift());
                return true;
            case Key::Up: moveVertical(-1, e.shift()); return true;
            case Key::Down: moveVertical(1, e.shift()); return true;
            case Key::PageUp: moveVertical(-10); return true;
            case Key::PageDown: moveVertical(10); return true;
            case Key::Home:
                m_caret = lineStart(caretPos().first);
                if (!e.shift()) m_selectionAnchor = m_caret;
                notifyCaret();
                invalidate();
                return true;
            case Key::End:
                m_caret = lineEnd(caretPos().first);
                if (!e.shift()) m_selectionAnchor = m_caret;
                notifyCaret();
                invalidate();
                return true;
            default: return false;
        }
    }

    bool onTextInput(const TextInputEvent& e) override {
        if (e.text.empty()) return false;
        std::string text;
        for (char c : e.text) {
            if (c == '\r') text.push_back('\n');
            else if (c == '\t') text += "    ";
            else if (static_cast<unsigned char>(c) >= 0x20) text.push_back(c);
        }
        if (text.empty()) return false;
        insert(text);
        return true;
    }

    void draw(Painter& p) override {
        const Theme& t = theme();
        const Rect r = contentRect();
        p.pushClip(r);
        p.fillRect(r, t.colors.contentBackground);

        const auto lines = splitLines();
        const int first = std::max(0, static_cast<int>(m_scroll / kLineHeight));
        const int visible = static_cast<int>(r.height / kLineHeight) + 2;
        const int last = std::min(static_cast<int>(lines.size()), first + visible);

        p.fillRect({r.x, r.y, kGutter, r.height}, t.colors.contentBackground);
        p.fillRect({r.x + kGutter - 0.5f, r.y, 0.5f, r.height}, t.colors.separator);

        const Font code = makeFont(13.5f);
        const Font numbers = makeFont(11.0f);
        const LineMetrics fm = fontMetrics(code);
        const float baselineOffset = (kLineHeight - (fm.ascender + fm.descender)) * 0.5f + fm.ascender;

        for (int i = first; i < last; ++i) {
            const float y = r.y + kEditorTop + static_cast<float>(i) * kLineHeight - m_scroll;
            if (y + kLineHeight < r.y || y > r.bottom()) continue;

            const std::string number = std::to_string(i + 1);
            p.drawText(number, {r.x + 6, y, kGutter - 18, kLineHeight}, numbers,
                       t.colors.tertiaryLabel, TextAlign::Trailing);

            float x = r.x + kGutter + kTextInset;
            const size_t rowStart = lineStart(static_cast<size_t>(i));
            const size_t rowEnd = rowStart + lines[static_cast<size_t>(i)].size();
            const size_t selectionBegin = std::min(m_caret, m_selectionAnchor);
            const size_t selectionEnd = std::max(m_caret, m_selectionAnchor);
            const size_t selectedBegin = std::max(rowStart, selectionBegin);
            const size_t selectedEnd = std::min(rowEnd, selectionEnd);
            if (selectedEnd > selectedBegin) {
                const auto row = lines[static_cast<size_t>(i)];
                const float sx = x + p.measureText(row.substr(0, selectedBegin - rowStart), code);
                const float sw = p.measureText(
                    row.substr(selectedBegin - rowStart, selectedEnd - selectedBegin), code);
                p.fillRect({sx, y + 2.0f, std::max(sw, 2.0f), kLineHeight - 3.0f},
                           t.colors.accent.withAlpha(0.24f));
            }
            for (const Run& run : highlight(lines[static_cast<size_t>(i)])) {
                const Color color = tokenColor(t, run.token);
                p.drawText(run.text, {x, y + baselineOffset}, code, color);
                x += p.measureText(run.text, code);
            }
        }

        if (isFocused()) {
            const auto [line, column] = caretPos();
            if (line >= static_cast<size_t>(first) && line < static_cast<size_t>(last)) {
                const float x = r.x + kGutter + kTextInset +
                                p.measureText(std::string_view(lines[line]).substr(0, column), code);
                const float y = r.y + kEditorTop + static_cast<float>(line) * kLineHeight - m_scroll;
                const double phase = std::fmod(now(), 1.0);
                if (phase < 0.6) {
                    p.fillRect({x, y + 3.0f, 1.5f, kLineHeight - 6.0f}, t.colors.accent);
                }
            }
        }
        p.popClip();
    }

private:
    Font makeFont(float size) const {
        Font f;
        f.size = size;
        f.weight = FontWeight::Regular;
        f.lineHeight = kLineHeight;
        f.family = m_family;
        return f;
    }

    Color tokenColor(const Theme& t, Token token) const {
        switch (token) {
            case Token::Keyword: return t.colors.purple;
            case Token::Type: return t.colors.blue;
            case Token::String: return t.colors.green;
            case Token::Comment: return t.colors.tertiaryLabel;
            case Token::Number: return t.colors.orange;
            case Token::Plain: break;
        }
        return t.colors.label;
    }

    std::vector<std::string_view> splitLines() const {
        std::vector<std::string_view> lines;
        size_t start = 0;
        while (start <= m_text.size()) {
            const size_t end = m_text.find('\n', start);
            if (end == std::string::npos) {
                lines.emplace_back(m_text.data() + start, m_text.size() - start);
                break;
            }
            lines.emplace_back(m_text.data() + start, end - start);
            start = end + 1;
        }
        return lines;
    }

    std::vector<Run> highlight(std::string_view line) const {
        std::vector<Run> runs;
        size_t i = 0;
        while (i < line.size()) {
            if (line[i] == '/' && i + 1 < line.size() && line[i + 1] == '/') {
                runs.push_back({std::string(line.substr(i)), Token::Comment});
                break;
            }
            if (line[i] == '"') {
                size_t j = i + 1;
                while (j < line.size()) {
                    if (line[j] == '\\' && j + 1 < line.size()) j += 2;
                    else if (line[j] == '"') { ++j; break; }
                    else ++j;
                }
                runs.push_back({std::string(line.substr(i, j - i)), Token::String});
                i = j;
                continue;
            }
            if (std::isalpha(static_cast<unsigned char>(line[i])) || line[i] == '_') {
                size_t j = i + 1;
                while (j < line.size() &&
                       (std::isalnum(static_cast<unsigned char>(line[j])) || line[j] == '_')) {
                    ++j;
                }
                const std::string_view word = line.substr(i, j - i);
                const bool upper = std::isupper(static_cast<unsigned char>(word.front()));
                const Token token = isKeyword(word) ? Token::Keyword
                                    : upper           ? Token::Type
                                                      : Token::Plain;
                runs.push_back({std::string(word), token});
                i = j;
                continue;
            }
            if (std::isdigit(static_cast<unsigned char>(line[i]))) {
                size_t j = i + 1;
                while (j < line.size() &&
                       (std::isalnum(static_cast<unsigned char>(line[j])) || line[j] == '.')) {
                    ++j;
                }
                runs.push_back({std::string(line.substr(i, j - i)), Token::Number});
                i = j;
                continue;
            }
            size_t j = i + 1;
            while (j < line.size() && line[j] != '"' && line[j] != '/' &&
                   !std::isalnum(static_cast<unsigned char>(line[j])) && line[j] != '_') {
                ++j;
            }
            runs.push_back({std::string(line.substr(i, j - i)), Token::Plain});
            i = j;
        }
        return runs;
    }

    int lineCount() const {
        return 1 + static_cast<int>(std::count(m_text.begin(), m_text.end(), '\n'));
    }
    std::pair<size_t, size_t> caretPos() const {
        size_t line = 0, start = 0;
        for (size_t i = 0; i < m_caret && i < m_text.size(); ++i) {
            if (m_text[i] == '\n') {
                ++line;
                start = i + 1;
            }
        }
        return {line, m_caret - start};
    }
    size_t lineStart(size_t line) const {
        size_t p = 0;
        for (size_t i = 0; i < line; ++i) {
            const size_t n = m_text.find('\n', p);
            if (n == std::string::npos) return m_text.size();
            p = n + 1;
        }
        return p;
    }
    size_t lineEnd(size_t line) const {
        const size_t n = m_text.find('\n', lineStart(line));
        return n == std::string::npos ? m_text.size() : n;
    }
    void moveVertical(int delta) {
        moveVertical(delta, false);
    }
    void moveVertical(int delta, bool extendSelection) {
        const auto [line, column] = caretPos();
        const int target = std::clamp(static_cast<int>(line) + delta, 0, lineCount() - 1);
        const size_t start = lineStart(static_cast<size_t>(target));
        m_caret = std::min(start + column, lineEnd(static_cast<size_t>(target)));
        if (!extendSelection) m_selectionAnchor = m_caret;
        notifyCaret();
        invalidate();
    }
    bool hasSelection() const { return m_caret != m_selectionAnchor; }
    std::string selectedText() const {
        const size_t begin = std::min(m_caret, m_selectionAnchor);
        return m_text.substr(begin, std::max(m_caret, m_selectionAnchor) - begin);
    }
    void eraseSelection() {
        const size_t begin = std::min(m_caret, m_selectionAnchor);
        const size_t end = std::max(m_caret, m_selectionAnchor);
        m_text.erase(begin, end - begin);
        m_caret = m_selectionAnchor = begin;
    }
    void moveHorizontal(int delta, bool extendSelection) {
        if (!extendSelection && hasSelection()) {
            m_caret = delta < 0 ? std::min(m_caret, m_selectionAnchor)
                                : std::max(m_caret, m_selectionAnchor);
            m_selectionAnchor = m_caret;
        } else if (delta < 0 && m_caret > 0) {
            --m_caret;
        } else if (delta > 0 && m_caret < m_text.size()) {
            ++m_caret;
        }
        if (!extendSelection) m_selectionAnchor = m_caret;
        notifyCaret();
        invalidate();
    }
public:
    /* Replace the document with real content from the loaded app. */
    void setTextContent(std::string text) {
        m_text = std::move(text);
        m_caret = 0;
        notifyCaret();
        invalidate();
    }
    void insert(std::string_view text) {
        if (text.empty()) return;
        recordEdit();
        if (hasSelection()) eraseSelection();
        m_text.insert(m_caret, text);
        m_caret += text.size();
        m_selectionAnchor = m_caret;
        changed();
    }
    void changed() {
        if (onTextChanged) onTextChanged(m_text);
        notifyCaret();
        invalidate();
    }
    void recordEdit() {
        m_undo.push_back(m_text);
        if (m_undo.size() > 256) m_undo.erase(m_undo.begin());
        m_redo.clear();
    }
    void restoreEdit(std::vector<std::string>& from,
                     std::vector<std::string>& to) {
        if (from.empty()) return;
        to.push_back(m_text);
        m_text = std::move(from.back());
        from.pop_back();
        m_caret = m_selectionAnchor = std::min(m_caret, m_text.size());
        changed();
    }
    void undo() { restoreEdit(m_undo, m_redo); }
    void redo() { restoreEdit(m_redo, m_undo); }
    void notifyCaret() {
        if (onCaretChanged) {
            const auto [line, column] = caretPos();
            onCaretChanged(line + 1, column + 1);
        }
        updateTextInputArea();
    }
    Rect caretRect() const {
        const auto [line, column] = caretPos();
        const Rect r = contentRect();
        return {r.x + kGutter + kTextInset + static_cast<float>(column) * 8.0f,
                r.y + kEditorTop + static_cast<float>(line) * kLineHeight - m_scroll,
                1.0f, kLineHeight};
    }
    void updateTextInputArea() {
        if (auto* h = host(); h && h->focusedWidget() == this)
            h->setTextInput(true, caretRect());
    }
    float maxScroll() const {
        return std::max(0.0f, static_cast<float>(lineCount()) * kLineHeight + 16.0f -
                                  contentRect().height);
    }

    FontFamily m_family;
    std::string m_text;
    size_t m_caret = 0;
    size_t m_selectionAnchor = 0;
    float m_scroll = 0;
    std::vector<std::string> m_undo;
    std::vector<std::string> m_redo;
};

}  // namespace

extern "C" int plsr_mayo_ui_run(int argc, char **argv) {
    (void)argc;
    (void)argv;
    /* Everything shown below comes from the state the loader recovered for the
     * loaded bundle, not from a hard-coded sample. */
    const ShellState state = shell_state_load();
    const std::string &appName = state.appName;
    App app(appName, std::string("org.") + appName + "." + appName);
    auto& window = app.createWindow({.title = std::string("Untitled — ") + appName,
                                     .width = 1040,
                                     .height = 720,
                                     .minWidth = 720,
                                     .minHeight = 460,
                                     .autosaveName = "main"});

    const FontFamily codeFamily = systemFontFamily("monospace").value_or(0);

    const std::string &exeName = state.exeName;
    const std::string &summary = state.summary;
    auto editor = std::make_shared<EditorView>(codeFamily);
    /* The same handler serves the initial entry and sidebar clicks, so the
     * test exercises exactly this path.  Sidebar::select() does not notify for
     * an unchanged id, hence the explicit initial select below. */
    auto selection = std::make_shared<ShellSelection>(ShellSelection{state, {}});
    selection->onDocument = [editor](const std::string &text) {
        editor->setTextContent(text);
    };
    auto position = Label("Ln 1, Col 1").secondary();
    auto mode = Label(exeName.empty() ? "Loaded" : exeName).secondary();
    auto encoding = Label("UTF-8").secondary();
    editor->onCaretChanged = [position](size_t line, size_t column) mutable {
        position.setText("Ln " + std::to_string(line) + ", Col " + std::to_string(column));
    };

    std::vector<SidebarItem> items;
    for (const ShellEntry &e : state.sidebar) {
        if (e.header) {
            items.push_back(SidebarItem::header(e.title));
        } else {
            items.push_back(SidebarItem(e.id, e.folder ? Icon::Folder : Icon::Document, e.title));
        }
    }
    auto sidebar = Sidebar(std::move(items));
    sidebar.selectionStyle(SidebarSelectionStyle::Accent);
    sidebar.onSelect([selection](const std::string &id) { selection->select(id); });
    sidebar.selection("app");
    selection->select("app");   /* open the bundle's Info.plist */

    auto detail = VStack({
        HStack({
            Label(appName).font(TextStyle::Headline),
            Spacer(),
            Label(summary).secondary(),
        }).padding(Insets(10, 16, 6, 16)),
        Divider(),
        Element(editor),
        Divider(),
        HStack({
            mode,
            Spacer(),
            position,
            Label("  "),
            encoding,
        }).padding(Insets(6, 16, 8, 16)),
    });
    detail.padding(Insets(52, 0, 0, 0));

    window.setContent(SplitView(sidebar, detail).sidebarWidth(232));
    app.after(0.05, [editor] {
        if (auto* h = editor->host()) h->setFocus(editor.get(), true);
    });
    window.setToolbar(Toolbar({
        Button(Icon::Plus).style(ButtonStyle::Borderless).tooltip("New document"),
        Button(Icon::Folder).style(ButtonStyle::Borderless).tooltip("Open"),
        Button(Icon::Share).style(ButtonStyle::Borderless).tooltip("Share"),
        Spacer(),
        Button(Icon::Play).style(ButtonStyle::Borderless).tooltip("Run"),
        Button(Icon::Gear).style(ButtonStyle::Borderless).tooltip("Settings"),
    }));

    window.onKey([&](const KeyEvent& e) {
        if (!e.primary()) return false;
        if (e.key == Key::Q) {
            window.close();
            return true;
        }
        return false;
    });

    return app.run();
}

namespace {
struct RuntimeMayoWindow {
    void *nsWindow = nullptr;
    std::string title;
    int width = 0;
    int height = 0;
    sw_node *tree = nullptr;
    plsr_widget *widgets = nullptr;
    Window *hostWindow = nullptr;
    std::shared_ptr<Widget> hostContent;
};

extern "C" void plsr_appkit_set_window_content_size(void *nsWindow,
                                                       double width,
                                                       double height);

std::mutex g_runtime_mayo_mutex;
std::condition_variable g_runtime_mayo_ready_cv;
std::vector<RuntimeMayoWindow> g_runtime_mayo_windows;
bool g_runtime_mayo_started = false;
bool g_runtime_mayo_ready = false;
bool g_runtime_mayo_failed = false;

class RuntimeMayoHost;
RuntimeMayoHost *g_runtime_mayo_host = nullptr;

struct SwiftUIPaintContext {
    Painter *painter = nullptr;
    float originX = 0;
    float originY = 0;
    int rects = 0;
    int texts = 0;
};

void swiftui_rect(void *context, sw_rect r, sw_color c, double radius) {
    auto *ctx = static_cast<SwiftUIPaintContext *>(context);
    if (!ctx || !ctx->painter) return;
    ctx->rects++;
    (void)radius;
    ctx->painter->fillRect({ctx->originX + static_cast<float>(r.x),
                            ctx->originY + static_cast<float>(r.y),
                            static_cast<float>(r.width),
                            static_cast<float>(r.height)},
                           {static_cast<float>(c.r), static_cast<float>(c.g),
                            static_cast<float>(c.b), static_cast<float>(c.a)});
}

void swiftui_text(void *context, const char *text, sw_rect r, sw_font f,
                  sw_color c) {
    auto *ctx = static_cast<SwiftUIPaintContext *>(context);
    if (!ctx || !ctx->painter || !text) return;
    ctx->texts++;
    Font font;
    font.size = static_cast<float>(f.size);
    font.lineHeight = font.size * 1.25f;
    ctx->painter->drawText(text,
        {ctx->originX + static_cast<float>(r.x),
         ctx->originY + static_cast<float>(r.y),
         static_cast<float>(r.width), static_cast<float>(r.height)},
        font,
        {static_cast<float>(c.r), static_cast<float>(c.g),
         static_cast<float>(c.b), static_cast<float>(c.a)},
        TextAlign::Leading);
}

/* Hosts the clean-room SwiftUI tree extracted from the app's NSWindow.  The
 * engine lays the tree out at the widget's size and emits rectangles and text
 * through sw_paint; the painter adapter forwards those to Mayonnaise. */
class SwiftUITreeView : public Widget {
public:
    explicit SwiftUITreeView(sw_node *root) : m_root(root) {}

    Size measureContent(Size available) override { return available; }
    [[nodiscard]] bool expandsContent(Axis) const override { return true; }

    void draw(Painter& painter) override {
        if (!m_root) return;
        const Rect r = contentRect();
        sw_layout(m_root, sw_proposal_size(static_cast<double>(r.width),
                                           static_cast<double>(r.height)));
        SwiftUIPaintContext ctx{&painter, r.x, r.y};
        sw_painter bridge{&ctx, swiftui_rect, swiftui_text};
        sw_paint(m_root, nullptr, &bridge);
        static int reports = 0;
        if (reports++ < 3)
            std::fprintf(stderr,
                         "reef: swiftui paint rects=%d texts=%d size=%.0fx%.0f\n",
                         ctx.rects, ctx.texts, r.width, r.height);
    }

private:
    sw_node *m_root = nullptr;
};

/* AppKit frames use a bottom-left origin unless the parent view is flipped.
 * Lay out host widgets at the same per-view rectangles rather than replacing
 * the application's layout with a generic stack. */
class AppKitFrameView final : public Widget {
public:
    void add(const plsr_widget *spec, std::shared_ptr<Widget> child) {
        if (!spec || !child) return;
        m_specs.push_back(spec);
        addChild(std::move(child));
    }

    bool focusSourceResponder() {
        const auto& nodes = children();
        for (size_t i = 0; i < nodes.size() && i < m_specs.size(); ++i) {
            const auto& child = nodes[i];
            if (!child) continue;
            if (m_specs[i]->focused && child->acceptsFocus()) {
                if (std::getenv("PLSR_REEF_TRACE"))
                    std::fprintf(stderr, "reef: source first responder mapped kind=%d\n",
                                 static_cast<int>(m_specs[i]->kind));
                if (auto *widgetHost = child->host())
                    widgetHost->setFocus(child.get(), true);
                return true;
            }
            if (auto nested = std::dynamic_pointer_cast<AppKitFrameView>(child);
                nested && nested->focusSourceResponder())
                return true;
        }
        return false;
    }

    Size measureContent(Size available) override { return available; }
    [[nodiscard]] bool expandsContent(Axis) const override { return true; }

    void arrangeContent(const Rect& content) override {
        const auto& children = this->children();
        bool hasFrames = false;
        for (const auto *spec : m_specs)
            hasFrames = hasFrames || (spec && spec->has_frame);
        const float gap = 4.0f;
        const float fallbackH = m_specs.empty() ? 0.0f
            : std::max(24.0f, (content.height - gap * (m_specs.size() - 1)) /
                               static_cast<float>(m_specs.size()));
        float cursorY = content.y;
        float cursorX = content.x;
        for (size_t i = 0; i < children.size() && i < m_specs.size(); ++i) {
            const auto *spec = m_specs[i];
            auto& child = children[i];
            if (!spec || !child) continue;
            child->setHidden(spec->hidden);
            child->setDisabled(!spec->enabled);
            if (std::isfinite(spec->alpha))
                child->setOpacity(static_cast<float>(std::clamp(spec->alpha, 0.0, 1.0)));

            Rect frame{};
            if (hasFrames && spec->has_frame) {
                float w = static_cast<float>(spec->width);
                float h = static_cast<float>(spec->height);
                if (w <= 0.0f) w = content.width;
                if (h <= 0.0f) h = content.height;
                float y = static_cast<float>(spec->y);
                if (!spec->flipped) y = content.height - y - h;
                frame = {content.x + static_cast<float>(spec->x),
                         content.y + y, w, h};
            } else if (hasFrames) {
                frame = {content.x, content.y, content.width, content.height};
            } else if (spec->kind == PLSR_WIDGET_CONTAINER_H) {
                float w = children.empty() ? 0.0f
                    : (content.width - gap * (children.size() - 1)) /
                          static_cast<float>(children.size());
                frame = {cursorX, content.y, w, content.height};
                cursorX += w + gap;
            } else {
                frame = {content.x, cursorY, content.width, fallbackH};
                cursorY += fallbackH + gap;
            }
            child->measure(frame.size());
            child->arrange(frame);
        }
    }

    bool refresh();

private:
    std::vector<const plsr_widget *> m_specs;
};

static void apply_appkit_state(const plsr_widget *spec,
                               const std::shared_ptr<Widget>& widget) {
    if (!spec || !widget) return;
    widget->setHidden(spec->hidden);
    widget->setDisabled(!spec->enabled);
    if (std::isfinite(spec->alpha))
        widget->setOpacity(static_cast<float>(std::clamp(spec->alpha, 0.0, 1.0)));
}

static bool refresh_control_widget(const plsr_widget *spec,
                                   const std::shared_ptr<Widget>& widget);

static bool refresh_appkit_state(const plsr_widget *spec,
                                 const std::shared_ptr<Widget>& widget) {
    if (!spec || !widget) return false;
    bool changed = false;
    if (widget->isVisible() == (spec->hidden != 0)) {
        widget->setHidden(spec->hidden != 0);
        changed = true;
    }
    if (widget->isDisabledSelf() != (spec->enabled == 0)) {
        widget->setDisabled(spec->enabled == 0);
        changed = true;
    }
    if (std::isfinite(spec->alpha))
        widget->setOpacity(static_cast<float>(std::clamp(spec->alpha, 0.0, 1.0)));
    return changed;
}

static bool refresh_control_widget(const plsr_widget *spec,
                                   const std::shared_ptr<Widget>& widget) {
    if (!spec || !widget) return false;
    bool changed = refresh_appkit_state(spec, widget);
    if (auto frame = std::dynamic_pointer_cast<AppKitFrameView>(widget)) {
        changed = frame->refresh() || changed;
        return changed;
    }
    if (auto label = std::dynamic_pointer_cast<LabelView>(widget)) {
        const std::string text = spec->text ? spec->text : "";
        if (label->text() != text) {
            label->setText(text);
            changed = true;
        }
    } else if (auto button = std::dynamic_pointer_cast<ButtonView>(widget)) {
        const std::string title = spec->text ? spec->text : "";
        if (button->title() != title) {
            button->setTitle(title);
            changed = true;
        }
    } else if (auto field = std::dynamic_pointer_cast<TextFieldView>(widget)) {
        const std::string text = spec->text ? spec->text : "";
        if (field->text() != text) {
            field->setText(text);
            changed = true;
        }
    } else if (auto editor = std::dynamic_pointer_cast<EditorView>(widget)) {
        changed = editor->synchronizeText(spec->text ? spec->text : "") || changed;
    } else if (auto toggle = std::dynamic_pointer_cast<BooleanControlView>(widget)) {
        if (toggle->isOn() != (spec->selected != 0)) {
            toggle->setOn(spec->selected != 0, false);
            changed = true;
        }
    } else if (auto slider = std::dynamic_pointer_cast<SliderView>(widget)) {
        if (std::abs(slider->value() - static_cast<float>(spec->value)) > 0.0001f) {
            slider->setValue(static_cast<float>(spec->value));
            changed = true;
        }
    } else if (auto menu = std::dynamic_pointer_cast<MenuButtonView>(widget)) {
        const std::string title = spec->text ? spec->text : "";
        if (menu->title() != title) {
            menu->setTitle(title);
            changed = true;
        }
        std::vector<MenuItem> items;
        items.reserve(spec->nkids);
        for (size_t i = 0; i < spec->nkids; ++i) {
            const auto *option = spec->kids[i];
            if (option && option->kind == PLSR_WIDGET_SEPARATOR) {
                items.push_back(MenuItem::separator());
                continue;
            }
            if (!option || !option->text) continue;
            void *item = option->view;
            const std::string itemTitle(option->text);
            MenuItem entry(itemTitle, [item, itemTitle] {
                if (std::getenv("PLSR_REEF_TRACE"))
                    std::fprintf(stderr,
                        "reef: host menu activate title='%s' item=%p\n",
                        itemTitle.c_str(), item);
                plsr_widget_action(item, PLSR_WIDGET_ACTION_MENU, nullptr);
            });
            entry.disabled(!option->enabled);
            entry.setChecked(option->selected);
            const Icon icon = option->glyph
                ? icon_from_name(option->glyph) : Icon::None;
            if (icon != Icon::None) entry.withIcon(icon);
            items.push_back(std::move(entry));
        }
        const auto& current = menu->items();
        bool itemsChanged = current.size() != items.size();
        for (size_t i = 0; !itemsChanged && i < items.size(); ++i) {
            itemsChanged = current[i].title != items[i].title ||
                current[i].enabled != items[i].enabled ||
                current[i].checked != items[i].checked ||
                current[i].icon != items[i].icon ||
                current[i].isSeparator != items[i].isSeparator;
        }
        if (itemsChanged) menu->setItems(std::move(items));
    } else if (auto picker = std::dynamic_pointer_cast<PickerView>(widget)) {
        std::vector<std::string> options;
        options.reserve(spec->nkids);
        for (size_t i = 0; i < spec->nkids; ++i)
            if (spec->kids[i] && spec->kids[i]->text)
                options.emplace_back(spec->kids[i]->text);
        if (picker->options() != options) picker->setOptions(std::move(options));
        picker->setSelected(static_cast<int>(spec->value), false);
    }
    if (changed) widget->invalidate();
    return changed;
}

bool AppKitFrameView::refresh() {
    const auto& nodes = children();
    bool changed = false;
    for (size_t i = 0; i < nodes.size() && i < m_specs.size(); ++i)
        changed = refresh_control_widget(m_specs[i], nodes[i]) || changed;
    if (changed) invalidateLayout();
    return changed;
}

/* Map the common AppKit control families to native host controls. Every
 * callback stays attached to the original object and selector contract. */
std::shared_ptr<Widget> build_control_tree(const plsr_widget *spec, int depth) {
    if (!spec || depth > 8) return nullptr;
    std::shared_ptr<Widget> result;
    switch (spec->kind) {
        case PLSR_WIDGET_LABEL:
            result = std::make_shared<LabelView>(spec->text ? spec->text : "");
            break;
        case PLSR_WIDGET_BUTTON: {
            auto button = std::make_shared<ButtonView>(spec->text ? spec->text : "");
            Icon icon = spec->glyph ? icon_from_name(spec->glyph) : Icon::None;
            if (icon != Icon::None) button->setIcon(icon);
            void *view = spec->view;
            const std::string command = spec->detail ? spec->detail : "";
            button->setOnClick([view, command] {
                plsr_widget_action(view,
                    command.empty() ? PLSR_WIDGET_ACTION_CLICK
                                    : PLSR_WIDGET_ACTION_COMMAND,
                    command.empty() ? nullptr : command.c_str());
            });
            result = button;
            break;
        }
        case PLSR_WIDGET_TEXTFIELD: {
            auto field = std::make_shared<TextFieldView>(spec->detail ? spec->detail : "");
            if (spec->text) field->setText(spec->text);
            void *view = spec->view;
            field->setOnChange([view](const std::string &value) {
                plsr_widget_action(view, PLSR_WIDGET_ACTION_TEXT, value.c_str());
            });
            result = field;
            break;
        }
        case PLSR_WIDGET_EDITOR: {
            /* Keep the editor affordances visible even for an empty new
             * document: gutter, first line and a focused caret. */
            auto editor = std::make_shared<EditorView>(
                systemFontFamily("monospace").value_or(0),
                spec->text ? spec->text : "");
            void *view = spec->view;
            editor->onTextChanged = [view](const std::string &value) {
                plsr_widget_action(view, PLSR_WIDGET_ACTION_TEXT, value.c_str());
            };
            editor->onFocus = [view] {
                plsr_widget_action(view, PLSR_WIDGET_ACTION_FOCUS, nullptr);
            };
            result = editor;
            break;
        }
        case PLSR_WIDGET_CHECKBOX: {
            auto control = std::make_shared<CheckboxView>(
                spec->text ? spec->text : "", spec->selected != 0);
            void *view = spec->view;
            control->setOnChange([view](bool) {
                plsr_widget_action(view, PLSR_WIDGET_ACTION_CLICK, nullptr);
            });
            result = control;
            break;
        }
        case PLSR_WIDGET_TOGGLE: {
            auto control = std::make_shared<ToggleView>(
                spec->text ? spec->text : "", spec->selected != 0);
            void *view = spec->view;
            control->setOnChange([view](bool) {
                plsr_widget_action(view, PLSR_WIDGET_ACTION_CLICK, nullptr);
            });
            result = control;
            break;
        }
        case PLSR_WIDGET_SLIDER: {
            auto control = std::make_shared<SliderView>(
                static_cast<float>(spec->value),
                static_cast<float>(spec->minimum),
                static_cast<float>(spec->maximum));
            void *view = spec->view;
            control->setOnChange([view](float value) {
                char text[48];
                std::snprintf(text, sizeof text, "%.9g", value);
                plsr_widget_action(view, PLSR_WIDGET_ACTION_VALUE, text);
            });
            result = control;
            break;
        }
        case PLSR_WIDGET_PROGRESS: {
            float range = static_cast<float>(spec->maximum - spec->minimum);
            std::optional<float> value = range > 0.0f
                ? std::optional<float>((static_cast<float>(spec->value - spec->minimum)) / range)
                : std::nullopt;
            result = std::make_shared<ProgressBarView>(value);
            break;
        }
        case PLSR_WIDGET_SEGMENTED: {
            std::vector<Segment> segments;
            for (size_t i = 0; i < spec->nkids; ++i)
                if (spec->kids[i] && spec->kids[i]->text)
                    segments.emplace_back(spec->kids[i]->text);
            if (segments.empty() && spec->text) segments.emplace_back(spec->text);
            /* An empty source segmented control has no segment to display.
             * Do not invent one in the host window. */
            if (segments.empty()) return nullptr;
            auto control = std::make_shared<SegmentedControlView>(
                std::move(segments), static_cast<int>(spec->value));
            void *view = spec->view;
            control->setOnChange([view](int selected) {
                char text[32];
                std::snprintf(text, sizeof text, "%d", selected);
                plsr_widget_action(view, PLSR_WIDGET_ACTION_VALUE, text);
            });
            result = control;
            break;
        }
        case PLSR_WIDGET_MENU_BUTTON: {
            std::vector<std::string> options;
            for (size_t i = 0; i < spec->nkids; ++i)
                if (spec->kids[i] && spec->kids[i]->text)
                    options.emplace_back(spec->kids[i]->text);
            if (options.empty()) {
                auto button = std::make_shared<ButtonView>(
                    spec->text ? spec->text : "");
                void *view = spec->view;
                button->setOnClick([view] {
                    plsr_widget_action(view, PLSR_WIDGET_ACTION_CLICK, nullptr);
                });
                result = button;
            } else if (spec->view && spec->nkids && !spec->detail) {
                std::vector<MenuItem> items;
                items.reserve(spec->nkids);
                for (size_t i = 0; i < spec->nkids; ++i) {
                    const auto *option = spec->kids[i];
                    if (option && option->kind == PLSR_WIDGET_SEPARATOR) {
                        items.push_back(MenuItem::separator());
                        continue;
                    }
                    if (!option || !option->text) continue;
                    void *item = option->view;
                    const std::string itemTitle(option->text);
                    MenuItem entry(itemTitle, [item, itemTitle] {
                        if (std::getenv("PLSR_REEF_TRACE"))
                            std::fprintf(stderr,
                                "reef: host menu activate title='%s' item=%p\n",
                                itemTitle.c_str(), item);
                        plsr_widget_action(item, PLSR_WIDGET_ACTION_MENU, nullptr);
                    });
                    entry.disabled(!option->enabled);
                    entry.setChecked(option->selected);
                    const Icon itemIcon = option->glyph
                        ? icon_from_name(option->glyph) : Icon::None;
                    if (itemIcon != Icon::None) entry.withIcon(itemIcon);
                    items.push_back(std::move(entry));
                }
                auto menu = std::make_shared<MenuButtonView>(
                    spec->text ? spec->text : "Menu", std::move(items));
                Icon icon = spec->glyph ? icon_from_name(spec->glyph) : Icon::None;
                if (icon != Icon::None) menu->setIcon(icon);
                result = menu;
            } else {
                auto picker = std::make_shared<PickerView>(
                    std::move(options), static_cast<int>(spec->value));
                void *view = spec->view;
                picker->setOnChange([view](int selected) {
                    char text[32];
                    std::snprintf(text, sizeof text, "%d", selected);
                    plsr_widget_action(view, PLSR_WIDGET_ACTION_VALUE, text);
                });
                result = picker;
            }
            break;
        }
        case PLSR_WIDGET_IMAGE:
            /* The bridge does not yet extract NSImage pixels. Leaving the
             * image unpainted is more faithful than substituting fake text. */
            return nullptr;
        case PLSR_WIDGET_SEPARATOR:
            result = std::make_shared<DividerView>();
            break;
        case PLSR_WIDGET_CONTAINER_V:
        case PLSR_WIDGET_CONTAINER_H:
        case PLSR_WIDGET_LIST: {
            auto container = std::make_shared<AppKitFrameView>();
            for (size_t i = 0; i < spec->nkids; ++i)
                container->add(spec->kids[i], build_control_tree(spec->kids[i], depth + 1));
            if (container->children().empty()) return nullptr;
            result = container;
            break;
        }
        default: {
            if (spec->nkids) {
                auto container = std::make_shared<AppKitFrameView>();
                for (size_t i = 0; i < spec->nkids; ++i)
                    container->add(spec->kids[i], build_control_tree(spec->kids[i], depth + 1));
                if (!container->children().empty()) result = container;
            }
            break;
        }
    }
    apply_appkit_state(spec, result);
    return result;
}

bool focus_first_editor(const std::shared_ptr<Widget>& widget) {
    if (!widget) return false;
    if (auto editor = std::dynamic_pointer_cast<EditorView>(widget)) {
        if (auto *host = editor->host()) host->setFocus(editor.get(), true);
        return true;
    }
    for (const auto& child : widget->children())
        if (focus_first_editor(child)) return true;
    return false;
}

class RuntimeMayoHost {
public:
    explicit RuntimeMayoHost(const std::string& name)
        : app(name, "org.pulsar.runtime." + name) {}

    void requestSync() {
        app.post([this] { syncWindows(); });
    }

    int run() {
        syncWindows();
        auto pump = std::make_shared<std::function<void()>>();
        std::weak_ptr<std::function<void()>> weakPump = pump;
        *pump = [this, weakPump] {
            plsr_widget_drain_actions();
            plsr_runtime_refresh_app_windows();
            if (auto keep = weakPump.lock())
                app.after(0.016, [keep] { (*keep)(); });
        };
        app.after(0.016, [pump] { (*pump)(); });
        const int result = app.run();
        *pump = {};
        return result;
    }

private:
    void syncWindows() {
        std::vector<RuntimeMayoWindow> snapshot;
        {
            std::lock_guard lock(g_runtime_mayo_mutex);
            snapshot = g_runtime_mayo_windows;
        }
        while (windows.size() < snapshot.size()) {
            const auto& item = snapshot[windows.size()];
            WindowOptions options;
            options.title = item.title;
            options.width = item.width;
            options.height = item.height;
            options.minWidth = 1;
            options.minHeight = 1;
            auto& window = app.createWindow(options);
            std::shared_ptr<Widget> controls =
                item.widgets ? build_control_tree(item.widgets, 0) : nullptr;
            std::shared_ptr<Widget> hostContent = controls;
            if (controls) {
                window.setContent(Element(controls));
                auto root = std::dynamic_pointer_cast<AppKitFrameView>(controls);
                const bool sourceFocus = root && root->focusSourceResponder();
                if (std::getenv("PLSR_REEF_TRACE"))
                    std::fprintf(stderr, "reef: source responder mapping=%d; editor fallback=%d\n",
                                 sourceFocus ? 1 : 0,
                                 sourceFocus ? 0 : focus_first_editor(controls));
                else if (!sourceFocus)
                    focus_first_editor(controls);
            } else if (item.tree) {
                std::shared_ptr<Widget> treeView =
                    std::make_shared<SwiftUITreeView>(item.tree);
                hostContent = treeView;
                window.setContent(Element(treeView));
            } else {
                auto title = std::make_unique<Label>(item.title);
                title->font(TextStyle::Headline);
                auto details = Label("AppKit NSWindow · " + std::to_string(item.width) +
                                     " × " + std::to_string(item.height)).secondary();
                window.setContent(VStack({
                    title->padding(Insets(14, 18, 6, 18)),
                    Divider(),
                    details.padding(Insets(8, 18, 14, 18)),
                }));
                titleLabels.push_back(std::move(title));
            }
            const void *sourceWindow = item.nsWindow;
            {
                std::lock_guard lock(g_runtime_mayo_mutex);
                auto live = std::find_if(g_runtime_mayo_windows.begin(),
                    g_runtime_mayo_windows.end(),
                    [sourceWindow](const RuntimeMayoWindow& candidate) {
                        return candidate.nsWindow == sourceWindow;
                    });
                if (live != g_runtime_mayo_windows.end())
                    live->hostWindow = &window;
                if (live != g_runtime_mayo_windows.end())
                    live->hostContent = hostContent;
            }
            window.onClosed([sourceWindow] {
                plsr_widget_action(const_cast<void *>(sourceWindow),
                                   PLSR_WIDGET_ACTION_CLOSE, nullptr);
                plsr_widget_drain_actions();
                std::lock_guard lock(g_runtime_mayo_mutex);
                std::erase_if(g_runtime_mayo_windows,
                    [sourceWindow](const RuntimeMayoWindow& item) {
                        return item.nsWindow == sourceWindow;
                    });
            });
            window.onResized([sourceWindow](Size size) {
                plsr_appkit_set_window_content_size(
                    const_cast<void *>(sourceWindow),
                    static_cast<double>(size.width),
                    static_cast<double>(size.height));
            });
            window.raise();
            windows.push_back(&window);
        }
        if (!windows.empty()) {
            {
                std::lock_guard lock(g_runtime_mayo_mutex);
                g_runtime_mayo_ready = true;
            }
            g_runtime_mayo_ready_cv.notify_all();
        }
    }

    App app;
    std::vector<Window *> windows;
    std::vector<std::unique_ptr<Label>> titleLabels;
};

void run_runtime_mayo(std::string appName) {
    try {
        RuntimeMayoHost host(appName);
        {
            std::lock_guard lock(g_runtime_mayo_mutex);
            g_runtime_mayo_host = &host;
        }
        host.run();
        {
            std::lock_guard lock(g_runtime_mayo_mutex);
            g_runtime_mayo_host = nullptr;
        }
    } catch (...) {
        {
            std::lock_guard lock(g_runtime_mayo_mutex);
            g_runtime_mayo_host = nullptr;
            g_runtime_mayo_failed = true;
        }
        g_runtime_mayo_ready_cv.notify_all();
    }
}
}  // namespace

/* Runtime NSWindow presentation bridge.  NSWindow remains the app-facing
 * object; Mayonnaise owns the Linux desktop surface and mirrors its title and
 * content size.  This path is separate from the sample editor above. */
extern "C" int plsr_mayo_runtime_window_is_open(void *nsWindow) {
    if (!nsWindow) return 0;
    std::lock_guard lock(g_runtime_mayo_mutex);
    return std::any_of(g_runtime_mayo_windows.begin(), g_runtime_mayo_windows.end(),
        [nsWindow](const RuntimeMayoWindow& item) {
            return item.nsWindow == nsWindow;
        });
}

extern "C" void plsr_mayo_runtime_update_widgets(void *nsWindow,
                                                    plsr_widget *widgets) {
    if (!nsWindow || !widgets) return;
    std::shared_ptr<Widget> content;
    {
        std::lock_guard lock(g_runtime_mayo_mutex);
        auto live = std::find_if(g_runtime_mayo_windows.begin(),
            g_runtime_mayo_windows.end(),
            [nsWindow](const RuntimeMayoWindow& candidate) {
                return candidate.nsWindow == nsWindow;
            });
        if (live != g_runtime_mayo_windows.end())
            content = live->hostContent;
    }
    if (content) refresh_control_widget(widgets, content);
}

extern "C" void plsr_mayo_runtime_update_title(void *nsWindow,
                                                   const char *title) {
    if (!nsWindow || !title) return;
    Window *hostWindow = nullptr;
    std::string updated(title);
    {
        std::lock_guard lock(g_runtime_mayo_mutex);
        auto live = std::find_if(g_runtime_mayo_windows.begin(),
            g_runtime_mayo_windows.end(),
            [nsWindow](const RuntimeMayoWindow& candidate) {
                return candidate.nsWindow == nsWindow;
            });
        if (live != g_runtime_mayo_windows.end() && live->title != updated) {
            live->title = updated;
            hostWindow = live->hostWindow;
        }
    }
    if (hostWindow) hostWindow->setTitle(updated);
}

extern "C" void plsr_mayo_runtime_update_size(void *nsWindow,
                                                 double width,
                                                 double height) {
    if (!nsWindow || !std::isfinite(width) || !std::isfinite(height) ||
        width <= 0.0 || height <= 0.0) return;
    Window *hostWindow = nullptr;
    {
        std::lock_guard lock(g_runtime_mayo_mutex);
        auto live = std::find_if(g_runtime_mayo_windows.begin(),
            g_runtime_mayo_windows.end(),
            [nsWindow](const RuntimeMayoWindow& candidate) {
                return candidate.nsWindow == nsWindow;
            });
        if (live != g_runtime_mayo_windows.end())
            hostWindow = live->hostWindow;
    }
    if (!hostWindow) return;
    const Size current = hostWindow->size();
    if (std::abs(current.width - static_cast<float>(width)) > 1.0f ||
        std::abs(current.height - static_cast<float>(height)) > 1.0f)
        hostWindow->setSize(Size(static_cast<float>(width),
                                 static_cast<float>(height)));
}

extern "C" int plsr_mayo_runtime_window_open(void *nsWindow, const char *title,
                                                int width, int height,
                                                sw_node *tree,
                                                plsr_widget *widgets) {
    if (!nsWindow || width <= 0 || height <= 0) return 0;
    std::unique_lock lock(g_runtime_mayo_mutex);
    auto found = std::find_if(g_runtime_mayo_windows.begin(), g_runtime_mayo_windows.end(),
        [nsWindow](const RuntimeMayoWindow& item) { return item.nsWindow == nsWindow; });
    if (found == g_runtime_mayo_windows.end()) {
        g_runtime_mayo_windows.push_back(
            {nsWindow, title ? title : "Application", width, height, tree,
             widgets, nullptr, {}});
    }
    bool startNow = false;
    bool runOnMain = false;
    std::string appName;
    if (!g_runtime_mayo_started) {
        g_runtime_mayo_started = true;
        appName = title && *title ? title : "Application";
        runOnMain = getenv("PLSR_MAYO_MAIN_THREAD") != nullptr;
        startNow = true;
    } else if (g_runtime_mayo_host) {
        g_runtime_mayo_host->requestSync();
    }
    if (startNow) {
        lock.unlock();
        if (runOnMain)
            run_runtime_mayo(appName);
        else
            std::thread([appName] { run_runtime_mayo(appName); }).detach();
        if (runOnMain) {
            std::lock_guard doneLock(g_runtime_mayo_mutex);
            const bool presented = g_runtime_mayo_ready;
            g_runtime_mayo_started = false;
            g_runtime_mayo_ready = false;
            g_runtime_mayo_failed = false;
            return presented ? 1 : 0;
        }
        lock.lock();
    }
    g_runtime_mayo_ready_cv.wait_for(lock, std::chrono::seconds(10), [] {
        return g_runtime_mayo_ready || g_runtime_mayo_failed;
    });
    return g_runtime_mayo_ready ? 1 : 0;
}
