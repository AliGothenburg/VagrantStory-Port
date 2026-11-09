#include "vsport/win32_input.hpp"
#include "vsport/pad_mapping.hpp"

#ifdef _WIN32

#define DIRECTINPUT_VERSION 0x0800
#include <windows.h>
#include <Xinput.h>
#include <dinput.h>

#include <array>
#include <cstdio>
#include <cwchar>
#include <set>
#include <vector>

namespace vsport {

namespace {

constexpr ULONGLONG PROBE_INTERVAL_MS = 2000; // Re-scan for newly plugged controllers

std::string narrow(const wchar_t* w) {
    if (!w || !*w) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(n > 0 ? n - 1 : 0), '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w, -1, out.data(), n, nullptr, nullptr);
    return out;
}

// XInput pads also show up in DirectInput. Their raw-input device paths contain "IG_";
// collect their VID/PID pairs (as DirectInput's guidProduct.Data1 = MAKELONG(VID, PID)) so the
// DirectInput backend can skip them and a pad is never read twice.
std::set<DWORD> collect_xinput_vid_pids() {
    std::set<DWORD> ids;
    UINT count = 0;
    if (GetRawInputDeviceList(nullptr, &count, sizeof(RAWINPUTDEVICELIST)) != 0 || count == 0) {
        return ids;
    }
    std::vector<RAWINPUTDEVICELIST> list(count);
    if (GetRawInputDeviceList(list.data(), &count, sizeof(RAWINPUTDEVICELIST)) == static_cast<UINT>(-1)) {
        return ids;
    }
    for (UINT i = 0; i < count; ++i) {
        if (list[i].dwType != RIM_TYPEHID) continue;
        wchar_t name[512]{};
        UINT len = static_cast<UINT>(std::size(name));
        if (GetRawInputDeviceInfoW(list[i].hDevice, RIDI_DEVICENAME, name, &len) == static_cast<UINT>(-1)) continue;
        if (!std::wcsstr(name, L"IG_")) continue;
        const wchar_t* v = std::wcsstr(name, L"VID_");
        const wchar_t* p = std::wcsstr(name, L"PID_");
        if (!v || !p) continue;
        const DWORD vid = static_cast<DWORD>(std::wcstoul(v + 4, nullptr, 16));
        const DWORD pid = static_cast<DWORD>(std::wcstoul(p + 4, nullptr, 16));
        ids.insert(static_cast<DWORD>(MAKELONG(vid, pid)));
    }
    return ids;
}

} // namespace

struct Win32GamepadInput::Impl {
    HWND hwnd = nullptr;

    // XInput
    std::array<bool, XUSER_MAX_COUNT> xi_connected{};
    std::array<ULONGLONG, XUSER_MAX_COUNT> xi_next_probe{};

    // DirectInput (first non-XInput game controller)
    IDirectInput8W* di = nullptr;
    IDirectInputDevice8W* di_dev = nullptr;
    std::string di_name;
    ULONGLONG di_next_probe = 0;

    struct EnumCtx {
        std::set<DWORD> xinput_ids;
        bool found = false;
        GUID guid{};
        std::wstring name;
    };

    static BOOL CALLBACK enum_cb(LPCDIDEVICEINSTANCEW inst, LPVOID user) {
        auto* ctx = static_cast<EnumCtx*>(user);
        if (ctx->xinput_ids.contains(inst->guidProduct.Data1)) {
            return DIENUM_CONTINUE; // Handled by XInput
        }
        ctx->found = true;
        ctx->guid  = inst->guidInstance;
        ctx->name  = inst->tszProductName;
        return DIENUM_STOP;
    }

    void release_dinput_device() {
        if (di_dev) {
            di_dev->Unacquire();
            di_dev->Release();
            di_dev = nullptr;
            std::printf("[vs_host] Gamepad ayrildi: DirectInput (%s)\n", di_name.c_str());
            std::fflush(stdout);
        }
    }

    void try_open_dinput_device() {
        EnumCtx ctx{};
        ctx.xinput_ids = collect_xinput_vid_pids();
        if (FAILED(di->EnumDevices(DI8DEVCLASS_GAMECTRL, &Impl::enum_cb, &ctx, DIEDFL_ATTACHEDONLY)) || !ctx.found) {
            return;
        }
        IDirectInputDevice8W* dev = nullptr;
        if (FAILED(di->CreateDevice(ctx.guid, &dev, nullptr))) return;
        if (FAILED(dev->SetDataFormat(&c_dfDIJoystick2)) ||
            FAILED(dev->SetCooperativeLevel(hwnd, DISCL_BACKGROUND | DISCL_NONEXCLUSIVE))) {
            dev->Release();
            return;
        }
        // Normalize every axis to [-1000, 1000]
        DIPROPRANGE range{};
        range.diph.dwSize       = sizeof(DIPROPRANGE);
        range.diph.dwHeaderSize = sizeof(DIPROPHEADER);
        range.diph.dwHow        = DIPH_DEVICE;
        range.lMin              = -1000;
        range.lMax              = 1000;
        dev->SetProperty(DIPROP_RANGE, &range.diph);
        dev->Acquire();

        di_dev  = dev;
        di_name = narrow(ctx.name.c_str());
        std::printf("[vs_host] Gamepad baglandi: DirectInput (%s)\n", di_name.c_str());
        std::fflush(stdout);
    }

