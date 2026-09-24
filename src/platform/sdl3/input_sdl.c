/* keyboard (events) and the first gamepad (polled), both remappable (OPTIONS > CONTROLS); the bindings are kept in
 * controls.cfg in SDL's pref folder (~/.local/share/SaberRider/ on Linux, %APPDATA%\SaberRider\ on Windows).
 * The left stick always moves. */
#include "sdl_platform.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>

enum { PAD_NONE = -1, PAD_LT = 100, PAD_RT = 101 };   /* pad codes: SDL_GamepadButton, or a trigger */

static SDL_Gamepad *pad;
static int keys[BTN_COUNT][BIND_SLOTS];   /* SDL_Scancode, 0 = none */
static int pads[BTN_COUNT][BIND_SLOTS];   /* pad code */
static bool key_down[SDL_SCANCODE_COUNT];
static struct { bool on; int dev, btn, slot; } cap;
static bool trig_held[2];                 /* a trigger past the capture threshold (it must come back before it counts again) */

static const char *const BTN_NAME[BTN_COUNT] = { "LEFT", "RIGHT", "UP", "DOWN", "JUMP", "SHOOT", "AIM", "PAUSE", "POWER" };

/* demo keyboard layout: arrows move, W/A jump, S/D shoot, Q/E aim, Enter pause; ours: X/F power.
 * pad: d-pad, south jump, east / west shoot, shoulders aim, start pause, north power */
static void defaults(int dev)
{
    static const int K[BTN_COUNT][BIND_SLOTS] = {
        { SDL_SCANCODE_LEFT }, { SDL_SCANCODE_RIGHT }, { SDL_SCANCODE_UP }, { SDL_SCANCODE_DOWN },
        { SDL_SCANCODE_W, SDL_SCANCODE_A }, { SDL_SCANCODE_S, SDL_SCANCODE_D }, { SDL_SCANCODE_Q, SDL_SCANCODE_E },
        { SDL_SCANCODE_RETURN }, { SDL_SCANCODE_X, SDL_SCANCODE_F } };
    static const int P[BTN_COUNT][BIND_SLOTS] = {
        { SDL_GAMEPAD_BUTTON_DPAD_LEFT, PAD_NONE }, { SDL_GAMEPAD_BUTTON_DPAD_RIGHT, PAD_NONE },
        { SDL_GAMEPAD_BUTTON_DPAD_UP, PAD_NONE }, { SDL_GAMEPAD_BUTTON_DPAD_DOWN, PAD_NONE },
        { SDL_GAMEPAD_BUTTON_SOUTH, PAD_NONE }, { SDL_GAMEPAD_BUTTON_EAST, SDL_GAMEPAD_BUTTON_WEST },
        { SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER },
        { SDL_GAMEPAD_BUTTON_START, PAD_NONE }, { SDL_GAMEPAD_BUTTON_NORTH, PAD_NONE } };
    if (dev == BIND_KEYBOARD) memcpy(keys, K, sizeof keys);
    else memcpy(pads, P, sizeof pads);
}

/* ---------------------------------------------------------------- controls.cfg */
static const char *cfg_path(void)
{
    static char path[1024];
    if (!path[0]) {
        char *dir = SDL_GetPrefPath("SaberRider", "SaberRider");
        if (!dir) return NULL;
        snprintf(path, sizeof path, "%scontrols.cfg", dir);
        SDL_free(dir);
    }
    return path;
}

static const char *pad_code_name(int c)
{
    if (c == PAD_LT) return SDL_GetGamepadStringForAxis(SDL_GAMEPAD_AXIS_LEFT_TRIGGER);
    if (c == PAD_RT) return SDL_GetGamepadStringForAxis(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER);
    if (c >= 0 && c < SDL_GAMEPAD_BUTTON_COUNT) return SDL_GetGamepadStringForButton((SDL_GamepadButton)c);
    return NULL;
}

