// The desktop app's widgets, shared by its screens: pill buttons, text links, segmented choices,
// and the pointer and easing state they run on. Immediate mode -- each frame a screen calls
// Button(...) and gets back whether it was clicked -- because the screens are few and simple, and
// a retained widget tree would be more machinery than they need.
#pragma once

#include "paint.h"

#include <SDL3/SDL.h>

#include <chrono>
#include <cmath>
#include <map>
#include <string>
#include <vector>

namespace fileflow::desktop {

// Eases a value toward a target, frame by frame: the same feel at any frame rate.
struct Ease {
    float value = 0;
    void To(float target, float dt, float seconds = 0.12F) {
        value += (target - value) * (1 - std::exp(-dt / seconds));
        if (std::abs(target - value) < 0.001F) value = target;
    }
};

struct Pointer {
    float x = -1;
    float y = -1;
    bool down = false;
    bool pressed = false;   // went down this frame
    bool released = false;  // came up this frame
    [[nodiscard]] bool In(SDL_FRect r) const { return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h; }
};

float Smooth(float x);

class Ui {
  public:
    explicit Ui(Painter& p) : paint(p) {}

    Painter& paint;
    Pointer pointer;
    float dt = 0;       // seconds since the last frame
    float t = 0;        // seconds since the app started
    bool hot = false;   // the pointer is over something clickable, this frame

    // Takes the pointer's part of an event.
    void Event(const SDL_Event& e);
    // Starts a frame: the hand cursor is decided by what the frame draws.
    void Begin(float dt_seconds, float t_seconds);
    // Ends a frame: sets the cursor, forgets this frame's clicks.
    void End();

    Ease& Anim(const std::string& id) { return anim_[id]; }

    // A pill button: lightens under the pointer, sinks while pressed. True when clicked.
    bool Button(const std::string& id, SDL_FRect r, const std::string& label, bool filled);
    // A line of text that works as a button. True when clicked.
    bool Link(const std::string& id, float cx, float y, const std::string& label, float size = 13);
    // Choices on a track; the chosen one's pill slides to it. Returns the choice.
    std::size_t Segmented(const std::string& id, SDL_FRect r, const std::vector<std::string>& labels, std::size_t chosen);
    // A labelled switch. Returns its new state.
    bool Switch(const std::string& id, SDL_FRect r, bool on);
    // A rounded card that brightens under the pointer, for a row of a list. True when clicked.
    bool Row(const std::string& id, SDL_FRect r);
    // A one-line text box: click to type in it. True when Return is pressed in it.
    bool TextField(const std::string& id, SDL_FRect r, std::string* value, const std::string& placeholder,
                   std::size_t max_chars = 40);
    [[nodiscard]] bool typing() const { return !focus_.empty(); }
    void Unfocus();

    enum class Icon { kSend, kReceive, kReceived, kTeams };
    // A destination on the navigation rail: its icon over its label, a pill behind it when chosen.
    bool RailItem(const std::string& id, float cx, float y, Icon icon, const char* label, bool chosen);

    SDL_Window* window = nullptr;  // for text input

  private:
    std::map<std::string, Ease> anim_;
    std::map<std::string, bool> seen_;
    SDL_Cursor* hand_ = nullptr;
    SDL_Cursor* ibeam_ = nullptr;
    bool over_field_ = false;
    std::string focus_;      // the text field typed into, if any
    std::string typed_;      // this frame's typing
    int backspaces_ = 0;
    bool enter_ = false;
};

}  // namespace fileflow::desktop
