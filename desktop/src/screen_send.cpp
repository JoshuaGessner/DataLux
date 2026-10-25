// The Send screen: the drop zone and its settings; while sending, the controls over the code; and,
// presenting on another display, the presenter's view of what is going there.
#include "app.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <numbers>

namespace fileflow::desktop {

namespace th = theme;

namespace {

constexpr std::array<const char*, 3> kCompressionHints = {
    "Files go exactly as they are.",
    "Documents and text shrink first; photos go as they are.",
    "Smallest transfer, longer to prepare.",
};

std::vector<std::string> SpeedNames() { return {kSpeeds[0].name, kSpeeds[1].name, kSpeeds[2].name}; }

std::string DisplayName(SDL_DisplayID id) {
    const char* n = SDL_GetDisplayName(id);
    return n != nullptr && *n != '\0' ? n : "Display " + std::to_string(id);
}

}  // namespace

void App::Setting(const std::string& id, float x, float y, float w, const char* label,
                  const std::vector<std::string>& options, std::size_t* value, const std::string& hint) {
    paint_.Text(label, x + 4, y, 13, Weight::kSemiBold, th::kTextDim);
    const std::size_t before = *value;
    *value = ui_.Segmented(id, {x, y + 24, w, 42}, options, *value);
    if (*value != before && id != "display") settings_.Save();
    paint_.Text(paint_.Fit(hint, w - 8, 13, Weight::kRegular), x + 4, y + 74, 13, Weight::kRegular, th::kTextDim);
}

void App::SendScreen(float x0, float W, float H) {
    paint_.Clear(th::kBackground);
    const float margin = W - x0 < 600 ? 20 : 32;
    const float L = x0;           // the screen's left edge (after the rail)
    const float SW = W - x0;      // and its width

    // Header: the page, and what FileFlow does.
    paint_.Text("Send", L + margin, 24, 28, Weight::kSemiBold, th::kText);
    if (SW > 560) paint_.Text("Files, sent with light", W - margin, 34, 14, Weight::kRegular, th::kTextDim, Align::kRight);

    // A second screen -- a projector, a TV -- can show the code while this one stays yours.
    const auto displays = Displays();
    const bool choose_display = displays.size() > 1 || options_.code_window;
    if (display_choice_ >= displays.size()) {
        const SDL_DisplayID here = SDL_GetDisplayForWindow(window_);
        display_choice_ = static_cast<std::size_t>(std::find(displays.begin(), displays.end(), here) - displays.begin());
    }

    // The drop zone.
    const float cw = std::min(SW - 2 * margin, 760.0F);
    const float cx0 = L + (SW - cw) / 2;
    const float extra = (recent_.empty() ? 0.0F : 36.0F + 52.0F * static_cast<float>(recent_.size())) + (choose_display ? 104.0F : 0.0F) +
                        (settings_.encrypted ? 126.0F : 76.0F);
    const float ch = std::clamp(H - 360 - extra, 260.0F, 400.0F);
    const SDL_FRect card{cx0, 84, cw, ch};
    const bool dragging = drag_seen_.has_value() && Clock::now() - *drag_seen_ < std::chrono::milliseconds(250);
    drag_.To(dragging ? 1.0F : 0.0F, dt_);
    const float lift = drag_.value;
    if (lift > 0.01F) paint_.Glow(card.x + cw / 2, card.y + ch / 2, cw * 0.7F, WithAlpha(th::kPrimary, 0.18F * lift));
    paint_.RoundRect(card, 28, Mix(th::kSurface, th::kSurfaceHigh, lift * 0.6F));
    paint_.RoundStroke(card, 28, 1.5F + lift, Mix(th::kOutline, th::kPrimary, lift));

    // The card's contents as one block, centred: the mark takes what the words and buttons leave,
    // so a short card shrinks the mark rather than piling the rest on top of each other.
    constexpr float kBelowMark = 22 + 36 + 20 + 16 + 46 + 12 + 20;  // headline, line, button, link
    const float side = std::clamp(ch - kBelowMark - 48, 64.0F, 150.0F);
    const float top = card.y + (ch - side - kBelowMark) / 2;
    const float mid = card.x + cw / 2;
    if (phase_ == Phase::kPreparing) {
        // Reading and compressing: a turning arc where the mark was.
        const float cy = top + side / 2;
        paint_.Glow(mid, cy, side * 0.9F, WithAlpha(th::kPrimary, 0.25F));
        paint_.Arc(mid, cy, side * 0.32F, 6, 0, 2 * std::numbers::pi_v<float>, WithAlpha(th::kText, 0.08F));
        paint_.Arc(mid, cy, side * 0.32F, 6, t_ * 4.2F, 1.6F + 0.8F * std::sin(t_ * 2.1F), th::kPrimary);
        const float ty = top + side + 22;
        paint_.Text(paint_.Fit("Preparing " + preparing_name_ + "…", cw - 64, 22, Weight::kSemiBold), mid, ty, 22,
                    Weight::kSemiBold, th::kText, Align::kCenter);
        paint_.Text(settings_.compression == 0 ? "Reading the files" : "Reading and compressing", mid, ty + 34, 14,
                    Weight::kRegular, th::kTextDim, Align::kCenter);
    } else {
        LightGrid(paint_, mid, top + side / 2, side, t_, lift);
        const float ty = top + side + 22;
        paint_.Text(dragging ? "Let go to send" : "Drop files here to send", mid, ty, 24, Weight::kSemiBold, th::kText,
                    Align::kCenter);
        paint_.Text("Several go as one transfer · up to " + FormatBytes(kMaxFileBytes) + " in all", mid, ty + 36, 14,
                    Weight::kRegular, th::kTextDim, Align::kCenter);
        const float bw = 200;
        const SDL_FRect choose{mid - bw / 2, ty + 72, bw, 46};
        if (ui_.Button("choose", choose, "Choose files…", true)) Choose();
        if (ui_.Link("paste", mid, choose.y + 58, "or send the clipboard's text  " + Shortcut("V"))) SendClipboard();
    }

    // The settings, under the drop zone: side by side when there's room, stacked when not.
    float sy = card.y + ch + 28;
    const bool stacked = cw < 620;
    const float colw = stacked ? cw : (cw - 24) / 2;
    Setting("speed", card.x, sy, colw, "Speed", SpeedNames(), &settings_.speed, kSpeeds[settings_.speed].hint);
    Setting("compression", stacked ? card.x : card.x + colw + 24, stacked ? sy + 104 : sy, colw, "Compression",
            {kCompressionNames[0], kCompressionNames[1], kCompressionNames[2]}, &settings_.compression,
            kCompressionHints[settings_.compression]);
    float bottom = sy + (stacked ? 196 : 98);
    bottom += 16 + EncryptedRow(card.x, bottom + 16, cw);
    if (choose_display) {
        std::vector<std::string> names;
        const SDL_DisplayID here = SDL_GetDisplayForWindow(window_);
        for (std::size_t i = 0; i < std::min<std::size_t>(displays.size(), 4); ++i) {
            names.push_back(displays[i] == here ? "This screen" : DisplayName(displays[i]));
        }
        if (options_.code_window && displays.size() == 1) names.push_back("A window (test)");
        std::size_t choice = std::min(display_choice_, names.size() - 1);
        const bool elsewhere = displays.empty() || choice >= displays.size() || displays[choice] != here;
        Setting("display", card.x, bottom + 6, cw, "Show the code on", names, &choice,
                elsewhere ? "The code goes full screen there; this window becomes your view of it."
                          : "The code fills this screen while it's sent.");
        display_choice_ = choice;
        bottom += 104;
    }

    // What was sent lately, to send again: as many as fit above the bottom line.
    const float footer_y = H - 40;
    const float limit = H - 16;
    if (!recent_.empty() && bottom + 22 + 26 + 46 <= limit) {
        float ry = bottom + 22;
        paint_.Text("Recent", card.x + 4, ry, 13, Weight::kSemiBold, th::kTextDim);
        ry += 26;
        for (std::size_t i = 0; i < recent_.size() && ry + 46 <= limit; ++i) {
            const Recent& r = recent_[i];
            const SDL_FRect row{card.x, ry, cw, 46};
            const std::string id = "recent/" + std::to_string(i);
            const bool clicked = ui_.Row(id, row);
            const float hover = ui_.Anim(id).value;
            // A small document, folded at the corner.
            paint_.RoundRect({row.x + 16, row.y + 12, 17, 22}, 3, WithAlpha(th::kPrimary, 0.85F));
            paint_.RoundRect({row.x + 26, row.y + 12, 7, 7}, 1.5F, Mix(th::kSurface, th::kSurfaceHigh, hover));
            paint_.Text(paint_.Fit(r.name, cw * 0.5F, 14, Weight::kSemiBold), row.x + 46, row.y + 4, 14, Weight::kSemiBold,
                        th::kText);
            paint_.Text(FormatBytes(r.bytes) + " · " + Ago(r.when), row.x + 46, row.y + 24, 12, Weight::kRegular,
                        th::kTextDim);
            paint_.Text("Send again", row.x + cw - 18, row.y + 14, 13, Weight::kSemiBold, Mix(th::kTextDim, th::kPrimary, hover),
                        Align::kRight);
            if (clicked && phase_ == Phase::kIdle) SendAgain(r);
            ry += 52;
        }
        bottom = ry - 6;
    }

    // A problem with the last files, until something else happens.
    if (!problem_.empty()) {
        const float tw = paint_.Measure(problem_, 14, Weight::kRegular) + 56;
        const SDL_FRect toast{L + (SW - tw) / 2, H - 104, tw, 40};
        paint_.RoundRect(toast, 20, th::kSurfaceHigh);
        paint_.Circle(toast.x + 22, toast.y + 20, 4, th::kError);
        paint_.Text(problem_, toast.x + 36, toast.y + 11, 14, Weight::kRegular, th::kText);
    }

    // The one line of instruction, where there's room for it.
    const char* footer = "Point a phone running FileFlow at the code. No network, no pairing, nothing to install.";
    if (bottom + 20 <= footer_y && paint_.Measure(footer, 13, Weight::kRegular) <= SW - 2 * margin) {
        paint_.Text(footer, L + SW / 2, footer_y, 13, Weight::kRegular, th::kTextDim, Align::kCenter);
    }
}

// While sending on this screen: what is going, and the speed and Stop, over the code -- faded out
// after a few seconds so the screen shows nothing but the code.
void App::SendingOverlay(float W, float H, float alpha) {
    const auto& e = prepared_->envelope;
    const Speed& s = transfer_->speed;
    const double pass = static_cast<double>(transfer_->PassFrames()) / s.states_per_second;
    const double shown = static_cast<double>(states_) / static_cast<double>(std::max<std::uint64_t>(1, transfer_->PassFrames()));
    char stats[96];
    std::snprintf(stats, sizeof stats, "each pass %.0f s · shown %.1f×", std::max(1.0, pass), shown);
    std::string what = FormatBytes(e.original_size);
    if (e.codec != 0) what += " → " + FormatBytes(e.bytes.size());
    const std::string name = paint_.Fit(prepared_->name, std::min(W * 0.4F, 420.0F), 15, Weight::kSemiBold);
    const float w1 = paint_.Measure(name, 15, Weight::kSemiBold);
    const float w2 = paint_.Measure(what + "  ·  " + stats, 13, Weight::kRegular);
    const float tw = std::max(w1 + 40, w2 + 40) + 28;
    const float slide = (1 - alpha) * -16;
    const SDL_FRect top{(W - tw) / 2, 20 + slide, tw, 62};
    paint_.RoundRect(top, 22, WithAlpha(th::kSurface, 0.94F * alpha));
    const float pulse = 0.5F + 0.5F * std::sin(t_ * 4);
    paint_.Glow(top.x + 26, top.y + 22, 12, WithAlpha(th::kTertiary, 0.5F * pulse * alpha));
    paint_.Circle(top.x + 26, top.y + 22, 4.5F, WithAlpha(th::kTertiary, alpha));
    paint_.Text(name, top.x + 40, top.y + 10, 15, Weight::kSemiBold, WithAlpha(th::kText, alpha));
    paint_.Text(what + "  ·  " + stats, top.x + 40, top.y + 34, 13, Weight::kRegular, WithAlpha(th::kTextDim, alpha));

    const float bw = std::min(W - 40, 460.0F);
    const SDL_FRect bar{(W - bw) / 2, H - 82 - slide, bw, 58};
    paint_.RoundRect(bar, 29, WithAlpha(th::kSurface, 0.94F * alpha));
    const std::size_t speed = ui_.Segmented("send-speed", {bar.x + 9, bar.y + 9, bw - 140, 40}, SpeedNames(), settings_.speed);
    if (speed != settings_.speed) SetSpeed(speed);
    if (ui_.Button("stop", {bar.x + bw - 122, bar.y + 9, 113, 40}, "Stop", false)) Stop();
}

// Presenting: the code is full screen on the other display; this window is the presenter's -- a
// live view of the code, what is going, the speed and Stop -- and stays put, never covered.
void App::Presenter(float W, float H) {
    paint_.Clear(th::kBackground);
    const float margin = 32;
    const auto& e = prepared_->envelope;
    const Speed& s = transfer_->speed;
    const SDL_DisplayID there = SDL_GetDisplayForWindow(code_->window);
    const float pulse = 0.5F + 0.5F * std::sin(t_ * 4);
    paint_.Glow(margin + 6, 40, 14, WithAlpha(th::kTertiary, 0.5F * pulse));
    paint_.Circle(margin + 6, 40, 5, th::kTertiary);
    paint_.Text(options_.code_window ? "Showing the code in its own window" : "Showing the code on " + DisplayName(there),
                margin + 22, 28, 18, Weight::kSemiBold, th::kText);
    paint_.Text("Esc stops", W - margin, 31, 14, Weight::kRegular, th::kTextDim, Align::kRight);

    // The code, as the other screen shows it, letterboxed into the space there is.
    const float pw = std::min(W - 2 * margin, 820.0F);
    const SDL_FRect frame{(W - pw) / 2, 76, pw, std::max(160.0F, H - 76 - 196)};
    paint_.RoundRect(frame, 20, Hex(0x000000));
    if (preview_ != nullptr && code_->tex_w > 0) {
        const float aspect = static_cast<float>(code_->tex_w) / static_cast<float>(code_->tex_h);
        float w = frame.w - 40;
        float h = w / aspect;
        if (h > frame.h - 40) {
            h = frame.h - 40;
            w = h * aspect;
        }
        const float k = paint_.scale();
        const SDL_FRect dst{(frame.x + (frame.w - w) / 2) * k, (frame.y + (frame.h - h) / 2) * k, w * k, h * k};
        SDL_RenderTexture(renderer_, preview_, nullptr, &dst);
    }

    const double pass = static_cast<double>(transfer_->PassFrames()) / s.states_per_second;
    const double shown = static_cast<double>(states_) / static_cast<double>(std::max<std::uint64_t>(1, transfer_->PassFrames()));
    char stats[128];
    std::snprintf(stats, sizeof stats, "%s%s%s · each pass %.0f s · shown %.1f×", FormatBytes(e.original_size).c_str(),
                  e.codec != 0 ? " → " : "", e.codec != 0 ? FormatBytes(e.bytes.size()).c_str() : "", std::max(1.0, pass), shown);
    const float y = frame.y + frame.h + 20;
    paint_.Text(paint_.Fit(prepared_->name, pw, 18, Weight::kSemiBold), frame.x + 4, y, 18, Weight::kSemiBold, th::kText);
    paint_.Text(stats, frame.x + 4, y + 28, 13, Weight::kRegular, th::kTextDim);
    const float row = y + 62;
    const std::size_t speed = ui_.Segmented("present-speed", {frame.x, row, std::min(360.0F, pw - 140), 42}, SpeedNames(), settings_.speed);
    if (speed != settings_.speed) SetSpeed(speed);
    if (ui_.Button("present-stop", {frame.x + pw - 120, row, 120, 42}, "Stop", false)) Stop();
}

}  // namespace fileflow::desktop
