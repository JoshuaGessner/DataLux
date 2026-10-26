#include "ui.h"

#include <algorithm>

namespace fileflow::desktop {

namespace th = theme;

float Smooth(float x) { return x * x * (3 - 2 * x); }

void Ui::Event(const SDL_Event& e) {
    switch (e.type) {
        case SDL_EVENT_MOUSE_MOTION:
            pointer.x = e.motion.x;
            pointer.y = e.motion.y;
            break;
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
            if (e.button.button == SDL_BUTTON_LEFT) {
                pointer.down = true;
                pointer.pressed = true;
            }
            break;
        case SDL_EVENT_MOUSE_BUTTON_UP:
            if (e.button.button == SDL_BUTTON_LEFT) {
                pointer.down = false;
                pointer.released = true;
            }
            break;
        case SDL_EVENT_WINDOW_MOUSE_LEAVE:
            pointer.x = pointer.y = -1;
            break;
        case SDL_EVENT_TEXT_INPUT:
            if (!focus_.empty() && e.text.text != nullptr) typed_ += e.text.text;
            break;
        case SDL_EVENT_KEY_DOWN:
            if (focus_.empty()) break;
            if (e.key.key == SDLK_BACKSPACE) ++backspaces_;
            if (e.key.key == SDLK_RETURN || e.key.key == SDLK_KP_ENTER) enter_ = true;
            if (e.key.key == SDLK_ESCAPE) Unfocus();
            break;
        default:
            break;
    }
}

void Ui::Unfocus() {
    focus_.clear();
    if (window != nullptr) SDL_StopTextInput(window);
}

void Ui::Begin(float dt_seconds, float t_seconds) {
    dt = dt_seconds;
    t = t_seconds;
    hot = false;
    over_field_ = false;
}

void Ui::End() {
    if (hand_ == nullptr) hand_ = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_POINTER);
    if (ibeam_ == nullptr) ibeam_ = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_TEXT);
    SDL_SetCursor(over_field_ ? ibeam_ : hot ? hand_ : SDL_GetDefaultCursor());
    // A click anywhere but a text field ends typing there.
    if (pointer.pressed && !over_field_ && !focus_.empty()) Unfocus();
    pointer.pressed = pointer.released = false;
    typed_.clear();
    backspaces_ = 0;
    enter_ = false;
}

bool Ui::TextField(const std::string& id, SDL_FRect r, std::string* value, const std::string& placeholder,
                   std::size_t max_chars) {
    const bool over = pointer.In(r);
    over_field_ |= over;
    if (over && pointer.pressed && focus_ != id) {
        focus_ = id;
        if (window != nullptr) SDL_StartTextInput(window);
    }
    const bool focused = focus_ == id;
    bool submitted = false;
    if (focused) {
        for (int i = 0; i < backspaces_ && !value->empty(); ++i) {
            // One character back, whole: UTF-8 continuation bytes go with it.
            std::size_t cut = value->size() - 1;
            while (cut > 0 && (static_cast<unsigned char>((*value)[cut]) & 0xC0) == 0x80) --cut;
            value->erase(cut);
        }
        for (char c : typed_) {
            if (static_cast<unsigned char>(c) < 0x20) continue;
            std::size_t chars = 0;
            for (char v : *value) chars += (static_cast<unsigned char>(v) & 0xC0) != 0x80 ? 1 : 0;
            if (chars >= max_chars && (static_cast<unsigned char>(c) & 0xC0) != 0x80) break;
            *value += c;
        }
        submitted = enter_;
    }
    Ease& f = anim_[id + "/focus"];
    f.To(focused ? 1.0F : 0.0F, dt);
    paint.RoundRect(r, 12, th::kSurfaceHigh);
    paint.RoundStroke(r, 12, 1 + f.value, Mix(th::kOutline, th::kPrimary, f.value));
    const float ty = r.y + (r.h - paint.LineHeight(15)) / 2;
    if (value->empty() && !focused) {
        paint.Text(placeholder, r.x + 14, ty, 15, Weight::kRegular, th::kTextDim);
    } else {
        const std::string shown = paint.Fit(*value, r.w - 32, 15, Weight::kRegular);
        const float w = paint.Text(shown, r.x + 14, ty, 15, Weight::kRegular, th::kText);
        // The caret, blinking.
        if (focused && std::fmod(t, 1.0F) < 0.6F) paint.Rect({r.x + 15 + w, ty + 2, 1.5F, 17}, th::kPrimary);
    }
    return submitted;
}

