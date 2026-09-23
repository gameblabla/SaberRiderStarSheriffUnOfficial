#include "input.h"

static SDL_Gamepad *pad;

void input_event(Input *in, const SDL_Event *ev)
{
    if (ev->type == SDL_EVENT_GAMEPAD_ADDED && !pad) pad = SDL_OpenGamepad(ev->gdevice.which);
    if (ev->type == SDL_EVENT_KEY_DOWN || ev->type == SDL_EVENT_KEY_UP) {
        bool d = ev->type == SDL_EVENT_KEY_DOWN;
        switch (ev->key.scancode) {    /* demo keyboard layout: arrows move, W/A jump, S/D shoot, Q/E aim, Enter pause; ours: X/F power */
        case SDL_SCANCODE_LEFT:  in->raw[BTN_LEFT] = d; break;
        case SDL_SCANCODE_RIGHT: in->raw[BTN_RIGHT] = d; break;
        case SDL_SCANCODE_UP:    in->raw[BTN_UP] = d; break;
        case SDL_SCANCODE_DOWN:  in->raw[BTN_DOWN] = d; break;
        case SDL_SCANCODE_W: case SDL_SCANCODE_A: in->raw[BTN_JUMP] = d; break;
        case SDL_SCANCODE_S: case SDL_SCANCODE_D: in->raw[BTN_SHOOT] = d; break;
        case SDL_SCANCODE_Q: case SDL_SCANCODE_E: in->raw[BTN_AIM] = d; break;
        case SDL_SCANCODE_RETURN: in->raw[BTN_PAUSE] = d; break;
        case SDL_SCANCODE_X: case SDL_SCANCODE_F: in->raw[BTN_POWER] = d; break;
        default: break;
        }
    }
}

void input_update(Input *in)
{
    bool down[BTN_COUNT];
    for (int i = 0; i < BTN_COUNT; i++) down[i] = in->raw[i];
    if (pad) {
        int ax = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTX), ay = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTY);
        down[BTN_LEFT]  |= SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_DPAD_LEFT)  || ax < -8000;
        down[BTN_RIGHT] |= SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_DPAD_RIGHT) || ax > 8000;
        down[BTN_UP]    |= SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_DPAD_UP)    || ay < -8000;
        down[BTN_DOWN]  |= SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_DPAD_DOWN)  || ay > 8000;
        down[BTN_JUMP]  |= SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_SOUTH);
        down[BTN_SHOOT] |= SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_EAST) || SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_WEST);
        down[BTN_AIM]   |= SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER) || SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER)
                        || SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > 8000 || SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 8000;
        down[BTN_PAUSE] |= SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_START);
        down[BTN_POWER] |= SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_NORTH);   /* was a second jump button */
    }
    for (int i = 0; i < BTN_COUNT; i++) {
        int *s = &in->state[i];
        if (down[i]) { if (*s == 2) *s = 0; else if (*s != 0) *s = 2; }
        else         { if (*s == 3) *s = 1; else if (*s != 1) *s = 3; }
    }
}
