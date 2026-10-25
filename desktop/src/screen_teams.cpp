// Encrypted mode on the desktop, as on the phone: pairing with a phone face to face, teams -- made,
// joined, invited to -- and choosing what a send is sealed to. Pairing shows this computer's code
// for the phone's front camera and reads the phone's code with this computer's camera.
#include "app.h"

#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstdlib>
#include <numbers>

namespace fileflow::desktop {

namespace th = theme;

namespace {

constexpr auto kPairingLifetime = std::chrono::minutes(30);  // PairingStore.LIFETIME_MS
constexpr auto kReadEvery = std::chrono::milliseconds(100);

std::string Initials(const std::string& name) {
    std::string out;
    bool start = true;
    for (char c : name) {
        if (c == ' ' || c == '-' || c == '_') {
            start = true;
        } else if (start && out.size() < 2 && static_cast<unsigned char>(c) < 0x80) {
            out += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            start = false;
        } else {
            start = false;
        }
    }
    return out.empty() ? "?" : out;
}

// A person, as a disc with their initials, its colour from their name -- as on the phone.
void Avatar(Painter& p, float cx, float cy, float r, const std::string& name) {
    static const Rgba kHues[] = {th::kPrimaryContainer, Hex(0x2E4A54), Hex(0x2F4A2C)};
    static const Rgba kInks[] = {Hex(0xBDEBFF), Hex(0xCFE6F0), Hex(0xC8F0C2)};
    std::size_t h = 0;
    for (char c : name) h = h * 31 + static_cast<unsigned char>(c);
    p.Circle(cx, cy, r, kHues[h % 3]);
    p.Text(Initials(name), cx, cy - p.LineHeight(r * 0.8F) / 2, r * 0.8F, Weight::kSemiBold, kInks[h % 3], Align::kCenter);
}

void Lock(Painter& p, float cx, float cy, bool closed, Rgba ink) {
    p.RoundRect({cx - 8, cy - 2, 16, 12}, 3, ink);
    // The shackle: closed, an arch over the body; open, lifted and swung aside.
    if (closed) p.Arc(cx, cy - 2, 5, 2.4F, std::numbers::pi_v<float>, std::numbers::pi_v<float>, ink);
    else p.Arc(cx + 5, cy - 5, 5, 2.4F, std::numbers::pi_v<float> * 1.1F, std::numbers::pi_v<float> * 0.9F, ink);
}

}  // namespace

std::string App::MyName() const {
    if (!settings_.name.empty()) return settings_.name;
    const char* user = std::getenv("USER");
    if (user == nullptr) user = std::getenv("USERNAME");
    if (user == nullptr || *user == '\0') return "This computer";
    std::string u = user;
    u[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(u[0])));
    return secure::Team::CleanName(u + "'s computer");
}

const secure::PairingKey* App::Paired() {
    if (!paired_) return nullptr;
    const auto age = std::chrono::milliseconds(secure::NowMs() - paired_->created_ms);
    if (age < std::chrono::milliseconds(0) || age >= kPairingLifetime) paired_.reset();
    return paired_ ? &*paired_ : nullptr;
}

std::optional<secure::SealKey> App::SealTarget() {
    if (!settings_.seal_to.empty()) {
        for (const auto& t : teams_.All()) {
            if (t.IdHex() == settings_.seal_to) return t.AsSealKey();
        }
        return std::nullopt;  // a chosen team that's gone: not the pairing instead, without asking
    }
    if (const auto* k = Paired()) return *k;
    return std::nullopt;
}

std::vector<secure::SealKey> App::Keys() {
    std::vector<secure::SealKey> keys;
    if (const auto* k = Paired()) keys.push_back(*k);
    for (const auto& t : teams_.All()) keys.push_back(t.AsSealKey());
    return keys;
}

