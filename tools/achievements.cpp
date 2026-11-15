// RetroAchievements through rcheevos rc_client (see achievements.hpp). Server requests run on
// short-lived worker threads (WinHTTP); their answers are handed back to rc_client on the main
// thread in pump(), so rc_client is only ever used from one thread.
#include "achievements.hpp"

#include "vsport/runtime.hpp"

#include "rc_api_user.h"
#include "rc_client.h"
#include "rc_consoles.h"
#include "rc_hash.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <deque>
#include <mutex>
#include <thread>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")
#endif

namespace ra {

namespace {

constexpr const char* CLIENT_NAME = "VagrantStoryPort/0.2";

#ifdef _WIN32
std::wstring widen_utf8(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}
#endif

struct HttpResult {
    int status = 0; // HTTP status, or an RC_API_SERVER_RESPONSE_* client error
    std::string body;
};

// One HTTP request, blocking (worker thread or the settings window)
HttpResult http_request(const std::string& url, const char* post_data, const std::string& content_type, const std::string& user_agent) {
    HttpResult res;
    res.status = RC_API_SERVER_RESPONSE_RETRYABLE_CLIENT_ERROR; // Until the server has answered
#ifdef _WIN32
    const std::wstring wurl = widen_utf8(url);
    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof(uc);
    wchar_t host[256] = {}, path[2048] = {};
    uc.lpszHostName = host;
    uc.dwHostNameLength = 255;
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = 2047;
    uc.dwExtraInfoLength = 1; // Query string stays in the path below
    if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &uc)) {
        res.status = RC_API_SERVER_RESPONSE_CLIENT_ERROR;
        return res;
    }
    std::wstring object(uc.lpszUrlPath, uc.dwUrlPathLength);
    if (uc.lpszExtraInfo && uc.dwExtraInfoLength) object.append(uc.lpszExtraInfo, uc.dwExtraInfoLength);
    const bool https = uc.nScheme == INTERNET_SCHEME_HTTPS;

    HINTERNET session = WinHttpOpen(widen_utf8(user_agent).c_str(), WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                                    WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) return res;
    WinHttpSetTimeouts(session, 10000, 10000, 20000, 20000);
    HINTERNET connect = WinHttpConnect(session, std::wstring(uc.lpszHostName, uc.dwHostNameLength).c_str(), uc.nPort, 0);
    HINTERNET request = connect ? WinHttpOpenRequest(connect, post_data ? L"POST" : L"GET", object.c_str(), nullptr, WINHTTP_NO_REFERER,
                                                     WINHTTP_DEFAULT_ACCEPT_TYPES, https ? WINHTTP_FLAG_SECURE : 0)
                                : nullptr;
    if (request) {
        const std::wstring headers = post_data ? L"Content-Type: " + widen_utf8(content_type) + L"\r\n" : std::wstring();
        const DWORD len = post_data ? static_cast<DWORD>(std::strlen(post_data)) : 0u;
        if (WinHttpSendRequest(request, headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(), static_cast<DWORD>(-1L),
                               const_cast<char*>(post_data ? post_data : ""), len, len, 0) &&
            WinHttpReceiveResponse(request, nullptr)) {
            DWORD code = 0, size = sizeof(code);
            WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &code, &size,
                                WINHTTP_NO_HEADER_INDEX);
            for (;;) {
                DWORD avail = 0;
                if (!WinHttpQueryDataAvailable(request, &avail) || avail == 0) break;
                const std::size_t at = res.body.size();
                res.body.resize(at + avail);
                DWORD got = 0;
                if (!WinHttpReadData(request, res.body.data() + at, avail, &got)) break;
                res.body.resize(at + got);
            }
            res.status = static_cast<int>(code);
        }
        WinHttpCloseHandle(request);
    }
    if (connect) WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);
#else
    (void)url; (void)post_data; (void)content_type; (void)user_agent;
    res.status = RC_API_SERVER_RESPONSE_CLIENT_ERROR;
#endif
    return res;
}

// Disc reading for the hash with UTF-8 paths (the default reader uses fopen)
void* RC_CCONV fr_open(const char* path_utf8) {
#ifdef _WIN32
    return _wfopen(widen_utf8(path_utf8).c_str(), L"rb");
#else
    return std::fopen(path_utf8, "rb");
#endif
}
void RC_CCONV fr_seek(void* f, int64_t offset, int origin) { _fseeki64(static_cast<FILE*>(f), offset, origin); }
int64_t RC_CCONV fr_tell(void* f) { return _ftelli64(static_cast<FILE*>(f)); }
size_t RC_CCONV fr_read(void* f, void* buffer, size_t n) { return std::fread(buffer, 1, n, static_cast<FILE*>(f)); }
void RC_CCONV fr_close(void* f) { std::fclose(static_cast<FILE*>(f)); }