static int pad_code_from_name(const char *s)
{
    SDL_GamepadAxis a = SDL_GetGamepadAxisFromString(s);
    if (a == SDL_GAMEPAD_AXIS_LEFT_TRIGGER) return PAD_LT;
    if (a == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) return PAD_RT;
    SDL_GamepadButton b = SDL_GetGamepadButtonFromString(s);
    return b == SDL_GAMEPAD_BUTTON_INVALID ? PAD_NONE : (int)b;
}

static char *trim(char *s)
{
    while (isspace((unsigned char)*s)) s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) *--e = 0;
    return s;
}

static void load(void)
{
    defaults(BIND_KEYBOARD); defaults(BIND_PAD);
    const char *path = cfg_path();
    FILE *f = path ? fopen(path, "r") : NULL;
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        char *eq = strchr(line, '='), *dot = strchr(line, '.');
        if (line[0] == '#' || !eq || !dot || dot > eq) continue;
        *eq = 0; *dot = 0;
        int dev = !strcmp(trim(line), "key") ? BIND_KEYBOARD : !strcmp(trim(line), "pad") ? BIND_PAD : -1;
        int btn = -1;
        for (int b = 0; b < BTN_COUNT; b++) if (!strcmp(trim(dot + 1), BTN_NAME[b])) btn = b;
        if (dev < 0 || btn < 0) continue;
        char *save = NULL, *tok = SDL_strtok_r(eq + 1, ",", &save);
        for (int s = 0; s < BIND_SLOTS; s++, tok = tok ? SDL_strtok_r(NULL, ",", &save) : NULL) {
            char *name = tok ? trim(tok) : "none";
            bool none = !strcmp(name, "none");
            if (dev == BIND_KEYBOARD) keys[btn][s] = none ? 0 : (int)SDL_GetScancodeFromName(name);
            else pads[btn][s] = none ? PAD_NONE : pad_code_from_name(name);
        }
    }
    fclose(f);
}

void plat_bind_save(void)
{
    const char *path = cfg_path();
    FILE *f = path ? fopen(path, "w") : NULL;
    if (!f) return;
    fprintf(f, "# Saber Rider controls (OPTIONS > CONTROLS): SDL key names / SDL gamepad button names, \"none\" = unbound\n");
    for (int b = 0; b < BTN_COUNT; b++) {
        fprintf(f, "key.%s = ", BTN_NAME[b]);
        for (int s = 0; s < BIND_SLOTS; s++) fprintf(f, "%s%s", s ? ", " : "", keys[b][s] ? SDL_GetScancodeName((SDL_Scancode)keys[b][s]) : "none");
        fputc('\n', f);
    }
    for (int b = 0; b < BTN_COUNT; b++) {
        fprintf(f, "pad.%s = ", BTN_NAME[b]);
        for (int s = 0; s < BIND_SLOTS; s++) { const char *n = pad_code_name(pads[b][s]); fprintf(f, "%s%s", s ? ", " : "", n ? n : "none"); }
        fputc('\n', f);
    }
    fclose(f);
}

/* ---------------------------------------------------------------- the CONTROLS screen */
bool plat_bind_supported(void) { return true; }
bool plat_bind_capturing(void) { return cap.on; }
void plat_bind_cancel(void) { cap.on = false; }

void plat_bind_defaults(int dev) { defaults(dev); }

void plat_bind_capture(int dev, int btn, int slot)
{
    cap.on = true; cap.dev = dev; cap.btn = btn; cap.slot = slot;
    trig_held[0] = pad && SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > 16000;
    trig_held[1] = pad && SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 16000;
}