void App::StartPairing(secure::PairingFlow::Mode mode, std::optional<secure::Team> team) {
    auto next = std::make_unique<PairState>();
    next->mode = mode;
    next->team = team;
    next->flow = std::make_unique<secure::PairingFlow>(mode, MyName(), std::move(team));
    pair_ = std::move(next);
    if (!options_.no_camera) pair_->camera.Open();
    ui_.Unfocus();
}

void App::PairFinished() {
    auto& f = *pair_->flow;
    using Mode = secure::PairingFlow::Mode;
    if (pair_->mode == Mode::kPair && f.key()) {
        paired_ = *f.key();
        settings_.encrypted = true;
        settings_.seal_to.clear();
        settings_.Save();
        pair_->outcome = "Paired. Check both screens show the same code.";
    } else if (pair_->mode == Mode::kJoin && f.joined()) {
        teams_.Add(*f.joined());
        pair_->outcome = "Joined " + f.joined()->name + ".";
    } else if (pair_->mode == Mode::kInvite && pair_->team) {
        // The newest copy: another member may have joined since this screen opened.
        secure::Team latest = *pair_->team;
        for (const auto& t : teams_.All()) {
            if (t.id == latest.id) latest = t;
        }
        const std::string name = f.member_name().value_or("");
        teams_.Add(latest.WithMember({name, secure::NowMs()}));
        pair_->outcome = (name.empty() ? std::string("The other phone") : name) + " has joined " + latest.name + ".";
    }
    pair_->finished = true;
    pair_->done_at = Clock::now();
}

// ------------------------------------------------------------------------------------------- rail

void App::Rail(float H) {
    paint_.Rect({0, 0, 92, H}, th::kSurface);
    LightGrid(paint_, 46, 42, 34, t_ * 0.6F, 0);
    struct Item {
        Tab tab;
        Ui::Icon icon;
        const char* label;
    };
    const Item items[] = {{Tab::kSend, Ui::Icon::kSend, "Send"},
                          {Tab::kReceive, Ui::Icon::kReceive, "Receive"},
                          {Tab::kReceived, Ui::Icon::kReceived, "Received"},
                          {Tab::kTeams, Ui::Icon::kTeams, "Teams"}};
    float y = 96;
    for (const auto& it : items) {
        if (ui_.RailItem(std::string("rail/") + it.label, 46, y, it.icon, it.label, tab_ == it.tab)) {
            if (tab_ == Tab::kReceive && it.tab != Tab::kReceive) StopReceiving();  // the camera only while it's shown
            tab_ = it.tab;
            team_open_.clear();
            received_dirty_ = true;
            ui_.Unfocus();
        }
        y += 76;
    }
}

// ------------------------------------------------------------------------------------ Send's row

float App::EncryptedRow(float x, float y, float w) {
    const bool on = settings_.encrypted;
    const SDL_FRect row{x, y, w, 60};
    paint_.RoundRect(row, 30, th::kSurface);
    Lock(paint_, x + 30, y + 30, on, on ? th::kPrimary : th::kTextDim);
    paint_.Text("Encrypted", x + 54, y + 9, 15, Weight::kSemiBold, th::kText);
    std::string state = "Off — anyone whose camera sees the code could read it";
    if (on) {
        const auto target = SealTarget();
        if (!settings_.seal_to.empty()) state = target ? "Sealed to " + target->label : "That team is no longer on this computer";
        else if (const auto* k = Paired()) state = "Sealed to the paired phone · " + k->code;
        else state = "Not paired — pair with a phone, or choose a team";
    }
    paint_.Text(paint_.Fit(state, w - 150, 13, Weight::kRegular), x + 54, y + 31, 13, Weight::kRegular, th::kTextDim);
    const bool now_on = ui_.Switch("encrypted", {x + w - 76, y, 64, 60}, on);
    if (now_on != on) {
        settings_.encrypted = now_on;
        settings_.Save();
    }
    if (!settings_.encrypted) return 60;
    const auto teams = teams_.All();
    if (teams.empty()) {
        // Nothing to choose between: paired, it's the phone; not, the way to pair.
        if (Paired() != nullptr) return 60;
        if (ui_.Button("pair-now", {x, y + 70, w, 42}, "Pair with a phone", false)) {
            StartPairing(secure::PairingFlow::Mode::kPair);
        }
        return 112;
    }

    // What it's sealed to: the paired phone, or one of the teams.
    std::vector<std::string> options = {Paired() != nullptr ? "Paired phone" : "Pair a phone…"};
    std::size_t chosen = 0;
    for (std::size_t i = 0; i < teams.size() && i < 3; ++i) {
        options.push_back(teams[i].name);
        if (teams[i].IdHex() == settings_.seal_to) chosen = i + 1;
    }
    const std::size_t pick = ui_.Segmented("seal-to", {x, y + 70, w, 40}, options, chosen);
    if (pick != chosen) {
        if (pick == 0) {
            settings_.seal_to.clear();
            if (Paired() == nullptr) StartPairing(secure::PairingFlow::Mode::kPair);
        } else {
            settings_.seal_to = teams[pick - 1].IdHex();
        }
        settings_.Save();
    }
    return 110;
}