bool Ui::RailItem(const std::string& id, float cx, float y, Icon icon, const char* label, bool chosen) {
    const SDL_FRect hit{cx - 40, y, 80, 64};
    const bool over = pointer.In(hit);
    hot |= over && !chosen;
    Ease& sel = anim_[id + "/sel"];
    Ease& hover = anim_[id];
    if (!seen_.contains(id)) sel.value = chosen ? 1.0F : 0.0F;
    seen_[id] = true;
    sel.To(chosen ? 1.0F : 0.0F, dt, 0.1F);
    hover.To(over ? 1.0F : 0.0F, dt);
    // The pill grows out from the icon's centre as it's chosen.
    const float pw = 56 * (0.5F + 0.5F * sel.value);
    if (sel.value > 0.01F) paint.RoundRect({cx - pw / 2, y + 4, pw, 32}, 16, WithAlpha(th::kPrimaryContainer, sel.value));
    else if (hover.value > 0.01F) paint.RoundRect({cx - 28, y + 4, 56, 32}, 16, WithAlpha(th::kSurfaceHigh, hover.value));
    const Rgba ink = Mix(th::kTextDim, th::kText, std::max(sel.value, hover.value));
    const float iy = y + 20;
    switch (icon) {
        case Icon::kSend:  // an arrow up, over a bar
            paint.RoundRect({cx - 1.4F, iy - 4, 2.8F, 10}, 1.4F, ink);
            paint.Polygon({{cx, iy - 10}, {cx + 7, iy - 2}, {cx - 7, iy - 2}}, ink);
            paint.RoundRect({cx - 8, iy + 8, 16, 2.6F}, 1.3F, ink);
            break;
        case Icon::kReceive:  // a camera
            paint.RoundRect({cx - 10, iy - 6, 20, 14}, 3, ink);
            paint.RoundRect({cx - 4, iy - 9, 8, 4}, 1.5F, ink);
            paint.Circle(cx, iy + 1, 4.2F, sel.value > 0.5F ? th::kPrimaryContainer : th::kBackground);
            paint.Circle(cx, iy + 1, 2.2F, ink);
            break;
        case Icon::kReceived:  // a folder
            paint.RoundRect({cx - 10, iy - 7, 9, 5}, 1.5F, ink);
            paint.RoundRect({cx - 10, iy - 5, 20, 13}, 2.5F, ink);
            break;
        case Icon::kTeams:  // two people
            paint.Circle(cx - 4, iy - 4, 3.6F, ink);
            paint.Circle(cx + 5, iy - 3, 3.0F, WithAlpha(ink, 0.8F));
            paint.RoundRect({cx - 10, iy + 1, 12, 8}, 4, ink);
            paint.RoundRect({cx + 1, iy + 2, 9, 6.5F}, 3.2F, WithAlpha(ink, 0.8F));
            break;
        case Icon::kAccount:
            paint.Circle(cx, iy - 5, 4.2F, ink);
            paint.RoundRect({cx - 8, iy + 1, 16, 9}, 4.5F, ink);
            break;
    }
    paint.Text(label, cx, y + 40, 12, chosen ? Weight::kSemiBold : Weight::kRegular, ink, Align::kCenter);
    return over && pointer.released;
}

bool Ui::Button(const std::string& id, SDL_FRect r, const std::string& label, bool filled) {
    const bool over = pointer.In(r);
    hot |= over;
    Ease& hover = anim_[id];
    Ease& press = anim_[id + "/press"];
    hover.To(over ? 1.0F : 0.0F, dt);
    press.To(over && pointer.down ? 1.0F : 0.0F, dt, 0.05F);
    const float k = 1 - 0.03F * press.value;
    const SDL_FRect d{r.x + r.w * (1 - k) / 2, r.y + r.h * (1 - k) / 2, r.w * k, r.h * k};
    if (filled) {
        paint.Glow(d.x + d.w / 2, d.y + d.h / 2, d.w * 0.7F, WithAlpha(th::kPrimary, 0.12F * hover.value));
        paint.RoundRect(d, d.h / 2, Mix(th::kPrimary, th::kText, 0.25F * hover.value));
    } else {
        paint.RoundRect(d, d.h / 2, Mix(th::kSurfaceHigh, th::kOutline, hover.value));
    }
    paint.Text(label, d.x + d.w / 2, d.y + (d.h - paint.LineHeight(15)) / 2, 15, Weight::kSemiBold,
               filled ? th::kOnPrimary : th::kText, Align::kCenter);
    return over && pointer.released;
}

