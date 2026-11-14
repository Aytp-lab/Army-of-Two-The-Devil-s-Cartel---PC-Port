// Verify SDK event-to-guest-state mapping; this does not verify gameplay aiming.
#include <rex/cvar.h>
#include <rex/input/mnk/mnk_input_driver.h>
#include <cstdio>
#include <windows.h>
#include <cstring>

int main(int argc, char** argv) {
    using namespace rex::input;
    using rex::ui::MouseEvent;
    if (!rex::cvar::SetFlagByName("mnk_mode", "true") ||
        !rex::cvar::SetFlagByName("mnk_mouse", "true") ||
        !rex::cvar::SetFlagByName("keybind_right_trigger", "LMB") ||
        !rex::cvar::SetFlagByName("keybind_left_trigger", "RMB")) return 1;
    mnk::MnkInputDriver driver(nullptr, 0);
    if (driver.Setup() != 0) return 2;
    std::vector<DeviceInfo> devices;
    driver.EnumerateDevices(devices);
    if (devices.size() != 1) return 3;
    X_INPUT_STATE state = {};
    auto read = [&] { return driver.GetDeviceState(devices[0].id, &state) == 0; };
    rex::ui::KeyEvent forward(nullptr, rex::ui::VirtualKey::kW, 1, false,
        false, false, false, false);
    driver.OnKeyDown(forward);
    if (!read() || int16_t(state.gamepad.thumb_ly) != 32767) return 4;
    driver.OnKeyUp(forward);
    if (!read() || int16_t(state.gamepad.thumb_ly) != 0) return 5;
    MouseEvent fire(nullptr, MouseEvent::Button::kLeft, 0, 0);
    driver.OnMouseDown(fire);
    if (!read() || state.gamepad.right_trigger != 255) return 6;
    driver.OnMouseUp(fire);
    if (!read() || state.gamepad.right_trigger != 0) return 7;
    MouseEvent aim(nullptr, MouseEvent::Button::kRight, 0, 0);
    driver.OnMouseDown(aim);
    if (!read() || state.gamepad.left_trigger != 255) return 8;
    driver.OnMouseUp(aim);
    // Exercise the user's modifier-held movement/fire regression through the
    // actual SDK event-to-controller path, including standalone modifier binds.
    if (!rex::cvar::SetFlagByName("keybind_lstick_press", "Shift") ||
        !rex::cvar::SetFlagByName("keybind_rstick_press", "Ctrl") ||
        !rex::cvar::SetFlagByName("keybind_b", "Alt") ||
        !rex::cvar::SetFlagByName("keybind_x", "Ctrl+R")) return 12;
    const rex::ui::VirtualKey modifier_keys[] = {
        rex::ui::VirtualKey::kShift, rex::ui::VirtualKey::kControl,
        rex::ui::VirtualKey::kMenu};
    for (unsigned mask = 0; mask < 8; ++mask) {
        for (unsigned i = 0; i < 3; ++i) {
            rex::ui::KeyEvent event(nullptr, modifier_keys[i], 1, false,
                false, false, false, false);
            if (mask & (1u << i)) driver.OnKeyDown(event);
        }
        driver.OnKeyDown(forward);
        driver.OnMouseDown(fire);
        rex::ui::KeyEvent reload(nullptr, rex::ui::VirtualKey::kR, 1, false,
            false, false, false, false);
        driver.OnKeyDown(reload);
        if (!read() || int16_t(state.gamepad.thumb_ly) != 32767 ||
            state.gamepad.right_trigger != 255 ||
            bool(state.gamepad.buttons & 0x0040) != bool(mask & 1) ||
            bool(state.gamepad.buttons & 0x0080) != bool(mask & 2) ||
            bool(state.gamepad.buttons & 0x2000) != bool(mask & 4) ||
            bool(state.gamepad.buttons & 0x4000) != (mask == 2)) {
            std::printf("FAIL: modifier combination %u\n", mask);
            return 13;
        }
        driver.OnKeyUp(reload);
        driver.OnKeyUp(forward);
        driver.OnMouseUp(fire);
        for (auto key : modifier_keys) {
            rex::ui::KeyEvent event(nullptr, key, 1, false,
                false, false, false, false);
            driver.OnKeyUp(event);
        }
    }
    MouseEvent motion(nullptr, MouseEvent::Button::kNone, 4, -2);
    driver.OnMouseMove(motion);
    if (!read() || int16_t(state.gamepad.thumb_rx) <= 0 ||
        int16_t(state.gamepad.thumb_ry) <= 0) return 9;
    if (!read() || int16_t(state.gamepad.thumb_rx) != 0 ||
        int16_t(state.gamepad.thumb_ry) != 0) return 10;
    driver.OnKeyDown(forward);
    driver.OnMouseDown(fire);
    rex::ui::UISetupEvent focus(nullptr);
    driver.OnLostFocus(focus);
    driver.OnGotFocus(focus);
    if (!read() || int16_t(state.gamepad.thumb_ly) != 0 ||
        state.gamepad.right_trigger != 0) return 11;
    if (argc > 1 && std::strcmp(argv[1], "--direct-mouse") == 0) {
        using Consume = int (*)(uint32_t, float*, float*);
        auto consume = reinterpret_cast<Consume>(GetProcAddress(
            GetModuleHandleW(L"rexruntime.dll"), "rex_ao4_mouse_consume_v1"));
        if (!consume) return 20;
        float dx = 0, dy = 0;
        if (!consume(1, &dx, &dy) || dx != 0 || dy != 0) return 21;
        rex::ui::KeyEvent stick(nullptr, rex::ui::VirtualKey::kRight, 1, false,
            false, false, false, false);
        driver.OnKeyDown(stick);
        driver.OnKeyDown(forward);
        driver.OnMouseDown(fire);
        MouseEvent raw(nullptr, MouseEvent::Button::kNone, 8, -4);
        driver.OnMouseMove(raw); // Previous position was (4,-2): delta (4,-2).
        if (!read() || int16_t(state.gamepad.thumb_rx) != 32767 ||
            int16_t(state.gamepad.thumb_ry) != 0 ||
            int16_t(state.gamepad.thumb_ly) != 32767 ||
            state.gamepad.right_trigger != 255) return 22;
        if (!consume(1, &dx, &dy) || dx != 4 || dy != -2) return 23;
        if (!consume(1, &dx, &dy) || dx != 0 || dy != 0) return 24;
        // Explicitly release the lease: later events take the original route.
        if (consume(0, &dx, &dy)) return 28;
        driver.OnKeyUp(stick);
        driver.OnMouseMove(motion); // (8,-4) -> (4,-2) = (-4,+2).
        if (!read() || int16_t(state.gamepad.thumb_rx) != -800 ||
            int16_t(state.gamepad.thumb_ry) != -400) return 29;
        if (!consume(1, &dx, &dy) || dx != 0 || dy != 0) return 30;
        driver.OnMouseMove(motion);
        driver.OnLostFocus(focus);
        if (consume(1, &dx, &dy) || dx != 0 || dy != 0) return 25;
        driver.OnGotFocus(focus);
        if (!read() || int16_t(state.gamepad.thumb_rx) != 0 ||
            int16_t(state.gamepad.thumb_ly) != 0 || state.gamepad.right_trigger != 0) return 26;
        mnk::SetMouseLookActive(false);
        if (consume(1, &dx, &dy)) return 27;
        mnk::SetMouseLookActive(true);
        std::puts("PASS: actual SDK raw mouse bridge, no duplicate stick motion, keyboard stick/buttons retained, focus/UI reset");
        if (argc > 2 && std::strcmp(argv[2], "--turret-pulse") == 0) {
            using Pulse = void (*)(float, float);
            auto pulse = reinterpret_cast<Pulse>(GetProcAddress(
                GetModuleHandleW(L"rexruntime.dll"), "rex_ao4_stick_pulse_v1"));
            using Scope = uint32_t (*)(uint32_t);
            auto scope = reinterpret_cast<Scope>(GetProcAddress(
                GetModuleHandleW(L"rexruntime.dll"), "rex_ao4_stick_poll_scope_v1"));
            if (!pulse || !scope || !read()) return 40;
            const uint32_t original_scope = scope(0x83228B48);
            driver.OnKeyDown(forward);
            driver.OnMouseDown(fire);
            pulse(.125f, -.25f);
            pulse(.125f, -.25f);
            // Connection-only and unscoped probes must leave the pulse queued.
            if (!read() || int16_t(state.gamepad.thumb_rx) != 0 ||
                int16_t(state.gamepad.thumb_ry) != 0) return 47;
            if (scope(0) != 0x83228B48) return 48;
            if (!read() || int16_t(state.gamepad.thumb_rx) != 0 ||
                int16_t(state.gamepad.thumb_ry) != 0) return 49;
            if (scope(0x82F86ABC) != 0) return 50;
            if (!read() || int16_t(state.gamepad.thumb_rx) != 8191 ||
                int16_t(state.gamepad.thumb_ry) != -16383 ||
                int16_t(state.gamepad.thumb_ly) != 32767 || state.gamepad.right_trigger != 255) return 41;
            if (!read() || int16_t(state.gamepad.thumb_rx) != 0 ||
                int16_t(state.gamepad.thumb_ry) != 0) return 42;
            pulse(1, -1);
            if (!read() || int16_t(state.gamepad.thumb_rx) != 32767 ||
                int16_t(state.gamepad.thumb_ry) != -32767) return 43;
            pulse(1, .25f);pulse(1, .25f);
            if (!read() || int16_t(state.gamepad.thumb_rx) != 32767 ||
                int16_t(state.gamepad.thumb_ry) != 8191) return 51;
            pulse(.25f, .25f);
            pulse(0, 0);
            if (!read() || int16_t(state.gamepad.thumb_rx) != 0 ||
                int16_t(state.gamepad.thumb_ry) != 0) return 44;
            pulse(.25f, .25f);
            driver.OnLostFocus(focus);
            driver.OnGotFocus(focus);
            if (!read() || int16_t(state.gamepad.thumb_rx) != 0 ||
                int16_t(state.gamepad.thumb_ry) != 0) return 45;
            pulse(.25f, .25f);
            mnk::SetMouseLookActive(false);
            mnk::SetMouseLookActive(true);
            if (!read() || int16_t(state.gamepad.thumb_rx) != 0 ||
                int16_t(state.gamepad.thumb_ry) != 0) return 46;
            scope(original_scope);
            std::puts("PASS: actual SDK turret analog-only drain, probes preserve pulses, diagonal ratio after accumulation, full range, reset; movement/buttons retained");
        }
    }
    std::puts("PASS: keyboard, mouse buttons, all 8 modifier combinations, explicit chord, mouse motion/drain, focus reset");
}