// ------------------------------------------------------------------------------------------ Teams

void App::TeamsScreen(float x0, float W, float H) {
    paint_.Clear(th::kBackground);
    const float L = x0 + 36;
    const float cw = std::min(W - x0 - 72, 680.0F);
    const auto teams = teams_.All();
    if (!team_open_.empty()) {
        for (const auto& t : teams) {
            if (t.IdHex() == team_open_) return TeamDetail(x0, W, H, t);
        }
        team_open_.clear();
    }
    paint_.Text("Teams", L, 26, 28, Weight::kSemiBold, th::kText);
    paint_.Text("A group that sends encrypted files to each other: one screen, every member's phone.", L, 66, 14,
                Weight::kRegular, th::kTextDim);
    float y = 106;

    // Who this computer is, to the teams it joins.
    const SDL_FRect me{L, y, cw, 76};
    paint_.RoundRect(me, 20, th::kSurface);
    Avatar(paint_, me.x + 38, me.y + 38, 20, MyName());
    paint_.Text("Your name", me.x + 72, me.y + 12, 13, Weight::kRegular, th::kTextDim);
    if (editing_name_) {
        const bool done = ui_.TextField("name", {me.x + 72, me.y + 32, cw - 200, 34}, &name_edit_, "Your name");
        if (ui_.Button("name-save", {me.x + cw - 112, me.y + 18, 96, 40}, "Save", true) || done) {
            const std::string clean = secure::Team::CleanName(name_edit_);
            if (!clean.empty()) {
                settings_.name = clean;
                settings_.Save();
            }
            editing_name_ = false;
            ui_.Unfocus();
        }
    } else {
        paint_.Text(paint_.Fit(MyName(), cw - 220, 17, Weight::kSemiBold), me.x + 72, me.y + 34, 17, Weight::kSemiBold, th::kText);
        if (ui_.Button("name-edit", {me.x + cw - 112, me.y + 18, 96, 40}, "Change", false)) {
            editing_name_ = true;
            name_edit_ = MyName();
        }
    }
    y += 92;

    // The paired phone, for sending to one phone alone.
    const SDL_FRect pr{L, y, cw, 64};
    paint_.RoundRect(pr, 20, th::kSurface);
    Lock(paint_, pr.x + 34, pr.y + 32, Paired() != nullptr, Paired() != nullptr ? th::kPrimary : th::kTextDim);
    if (const auto* k = Paired()) {
        const auto left = std::chrono::duration_cast<std::chrono::minutes>(
            kPairingLifetime - std::chrono::milliseconds(secure::NowMs() - k->created_ms));
        paint_.Text("Paired with a phone · " + k->code, pr.x + 62, pr.y + 12, 15, Weight::kSemiBold, th::kText);
        paint_.Text(std::to_string(std::max<long>(1, static_cast<long>(left.count()))) + " min left · kept only while FileFlow is open",
                    pr.x + 62, pr.y + 34, 13, Weight::kRegular, th::kTextDim);
        if (ui_.Button("forget", {pr.x + cw - 112, pr.y + 12, 96, 40}, "Forget", false)) paired_.reset();
    } else {
        paint_.Text("No phone paired", pr.x + 62, pr.y + 12, 15, Weight::kSemiBold, th::kText);
        paint_.Text("Pair to send to one phone alone, encrypted.", pr.x + 62, pr.y + 34, 13, Weight::kRegular, th::kTextDim);
        if (ui_.Button("pair", {pr.x + cw - 112, pr.y + 12, 96, 40}, "Pair", false)) StartPairing(secure::PairingFlow::Mode::kPair);
    }
    y += 84;

    // The teams.
    if (teams.empty()) {
        paint_.Glow(L + cw / 2, y + 60, 70, WithAlpha(th::kPrimary, 0.18F));
        paint_.Text("No teams yet", L + cw / 2, y + 40, 20, Weight::kSemiBold, th::kText, Align::kCenter);
        paint_.Text("Make one and invite the others, or join someone else's.", L + cw / 2, y + 72, 14, Weight::kRegular,
                    th::kTextDim, Align::kCenter);
        y += 120;
    }
    for (const auto& t : teams) {
        if (y + 64 > H - 80) break;
        const SDL_FRect row{L, y, cw, 64};
        if (ui_.Row("team/" + t.IdHex(), row)) {
            team_open_ = t.IdHex();
            ui_.Unfocus();
        }
        Avatar(paint_, row.x + 34, row.y + 32, 18, t.name);
        std::size_t count = 0;
        for (const auto& m : t.members) count += m.needs_new_key ? 0 : 1;
        const secure::Member* lead = t.Leader();
        const std::string who = t.is_leader ? "You lead it" : "Led by " + (lead != nullptr ? lead->name : std::string("someone else"));
        paint_.Text(t.name, row.x + 64, row.y + 12, 15, Weight::kSemiBold, th::kText);
        paint_.Text(who + " · " + std::to_string(std::max<std::size_t>(1, count)) + (count == 1 ? " member" : " members"),
                    row.x + 64, row.y + 34, 13, Weight::kRegular, th::kTextDim);
        paint_.Text("›", row.x + cw - 24, row.y + 18, 20, Weight::kRegular, th::kTextDim);
        y += 72;
    }

    // New team, Join a team.
    const float by = std::min(y + 12, H - 72);
    if (naming_team_) {
        const bool done = ui_.TextField("new-team", {L, by, cw - 232, 46}, &new_team_, "Team name, e.g. Field crew");
        if (ui_.Button("make", {L + cw - 220, by, 104, 46}, "Make", true) || done) {
            const std::string name = secure::Team::CleanName(new_team_);
            if (!name.empty()) {
                const auto team = secure::Team::Create(name, MyName(), secure::NowMs());
                teams_.Add(team);
                team_open_ = team.IdHex();
            }
            naming_team_ = false;
            new_team_.clear();
            ui_.Unfocus();
        }
        if (ui_.Button("make-cancel", {L + cw - 104, by, 104, 46}, "Cancel", false)) naming_team_ = false;
    } else {
        if (ui_.Button("new-team", {L, by, (cw - 12) / 2, 46}, "New team", true)) {
            naming_team_ = true;
            new_team_.clear();
        }
        if (ui_.Button("join", {L + (cw + 12) / 2, by, (cw - 12) / 2, 46}, "Join a team", false)) {
            StartPairing(secure::PairingFlow::Mode::kJoin);
        }
    }
}

