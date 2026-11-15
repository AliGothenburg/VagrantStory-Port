#pragma once

// RetroAchievements (retroachievements.org) through the official rcheevos rc_client: login with a
// stored token, disc identification, memory checks once per real vblank, unlock notices.
// Softcore only (a new client cannot be hardcore-approved). The set exists for the US disc
// (SLUS-01040); other discs are reported as "no achievements".

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>

namespace vsport {
class PsxRuntime;
}

namespace ra {

// Settings window: logs in with username + password and returns the token to keep (the password
// itself is never stored). Blocks until the server answers (a few seconds at most).
bool login_with_password(const std::string& user, const std::string& password, std::string& token_out,
                         std::string& display_name_out, std::string& error_out);

class Achievements {
public:
    using Notify = std::function<void(const std::string&)>; // Message for the player (window title)
    using Log    = std::function<void(const std::string&)>; // VagrantStory.log

    Achievements();
    ~Achievements();
    Achievements(const Achievements&)            = delete;
    Achievements& operator=(const Achievements&) = delete;

    // Starts logging in with the token and then loading the game of the disc (asynchronous)
    void start(vsport::PsxRuntime& rt, const std::string& user, const std::string& token,
               const std::filesystem::path& disc, Notify notify, Log log);
    void do_frame(); // Once per real (not run-ahead) vblank
    void pump();     // Every vblank: finished server requests are handled here (main thread)
    void reset();    // After a save state or rewind was loaded
    void shutdown();
    [[nodiscard]] bool active() const noexcept;

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

} // namespace ra