// Answers of finished requests, shared with the worker threads (they may outlive a shutdown)
struct Shared {
    struct Done {
        rc_client_server_callback_t callback;
        void* data;
        HttpResult result;
    };
    std::mutex m;
    std::deque<Done> done;
    std::atomic<int> in_flight{0};
};

} // namespace

bool login_with_password(const std::string& user, const std::string& password, std::string& token_out,
                         std::string& display_name_out, std::string& error_out) {
    rc_api_login_request_t params{};
    params.username = user.c_str();
    params.password = password.c_str();
    rc_api_request_t request{};
    if (rc_api_init_login_request(&request, &params) != RC_OK) {
        error_out = "Giriş isteği hazırlanamadı";
        return false;
    }
    const HttpResult r = http_request(request.url, request.post_data,
                                      request.content_type ? request.content_type : "application/x-www-form-urlencoded",
                                      std::string(CLIENT_NAME) + " rcheevos/12.5");
    rc_api_destroy_request(&request);
    if (r.status < 0) {
        error_out = "Sunucuya bağlanılamadı (internet bağlantısını kontrol edin)";
        return false;
    }
    rc_api_server_response_t server{};
    server.body = r.body.c_str();
    server.body_length = r.body.size();
    server.http_status_code = r.status;
    rc_api_login_response_t resp{};
    const int rc = rc_api_process_login_server_response(&resp, &server);
    const bool ok = rc == RC_OK && resp.response.succeeded && resp.api_token;
    if (ok) {
        token_out = resp.api_token;
        display_name_out = resp.display_name ? resp.display_name : (resp.username ? resp.username : user);
    } else {
        error_out = resp.response.error_message ? resp.response.error_message : "Giriş başarısız";
    }
    rc_api_destroy_login_response(&resp);
    return ok;
}

struct Achievements::Impl {
    rc_client_t* client = nullptr;
    vsport::PsxRuntime* rt = nullptr;
    std::string user, token, disc_utf8, user_agent;
    Notify notify;
    Log log;
    std::shared_ptr<Shared> shared = std::make_shared<Shared>();

    static Impl* of(const rc_client_t* c) { return static_cast<Impl*>(rc_client_get_userdata(c)); }

    // PS1 memory as rcheevos addresses it: 0..1FFFFF main RAM, 200000..2003FF scratchpad
    static uint32_t RC_CCONV read_memory(uint32_t address, uint8_t* buffer, uint32_t num_bytes, rc_client_t* c) {
        Impl* im = of(c);
        if (!im || !im->rt) return 0;
        const auto ram = im->rt->ram_span();
        for (uint32_t i = 0; i < num_bytes; ++i) {
            const uint32_t a = address + i;
            if (a < 0x200000u) {
                if (a >= ram.size()) return i;
                buffer[i] = ram[a];
            } else if (a < 0x200400u) {
                buffer[i] = im->rt->read8(0x1F800000u + (a - 0x200000u));
            } else {
                return i;
            }
        }
        return num_bytes;
    }

    static void RC_CCONV server_call(const rc_api_request_t* request, rc_client_server_callback_t callback, void* data, rc_client_t* c) {
        Impl* im = of(c);
        std::shared_ptr<Shared> sh = im->shared;
        const std::string url = request->url ? request->url : "";
        const bool has_post = request->post_data != nullptr;
        const std::string post = has_post ? request->post_data : "";
        const std::string type = request->content_type ? request->content_type : "application/x-www-form-urlencoded";
        const std::string ua = im->user_agent;
        ++sh->in_flight;
        std::thread([sh, url, has_post, post, type, ua, callback, data] {
            HttpResult r = http_request(url, has_post ? post.c_str() : nullptr, type, ua);
            {
                std::lock_guard<std::mutex> lk(sh->m);
                sh->done.push_back({callback, data, std::move(r)});
            }
            --sh->in_flight;
        }).detach();
    }

    static void RC_CCONV log_message(const char* message, const rc_client_t* c) {
        if (Impl* im = of(c); im && im->log) im->log(std::string("RetroAchievements: ") + message);
    }

    static void RC_CCONV event_handler(const rc_client_event_t* e, rc_client_t* c) {
        Impl* im = of(c);
        if (!im) return;
        switch (e->type) {
            case RC_CLIENT_EVENT_ACHIEVEMENT_TRIGGERED:
                if (e->achievement) {
                    const std::string msg = "Başarım açıldı: " + std::string(e->achievement->title) + " (" +
                                            std::to_string(e->achievement->points) + " puan)";
                    if (im->notify) im->notify(msg);
                    if (im->log) im->log("RetroAchievements: " + msg + " - " + (e->achievement->description ? e->achievement->description : ""));
                }
                break;
            case RC_CLIENT_EVENT_GAME_COMPLETED:
                if (im->notify) im->notify("Tüm başarımlar tamamlandı!");
                break;
            case RC_CLIENT_EVENT_LEADERBOARD_SUBMITTED:
                if (e->leaderboard && im->log) im->log(std::string("RetroAchievements: skor gönderildi: ") + e->leaderboard->title);
                break;
            case RC_CLIENT_EVENT_SERVER_ERROR:
                if (e->server_error && im->log) {
                    im->log(std::string("RetroAchievements: sunucu hatasi (") + (e->server_error->api ? e->server_error->api : "?") + "): " +
                            (e->server_error->error_message ? e->server_error->error_message : ""));
                }
                break;
            default:
                break;
        }
    }