void App::TeamDetail(float x0, float W, float H, const secure::Team& team) {
    const float L = x0 + 36;
    const float cw = std::min(W - x0 - 72, 680.0F);
    if (ui_.Link("back", L + 30, 22, "‹  Teams", 14)) {
        team_open_.clear();
        return;
    }
    paint_.Text(team.name, L, 48, 28, Weight::kSemiBold, th::kText);
    const secure::Member* lead = team.Leader();
    paint_.Text(team.is_leader ? "You lead this team. You invite people to it."
                               : "Led by " + (lead != nullptr ? lead->name : std::string("someone else")) + ".",
                L, 88, 14, Weight::kRegular, th::kTextDim);
    float y = 126;
    const float list_h = 48 + 56 * static_cast<float>(std::max<std::size_t>(1, team.members.size()));
    const SDL_FRect card{L, y, cw, std::min(list_h, H - y - 120)};
    paint_.RoundRect(card, 20, th::kSurface);
    std::size_t count = 0;
    for (const auto& m : team.members) count += m.needs_new_key ? 0 : 1;
    paint_.Text("Members · " + std::to_string(count), card.x + 20, card.y + 14, 15, Weight::kSemiBold, th::kText);
    float my = card.y + 48;
    for (const auto& m : team.members) {
        if (my + 48 > card.y + card.h) break;
        Avatar(paint_, card.x + 38, my + 20, 18, m.name.empty() ? "?" : m.name);
        paint_.Text(m.name + (m.you ? " (you)" : ""), card.x + 68, my + 2, 15, Weight::kRegular, th::kText);
        paint_.Text(m.leader ? "Leader" : m.needs_new_key ? "Needs the new key: invite again" : "Member", card.x + 68, my + 22, 12,
                    Weight::kRegular, th::kTextDim);
        my += 56;
    }
    if (!team.is_leader) {
        paint_.Text("The team as it was when you joined.", card.x + 20, card.y + card.h - 26, 12, Weight::kRegular, th::kTextDim);
    }
    y = card.y + card.h + 20;
    if (team.is_leader) {
        if (ui_.Button("invite", {L, y, (cw - 12) / 2, 46}, "Invite someone", true)) {
            StartPairing(secure::PairingFlow::Mode::kInvite, team);
        }
        if (ui_.Button("delete", {L + (cw + 12) / 2, y, (cw - 12) / 2, 46}, "Delete team", false)) {
            teams_.Remove(team.IdHex());
            if (settings_.seal_to == team.IdHex()) settings_.seal_to.clear();
            settings_.Save();
            team_open_.clear();
        }
    } else if (ui_.Button("leave", {L, y, (cw - 12) / 2, 46}, "Leave team", false)) {
        teams_.Remove(team.IdHex());
        if (settings_.seal_to == team.IdHex()) settings_.seal_to.clear();
        settings_.Save();
        team_open_.clear();
    }
}