static void pad_label(int c, char *buf, size_t n)
{
    SDL_GamepadType t = pad ? SDL_GetGamepadType(pad) : SDL_GAMEPAD_TYPE_STANDARD;
    bool ps = t == SDL_GAMEPAD_TYPE_PS3 || t == SDL_GAMEPAD_TYPE_PS4 || t == SDL_GAMEPAD_TYPE_PS5;
    bool nin = t >= SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO && t <= SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR;
    const char *s = NULL;
    switch (c) {
    case PAD_LT: s = ps ? "L2" : nin ? "ZL" : "LT"; break;
    case PAD_RT: s = ps ? "R2" : nin ? "ZR" : "RT"; break;
    case SDL_GAMEPAD_BUTTON_SOUTH: case SDL_GAMEPAD_BUTTON_EAST: case SDL_GAMEPAD_BUTTON_WEST: case SDL_GAMEPAD_BUTTON_NORTH:
        switch (pad ? SDL_GetGamepadButtonLabel(pad, (SDL_GamepadButton)c) : SDL_GetGamepadButtonLabelForType(SDL_GAMEPAD_TYPE_XBOX360, (SDL_GamepadButton)c)) {
        case SDL_GAMEPAD_BUTTON_LABEL_A: s = "A"; break;               case SDL_GAMEPAD_BUTTON_LABEL_B: s = "B"; break;
        case SDL_GAMEPAD_BUTTON_LABEL_X: s = "X"; break;               case SDL_GAMEPAD_BUTTON_LABEL_Y: s = "Y"; break;
        case SDL_GAMEPAD_BUTTON_LABEL_CROSS: s = "CROSS"; break;       case SDL_GAMEPAD_BUTTON_LABEL_CIRCLE: s = "CIRCLE"; break;
        case SDL_GAMEPAD_BUTTON_LABEL_SQUARE: s = "SQUARE"; break;     case SDL_GAMEPAD_BUTTON_LABEL_TRIANGLE: s = "TRIANGLE"; break;
        default: s = c == SDL_GAMEPAD_BUTTON_SOUTH ? "SOUTH" : c == SDL_GAMEPAD_BUTTON_EAST ? "EAST" : c == SDL_GAMEPAD_BUTTON_WEST ? "WEST" : "NORTH"; break;
        }
        break;
    case SDL_GAMEPAD_BUTTON_BACK: s = ps ? "SELECT" : nin ? "MINUS" : "BACK"; break;
    case SDL_GAMEPAD_BUTTON_GUIDE: s = "GUIDE"; break;
    case SDL_GAMEPAD_BUTTON_START: s = ps ? "START" : nin ? "PLUS" : "START"; break;
    case SDL_GAMEPAD_BUTTON_LEFT_STICK: s = ps ? "L3" : "L STICK"; break;
    case SDL_GAMEPAD_BUTTON_RIGHT_STICK: s = ps ? "R3" : "R STICK"; break;
    case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: s = ps ? "L1" : nin ? "L" : "LB"; break;
    case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: s = ps ? "R1" : nin ? "R" : "RB"; break;
    case SDL_GAMEPAD_BUTTON_DPAD_UP: s = "D-UP"; break;       case SDL_GAMEPAD_BUTTON_DPAD_DOWN: s = "D-DOWN"; break;
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT: s = "D-LEFT"; break;   case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: s = "D-RIGHT"; break;
    case SDL_GAMEPAD_BUTTON_TOUCHPAD: s = "TOUCHPAD"; break;
    default: s = pad_code_name(c); break;   /* paddles, misc */
    }
    snprintf(buf, n, "%s", s ? s : "-");
}

void plat_bind_label(int dev, int btn, int slot, char *buf, size_t n)
{
    if (!n) return;
    if (dev == BIND_KEYBOARD) {
        int k = keys[btn][slot];
        snprintf(buf, n, "%s", k ? SDL_GetScancodeName((SDL_Scancode)k) : "-");
        if (k && !buf[0]) snprintf(buf, n, "KEY %d", k);
    } else if (pads[btn][slot] == PAD_NONE) snprintf(buf, n, "-");
    else pad_label(pads[btn][slot], buf, n);
    for (char *p = buf; *p; p++) *p = (char)toupper((unsigned char)*p);
}

/* put code into the capture slot; a slot of another button already holding it gets this slot's old code */
static void assign(int code)
{
    int (*tab)[BIND_SLOTS] = cap.dev == BIND_KEYBOARD ? keys : pads;
    int old = tab[cap.btn][cap.slot];
    for (int b = 0; b < BTN_COUNT; b++)
        for (int s = 0; s < BIND_SLOTS; s++)
            if (tab[b][s] == code && !(b == cap.btn && s == cap.slot)) tab[b][s] = old;
    tab[cap.btn][cap.slot] = code;
    cap.on = false;
}