    static void RC_CCONV load_done(int result, const char* error, rc_client_t* c, void*) {
        Impl* im = of(c);
        if (!im) return;
        if (result == RC_OK) {
            const rc_client_game_t* g = rc_client_get_game_info(c);
            rc_client_user_game_summary_t sum{};
            rc_client_get_user_game_summary(c, &sum);
            if (im->notify) {
                im->notify("RetroAchievements: " + std::string(g && g->title ? g->title : "oyun") + " - " +
                           std::to_string(sum.num_unlocked_achievements) + "/" + std::to_string(sum.num_core_achievements) +
                           " başarım (softcore)");
            }
        } else if (result == RC_NO_GAME_LOADED) {
            if (im->notify) im->notify("RetroAchievements: bu disk sürümü için başarım seti yok (yalnızca ABD sürümü, SLUS-01040)");
        } else if (im->notify) {
            im->notify(std::string("RetroAchievements: oyun yüklenemedi (") + (error ? error : "?") + ")");
        }
    }

    static void RC_CCONV login_done(int result, const char* error, rc_client_t* c, void*) {
        Impl* im = of(c);
        if (!im) return;
        if (result != RC_OK) {
            if (im->notify) im->notify(std::string("RetroAchievements: giriş başarısız (") + (error ? error : "?") + ")");
            return;
        }
        rc_client_begin_identify_and_load_game(c, RC_CONSOLE_PLAYSTATION, im->disc_utf8.c_str(), nullptr, 0, &Impl::load_done, nullptr);
    }
};

Achievements::Achievements() : impl_(std::make_unique<Impl>()) {}
Achievements::~Achievements() { shutdown(); }

bool Achievements::active() const noexcept { return impl_ && impl_->client; }

void Achievements::start(vsport::PsxRuntime& rt, const std::string& user, const std::string& token,
                         const std::filesystem::path& disc, Notify notify, Log log) {
    shutdown();
    Impl& im = *impl_;
    im.rt = &rt;
    im.user = user;
    im.token = token;
    const auto u8 = disc.u8string();
    im.disc_utf8.assign(u8.begin(), u8.end());
    im.notify = std::move(notify);
    im.log = std::move(log);
    im.shared = std::make_shared<Shared>();

    static rc_hash_filereader_t reader{&fr_open, &fr_seek, &fr_tell, &fr_read, &fr_close};
    rc_hash_init_custom_filereader(&reader);
    rc_hash_init_default_cdreader();

    im.client = rc_client_create(&Impl::read_memory, &Impl::server_call);
    if (!im.client) return;
    rc_client_set_userdata(im.client, &im);
    rc_client_enable_logging(im.client, RC_CLIENT_LOG_LEVEL_INFO, &Impl::log_message);
    rc_client_set_event_handler(im.client, &Impl::event_handler);
    rc_client_set_hardcore_enabled(im.client, 0); // Softcore: save states, rewind and cheats stay usable
    char clause[64] = {};
    rc_client_get_user_agent_clause(im.client, clause, sizeof(clause));
    im.user_agent = std::string(CLIENT_NAME) + " " + clause;
    rc_client_begin_login_with_token(im.client, user.c_str(), token.c_str(), &Impl::login_done, nullptr);
}

void Achievements::pump() {
    if (!active()) return;
    std::deque<Shared::Done> ready;
    {
        std::lock_guard<std::mutex> lk(impl_->shared->m);
        ready.swap(impl_->shared->done);
    }
    for (Shared::Done& d : ready) {
        rc_api_server_response_t resp{};
        resp.body = d.result.body.c_str();
        resp.body_length = d.result.body.size();
        resp.http_status_code = d.result.status;
        d.callback(&resp, d.data);
    }
}

void Achievements::do_frame() {
    if (active()) rc_client_do_frame(impl_->client);
}

void Achievements::reset() {
    if (active()) rc_client_reset(impl_->client);
}

void Achievements::shutdown() {
    if (!active()) return;
    // Let requests already sent (an unlock just earned) finish, for a few seconds at most
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(4);
    while (impl_->shared->in_flight > 0 && std::chrono::steady_clock::now() < until) {
        pump();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    pump();
    rc_client_destroy(impl_->client);
    impl_->client = nullptr;
}

} // namespace ra
