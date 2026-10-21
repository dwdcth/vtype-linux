#include "punctuation.h"

#include <cctype>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

void expectNamedForms() {
    const struct { char key; std::string_view full; std::string_view half; } cases[] = {
        {',', "，", ","},   {'.', "。", "."},    {'?', "？", "?"},  {'!', "！", "!"},
        {':', "：", ":"},   {';', "；", ";"},    {'(', "（", "("},  {')', "）", ")"},
        {'[', "【", "["},   {']', "】", "]"},    {'<', "《", "<"},  {'>', "》", ">"},
        {'{', "｛", "{"},   {'}', "｝", "}"},    {'|', "｜", "|"},  {'~', "～", "~"},
        {'@', "＠", "@"},   {'#', "＃", "#"},    {'$', "＄", "$"},  {'%', "％", "%"},
        {'&', "＆", "&"},   {'*', "＊", "*"},    {'+', "＋", "+"},  {'=', "＝", "="},
        {'-', "－", "-"},   {'_', "＿", "_"},    {'/', "／", "/"},  {'^', "＾", "^"},
    };
    for (const auto &c : cases) {
        if (wetype::punctuation(c.key, true) != c.full)
            throw std::runtime_error(std::string("wrong fullwidth for ") + c.key);
        if (wetype::punctuation(c.key, false) != std::string_view(&c.key, 1))
            throw std::runtime_error(std::string("wrong halfwidth for ") + c.key);
        if (!wetype::ownsPunctuation(c.key))
            throw std::runtime_error(std::string("key not owned: ") + c.key);
    }
    // The backslash is the 、 key and has no ASCII twin: the caller must let the
    // key through in the halfwidth case rather than commit an empty string.
    if (wetype::punctuation('\\', true) != "、" || !wetype::punctuation('\\', false).empty())
        throw std::runtime_error("wrong backslash/、 handling");
}

void expectQuotePairing() {
    if (wetype::punctuation('"', true, true) != "“" ||
        wetype::punctuation('"', true, false) != "”" ||
        wetype::punctuation('\'', true, true) != "‘" ||
        wetype::punctuation('\'', true, false) != "’")
        throw std::runtime_error("quote forms must depend on the open flag");
    if (wetype::punctuation('"', false, true) != "\"" ||
        wetype::punctuation('\'', false, false) != "'")
        throw std::runtime_error("halfwidth quotes must stay ASCII");
    if (!wetype::isQuote('"') || !wetype::isQuote('\'') || wetype::isQuote('`'))
        throw std::runtime_error("isQuote must name exactly the two pairable keys");
}

// Whole-set property: every printable ASCII symbol is owned except the grave
// key, so a new symbol on the keyboard cannot silently fall back to halfwidth.
void expectWholeSymbolSet() {
    int owned = 0;
    for (unsigned char c = 0x21; c <= 0x7E; ++c) {
        if (std::isalnum(c)) continue;
        const char key = static_cast<char>(c);
        if (key == '`') {
            if (wetype::ownsPunctuation(key))
                throw std::runtime_error("the grave key must stay a backtick");
            continue;
        }
        if (!wetype::ownsPunctuation(key))
            throw std::runtime_error(std::string("symbol missing from the table: ") + key);
        const auto full = wetype::punctuation(key, true);
        if (full.empty() || static_cast<unsigned char>(full[0]) < 0x80)
            throw std::runtime_error(std::string("not a fullwidth form: ") + key);
        ++owned;
    }
    if (owned != 31) throw std::runtime_error("expected 31 owned symbols, got " + std::to_string(owned));
    // Letters and digits are never punctuation.
    if (wetype::ownsPunctuation('a') || wetype::ownsPunctuation('7') ||
        wetype::ownsPunctuation(' ') || wetype::ownsPunctuation('\n'))
        throw std::runtime_error("alphanumeric keys must stay out of the table");
}

void expectDigitExceptionScope() {
    if (!wetype::digitException(',') || !wetype::digitException('.'))
        throw std::runtime_error("decimal point and thousands separator must keep the exception");
    for (const char key : {'?', '!', ':', ';', '(', ')', '[', '<', '"', '\\', '-', '/'}) {
        if (wetype::digitException(key))
            throw std::runtime_error(std::string("digit exception leaked onto ") + key);
    }
}

void expectDigitDetection() {
    const std::pair<std::string_view, bool> cases[] = {
        {"", false},       {"123", true},     {"3.14", true},  {"总分 3", true},
        {"3 \t\r\n", true}, {"你好", false},   {"你好3", true}, {"hello", false},
        {"，。", false},    {"300米", false},  {"a", false},
    };
    for (const auto &[text, expected] : cases) {
        if (wetype::endsWithDigit(text) != expected)
            throw std::runtime_error("wrong digit classification");
    }
    // Invalid UTF-8 must not be read as a digit: continuation bytes are
    // 0x80-0xBF, so a CJK character can never end in an ASCII digit byte.
    if (wetype::endsWithDigit(std::string_view("\xE4\xBD", 2)))
        throw std::runtime_error("truncated UTF-8 read as a digit");
    if (wetype::endsWithDigit(std::string_view("中\xF0\x9F", 4)))
        throw std::runtime_error("invalid trailing sequence read as a digit");
    if (!wetype::endsWithDigit("中" "7"))
        throw std::runtime_error("digit after a CJK character should count");
}

}  // namespace

int main() {
    try {
        expectNamedForms();
        expectQuotePairing();
        expectWholeSymbolSet();
        expectDigitExceptionScope();
        expectDigitDetection();
        std::cout << "PASS punctuation: full symbol table, quote pairing, digit scope\n";
    } catch (const std::exception &error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
    return 0;
}