    u16 poll_dinput(ULONGLONG now) {
        if (!di) return 0;
        if (!di_dev) {
            if (now >= di_next_probe) {
                di_next_probe = now + PROBE_INTERVAL_MS;
                try_open_dinput_device();
            }
            if (!di_dev) return 0;
        }

        di_dev->Poll();
        DIJOYSTATE2 js{};
        HRESULT hr = di_dev->GetDeviceState(sizeof(js), &js);
        if (hr == DIERR_INPUTLOST || hr == DIERR_NOTACQUIRED) {
            di_dev->Acquire();
            hr = di_dev->GetDeviceState(sizeof(js), &js);
        }
        if (FAILED(hr)) {
            release_dinput_device(); // Unplugged: rescan later
            di_next_probe = now + PROBE_INTERVAL_MS;
            return 0;
        }
        return pad::map_dinput(static_cast<u32>(js.rgdwPOV[0]), js.lX, js.lY,
                               js.rgbButtons, sizeof(js.rgbButtons));
    }

    u16 poll_xinput(ULONGLONG now) {
        u16 pressed = 0;
        for (DWORD slot = 0; slot < XUSER_MAX_COUNT; ++slot) {
            // XInputGetState on an empty slot is slow; only re-probe empty slots periodically
            if (!xi_connected[slot] && now < xi_next_probe[slot]) continue;

            XINPUT_STATE st{};
            if (XInputGetState(slot, &st) == ERROR_SUCCESS) {
                if (!xi_connected[slot]) {
                    xi_connected[slot] = true;
                    std::printf("[vs_host] Gamepad baglandi: XInput #%lu\n", slot + 1);
                    std::fflush(stdout);
                }
                const XINPUT_GAMEPAD& g = st.Gamepad;
                pressed |= pad::map_xinput(g.wButtons, g.bLeftTrigger, g.bRightTrigger, g.sThumbLX, g.sThumbLY);
            } else {
                if (xi_connected[slot]) {
                    xi_connected[slot] = false;
                    std::printf("[vs_host] Gamepad ayrildi: XInput #%lu\n", slot + 1);
                    std::fflush(stdout);
                }
                xi_next_probe[slot] = now + PROBE_INTERVAL_MS;
            }
        }
        return pressed;
    }
};

Win32GamepadInput::Win32GamepadInput() : impl_(std::make_unique<Impl>()) {}

Win32GamepadInput::~Win32GamepadInput() {
    shutdown();
}

bool Win32GamepadInput::initialize(void* hwnd) {
    impl_->hwnd = static_cast<HWND>(hwnd);
    const HRESULT hr = DirectInput8Create(GetModuleHandleW(nullptr), DIRECTINPUT_VERSION, IID_IDirectInput8W,
                                          reinterpret_cast<void**>(&impl_->di), nullptr);
    if (FAILED(hr)) {
        impl_->di = nullptr; // XInput still works
    }
    return true;
}

void Win32GamepadInput::shutdown() {
    if (!impl_) return;
    if (impl_->di_dev) {
        impl_->di_dev->Unacquire();
        impl_->di_dev->Release();
        impl_->di_dev = nullptr;
    }
    if (impl_->di) {
        impl_->di->Release();
        impl_->di = nullptr;
    }
}

u16 Win32GamepadInput::poll() {
    const ULONGLONG now = GetTickCount64();
    return static_cast<u16>(impl_->poll_xinput(now) | impl_->poll_dinput(now));
}

} // namespace vsport

#else // !_WIN32

namespace vsport {
struct Win32GamepadInput::Impl {};
Win32GamepadInput::Win32GamepadInput() : impl_(std::make_unique<Impl>()) {}
Win32GamepadInput::~Win32GamepadInput() = default;
bool Win32GamepadInput::initialize(void*) { return false; }
void Win32GamepadInput::shutdown() {}
u16 Win32GamepadInput::poll() { return 0; }
} // namespace vsport

#endif
