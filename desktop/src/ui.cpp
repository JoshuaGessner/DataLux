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
        default:
            break;
    }
}

void Ui::Begin(float dt_seconds, float t_seconds) {
    dt = dt_seconds;
    t = t_seconds;
    hot = false;
}

void Ui::End() {
    if (hand_ == nullptr) hand_ = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_POINTER);
    SDL_SetCursor(hot ? hand_ : SDL_GetDefaultCursor());
    pointer.pressed = pointer.released = false;
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
