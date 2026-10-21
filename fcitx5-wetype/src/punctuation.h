#pragma once

#include <fcitx-utils/utf8.h>
#include <cstdint>
#include <string_view>

namespace wetype {

// Every ASCII symbol WeType owns in the Chinese input state. Where Chinese has
// its own punctuation glyph that glyph wins (，。？！：；、《》【】“”‘’), the rest
// take their fullwidth form (U+FF01-FF5E). `openQuote` selects the opening or
// closing form of the two quote keys because pairing is stateful.
//
// The grave key is deliberately absent: markdown, shell quoting and LaTeX all
// need a literal backtick, and the user asked for 、 on the backslash instead.
inline std::string_view punctuation(char key, bool fullwidth, bool openQuote = true) {
    switch (key) {
        case ',': return fullwidth ? "，" : ",";
        case '.': return fullwidth ? "。" : ".";
        case '?': return fullwidth ? "？" : "?";
        case '!': return fullwidth ? "！" : "!";
        case ':': return fullwidth ? "：" : ":";
        case ';': return fullwidth ? "；" : ";";
        case '\\': return fullwidth ? "、" : "";  // 、 has no ASCII twin
        case '(': return fullwidth ? "（" : "(";
        case ')': return fullwidth ? "）" : ")";
        case '[': return fullwidth ? "【" : "[";
        case ']': return fullwidth ? "】" : "]";
        case '{': return fullwidth ? "｛" : "{";
        case '}': return fullwidth ? "｝" : "}";
        case '<': return fullwidth ? "《" : "<";
        case '>': return fullwidth ? "》" : ">";
        case '"': return fullwidth ? (openQuote ? "“" : "”") : "\"";
        case '\'': return fullwidth ? (openQuote ? "‘" : "’") : "'";
        case '-': return fullwidth ? "－" : "-";
        case '_': return fullwidth ? "＿" : "_";
        case '+': return fullwidth ? "＋" : "+";
        case '=': return fullwidth ? "＝" : "=";
        case '*': return fullwidth ? "＊" : "*";
        case '/': return fullwidth ? "／" : "/";
        case '#': return fullwidth ? "＃" : "#";
        case '$': return fullwidth ? "＄" : "$";
        case '%': return fullwidth ? "％" : "%";
        case '&': return fullwidth ? "＆" : "&";
        case '@': return fullwidth ? "＠" : "@";
        case '^': return fullwidth ? "＾" : "^";
        case '~': return fullwidth ? "～" : "~";
        case '|': return fullwidth ? "｜" : "|";
        default: return {};
    }
}

// An owned key, whether it converts to anything at all.
inline bool ownsPunctuation(char key) {
    return !punctuation(key, true).empty();
}

inline bool isQuote(char key) { return key == '"' || key == '\''; }

// The digit exception covers only the decimal point and the thousands separator:
// "3.14" and "1,000" must stay ASCII, but "3（" or a bracketed list is still
// Chinese text. Keyboards deliver the shifted symbol as its own keysym, so this
// is the whole rule - no Shift bookkeeping.
inline bool digitException(char key) { return key == ',' || key == '.'; }

inline bool isDigitCodepoint(std::uint32_t c) { return c >= '0' && c <= '9'; }

// The only in-text exception to fullwidth punctuation: a digit right before the
// cursor, so "3.14" and "1,000" stay ASCII. Trailing spaces do not cancel it,
// because "总分 3 " + "." still follows a number.
inline bool endsWithDigit(std::string_view text) {
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' ||
                             text.back() == '\n' || text.back() == '\r')) {
        text.remove_suffix(1);
    }
    if (text.empty() || !fcitx::utf8::validate(text)) return false;
    // ASCII digits cannot hide inside a multi-byte sequence: continuation bytes
    // are 0x80-0xBF, so a trailing 0x30-0x39 is a real digit.
    return isDigitCodepoint(static_cast<unsigned char>(text.back()));
}

}  // namespace wetype
