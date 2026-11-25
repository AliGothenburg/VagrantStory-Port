#pragma once

// Language of the texts the player sees (settings window, notices, message boxes, on-screen menus).
// Every such text is written at its call site in both languages: txt("English", "Türkçe"), so the
// two versions stay next to each other. Logs, console output and the ini comments are English only.

#include "vsport/types.hpp"

namespace vsport {

enum class Language : u8 { English, Turkish };

void set_language(Language lang) noexcept;
[[nodiscard]] Language language() noexcept;

// The system's interface language: Turkish when it is Turkish, otherwise English
[[nodiscard]] Language system_language() noexcept;

// "language" setting: 0 = follow the system, 1 = English, 2 = Turkish
[[nodiscard]] Language language_from_setting(int setting) noexcept;

[[nodiscard]] inline const char* txt(const char* en, const char* tr) noexcept {
    return language() == Language::Turkish ? tr : en;
}
[[nodiscard]] inline const wchar_t* txt(const wchar_t* en, const wchar_t* tr) noexcept {
    return language() == Language::Turkish ? tr : en;
}

} // namespace vsport