// ---------------------------------------------------------------------------------------- pairing

void App::PairScreen(float W, float H) {
    using Mode = secure::PairingFlow::Mode;
    using Stage = secure::PairingFlow::Stage;
    PairState& p = *pair_;
    secure::PairingFlow& f = *p.flow;
    paint_.Clear(th::kBackground);

    // The camera: each new frame read for a code, a tenth of a second apart.
    const auto now = Clock::now();
    if (p.camera.Poll(renderer_) && now - p.last_read >= kReadEvery && !p.finished) {
        p.last_read = now;
        if (auto text = ReadQr(p.camera.luma().data(), p.camera.width(), p.camera.height())) {
            if (f.Read(*text) && f.stage() == Stage::kDone) PairFinished();
        }
    }
    if (p.done_at && now - *p.done_at > std::chrono::milliseconds(p.mode == Mode::kJoin ? 5000 : 2500)) {
        pair_.reset();
        return;
    }

    const std::string title = p.mode == Mode::kPair ? "Pair with a phone"
                              : p.mode == Mode::kJoin ? "Join a team"
                                                      : "Invite to " + (p.team ? p.team->name : std::string("the team"));
    const char* how = p.mode == Mode::kPair  ? "On the phone, tap Encrypted to pair. Hold it up in front of this screen, close to the code, its screen facing this computer's camera."
                      : p.mode == Mode::kJoin ? "On the leader's phone: Teams › the team › Invite someone. Hold it up in front of this screen, close to the code."
                                              : "On the new member's phone: Settings › Teams › Join a team. Hold it up in front of this screen, close to the code.";
    paint_.Text(title, 48, 26, 26, Weight::kSemiBold, th::kText);
    paint_.Text(paint_.Fit(how, W - 96, 14, Weight::kRegular), 48, 64, 14, Weight::kRegular, th::kTextDim);

    // This computer's code, as big as there's room for: the phone's front camera reads it.
    const float side = std::min({H - 150, (W - 144) * 0.55F, 560.0F});
    const float qx = 48;
    const float qy = 104;
    paint_.RoundRect({qx, qy, side, side}, 24, Hex(0xFFFFFF));
    const float k = paint_.scale();
    DrawQr(renderer_, f.qr(), (qx + 12) * k, (qy + 12) * k, (side - 24) * k);

    // What this computer's camera sees -- mirrored, as a camera facing you is -- and where it's got.
    const float rx = qx + side + 40;
    const float rw = std::min(W - rx - 48, 440.0F);
    const float ph = rw * 0.6F;
    const SDL_FRect view{rx, qy, rw, ph};
    paint_.RoundRect(view, 20, Hex(0x000000));
    const auto state = p.camera.state();
    if (state == Camera::State::kOn && p.camera.preview() != nullptr) {
        const float aspect = static_cast<float>(p.camera.width()) / static_cast<float>(std::max(1, p.camera.height()));
        float w = rw;
        float h = w / aspect;
        if (h > ph) {
            h = ph;
            w = h * aspect;
        }
        const SDL_FRect dst{(rx + (rw - w) / 2) * k, (qy + (ph - h) / 2) * k, w * k, h * k};
        SDL_RenderTextureRotated(renderer_, p.camera.preview(), nullptr, &dst, 0, nullptr, SDL_FLIP_HORIZONTAL);
    } else {
        const char* msg = state == Camera::State::kNone     ? "This computer has no camera to read the phone's code."
                          : state == Camera::State::kDenied ? "FileFlow can't use the camera. Allow it in your system's privacy settings."
                                                            : "Starting the camera… (allow it if you're asked)";
        paint_.Text(paint_.Fit(msg, rw - 32, 14, Weight::kRegular), rx + rw / 2, qy + ph / 2 - 10, 14, Weight::kRegular, th::kTextDim,
                    Align::kCenter);
    }

    float y = qy + ph + 28;
    const char* stage = "Hold the phone's screen up to this computer's camera.";
    switch (f.stage()) {
        case Stage::kScanning: break;
        case Stage::kGotKey: stage = "Got it. Keep holding until both screens show this code."; break;
        case Stage::kHandingOver: stage = "Paired. Now handing over the team: keep holding…"; break;
        case Stage::kWaitingForTeam: stage = "Paired. Now receiving the team: keep holding…"; break;
        case Stage::kDone: stage = ""; break;
    }
    if (f.foreign_invite() && f.stage() != Stage::kDone) stage = "That's a team code from a different pairing. Hold up the leader's phone.";
    if (f.key()) {
        paint_.Text(f.key()->code, rx, y, 44, Weight::kSemiBold, th::kText);
        y += 64;
    }
    const std::string said = p.finished ? p.outcome : stage;
    paint_.Text(paint_.Fit(said, rw, 15, Weight::kRegular), rx, y, 15, Weight::kRegular, p.finished ? th::kTertiary : th::kTextDim);
    y += 40;
    if (p.finished) {
        if (ui_.Button("pair-done", {rx, y, 140, 46}, "Done", true)) pair_.reset();
    } else if (ui_.Button("pair-cancel", {rx, y, 140, 46}, "Cancel", false)) {
        pair_.reset();
    }
}

}  // namespace fileflow::desktop
