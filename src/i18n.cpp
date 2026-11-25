#include "vsport/i18n.hpp"

#include <atomic>
#include <cstdlib>
#include <cstring>

#ifdef _WIN32
#include <windows.h>
#endif

namespace vsport {

namespace {
std::atomic<Language> g_language{Language::English};
} // namespace

void set_language(Language lang) noexcept { g_language.store(lang, std::memory_order_relaxed); }

Language language() noexcept { return g_language.load(std::memory_order_relaxed); }

Language system_language() noexcept {
#ifdef _WIN32
    return PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_TURKISH ? Language::Turkish : Language::English;
#else
    // POSIX locale variables, most specific first (Android hosts pass the device language in LANG)
    for (const char* var : {"LC_ALL", "LC_MESSAGES", "LANG"}) {
        const char* v = std::getenv(var);
        if (v && *v) return std::strncmp(v, "tr", 2) == 0 ? Language::Turkish : Language::English;
    }
    return Language::English;
#endif
}

Language language_from_setting(int setting) noexcept {
    switch (setting) {
        case 1: return Language::English;
        case 2: return Language::Turkish;
        default: return system_language();
    }
}

} // namespace vsport