/* a capture eats the key / pad event; true when it did */
static bool capture_event(const SDL_Event *ev)
{
    if (!cap.on) return false;
    int none = cap.dev == BIND_KEYBOARD ? 0 : PAD_NONE;
    switch (ev->type) {
    case SDL_EVENT_KEY_DOWN:
        if (ev->key.repeat) return true;
        if (ev->key.scancode == SDL_SCANCODE_ESCAPE) { cap.on = false; return true; }
        if (ev->key.scancode == SDL_SCANCODE_DELETE || ev->key.scancode == SDL_SCANCODE_BACKSPACE) {
            (cap.dev == BIND_KEYBOARD ? keys : pads)[cap.btn][cap.slot] = none; cap.on = false; return true;
        }
        if (cap.dev == BIND_KEYBOARD) assign(ev->key.scancode);
        return true;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
        if (cap.dev == BIND_PAD && pad && ev->gbutton.which == SDL_GetGamepadID(pad)) assign(ev->gbutton.button);
        return cap.dev == BIND_PAD;
    case SDL_EVENT_GAMEPAD_AXIS_MOTION:
        if (cap.dev == BIND_PAD && pad && ev->gaxis.which == SDL_GetGamepadID(pad)
            && (ev->gaxis.axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER || ev->gaxis.axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER)) {
            int i = ev->gaxis.axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER;
            if (ev->gaxis.value < 8000) trig_held[i] = false;
            else if (ev->gaxis.value > 16000 && !trig_held[i]) assign(i ? PAD_RT : PAD_LT);
        }
        return false;
    default: return false;
    }
}

/* ---------------------------------------------------------------- events + polling */
void input_sdl_init(void) { load(); }

void input_sdl_event(Input *in, const SDL_Event *ev)
{
    if (ev->type == SDL_EVENT_GAMEPAD_ADDED && !pad) pad = SDL_OpenGamepad(ev->gdevice.which);
    if (ev->type == SDL_EVENT_GAMEPAD_REMOVED && pad && ev->gdevice.which == SDL_GetGamepadID(pad)) {
        SDL_CloseGamepad(pad); pad = NULL;
        int n = 0; SDL_JoystickID *ids = SDL_GetGamepads(&n);   /* fall back on another one still plugged in */
        if (ids && n > 0) pad = SDL_OpenGamepad(ids[0]);
        SDL_free(ids);
    }
    if (capture_event(ev)) return;   /* its key up still comes through below (and finds nothing held) */
    if (ev->type == SDL_EVENT_KEY_DOWN || ev->type == SDL_EVENT_KEY_UP) {
        if (ev->key.scancode >= SDL_SCANCODE_COUNT) return;
        key_down[ev->key.scancode] = ev->type == SDL_EVENT_KEY_DOWN;
        for (int b = 0; b < BTN_COUNT; b++) {
            bool bound = false, d = false;
            for (int s = 0; s < BIND_SLOTS; s++) if (keys[b][s]) { if (keys[b][s] == (int)ev->key.scancode) bound = true; if (key_down[keys[b][s]]) d = true; }
            if (bound) in->raw[b] = d;
        }
    }
}

static bool pad_code_down(int c)
{
    if (c == PAD_LT) return SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > 8000;
    if (c == PAD_RT) return SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 8000;
    return c >= 0 && SDL_GetGamepadButton(pad, (SDL_GamepadButton)c);
}

void plat_input_poll(bool down[BTN_COUNT])
{
    if (!pad) return;
    int ax = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTX), ay = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTY);
    down[BTN_LEFT]  |= ax < -8000;
    down[BTN_RIGHT] |= ax > 8000;
    down[BTN_UP]    |= ay < -8000;
    down[BTN_DOWN]  |= ay > 8000;
    for (int b = 0; b < BTN_COUNT; b++)
        for (int s = 0; s < BIND_SLOTS; s++) down[b] |= pad_code_down(pads[b][s]);
}