bool Ui::Link(const std::string& id, float cx, float y, const std::string& label, float size) {
    const float w = paint.Measure(label, size, Weight::kRegular);
    const SDL_FRect r{cx - w / 2 - 8, y - 4, w + 16, paint.LineHeight(size) + 8};
    const bool over = pointer.In(r);
    hot |= over;
    Ease& hover = anim_[id];
    hover.To(over ? 1.0F : 0.0F, dt);
    paint.Text(label, cx, y, size, Weight::kRegular, Mix(th::kTextDim, th::kPrimary, hover.value), Align::kCenter);
    return over && pointer.released;
}

std::size_t Ui::Segmented(const std::string& id, SDL_FRect r, const std::vector<std::string>& labels, std::size_t chosen) {
    const std::size_t n = std::max<std::size_t>(1, labels.size());
    paint.RoundRect(r, r.h / 2, th::kSurfaceHigh);
    const float seg = r.w / static_cast<float>(n);
    Ease& at = anim_[id + "/at"];
    if (!seen_.contains(id)) at.value = static_cast<float>(chosen);  // no slide on first sight
    seen_[id] = true;
    at.To(static_cast<float>(chosen), dt, 0.09F);
    const SDL_FRect pill{r.x + 3 + at.value * seg, r.y + 3, seg - 6, r.h - 6};
    paint.RoundRect(pill, pill.h / 2, th::kPrimary);
    std::size_t result = chosen;
    for (std::size_t i = 0; i < labels.size(); ++i) {
        const SDL_FRect cell{r.x + static_cast<float>(i) * seg, r.y, seg, r.h};
        const bool over = pointer.In(cell);
        hot |= over && i != chosen;
        const float near = std::max(0.0F, 1 - std::abs(at.value - static_cast<float>(i)));
        const Rgba ink = Mix(over ? th::kText : th::kTextDim, th::kOnPrimary, near);
        paint.Text(paint.Fit(labels[i], seg - 16, 14, Weight::kSemiBold), cell.x + seg / 2,
                   cell.y + (r.h - paint.LineHeight(14)) / 2, 14, Weight::kSemiBold, ink, Align::kCenter);
        if (over && pointer.released) result = i;
    }
    return result;
}

bool Ui::Switch(const std::string& id, SDL_FRect r, bool on) {
    const bool over = pointer.In(r);
    hot |= over;
    Ease& at = anim_[id + "/on"];
    if (!seen_.contains(id)) at.value = on ? 1.0F : 0.0F;
    seen_[id] = true;
    at.To(on ? 1.0F : 0.0F, dt, 0.08F);
    const SDL_FRect track{r.x + r.w - 52, r.y + (r.h - 30) / 2, 52, 30};
    paint.RoundRect(track, 15, Mix(th::kSurfaceHigh, th::kPrimary, at.value));
    paint.RoundStroke(track, 15, 1.5F, Mix(th::kOutline, th::kPrimary, at.value));
    const float knob = 10 + 4 * at.value;
    paint.Circle(track.x + 15 + 22 * at.value, track.y + 15, knob, Mix(th::kTextDim, th::kOnPrimary, at.value));
    return over && pointer.released ? !on : on;
}

bool Ui::Row(const std::string& id, SDL_FRect r) {
    const bool over = pointer.In(r);
    hot |= over;
    Ease& hover = anim_[id];
    hover.To(over ? 1.0F : 0.0F, dt);
    paint.RoundRect(r, 14, Mix(th::kSurface, th::kSurfaceHigh, hover.value));
    return over && pointer.released;
}

}  // namespace fileflow::desktop
