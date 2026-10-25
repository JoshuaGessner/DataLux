// The desktop app: its screens, and the engine that shows the code.
//
// Laid out as the phone app is -- Send, Receive, Received, Teams down a navigation rail -- so the two
// read as one product. Each screen is drawn in its own file (screen_*.cpp); this class holds what
// they share: settings, the pointer, and the sending engine, which draws the code pixel-exact in
// the app's own window, or -- presenting -- full screen in a window of its own on another display,
// while the app's window becomes the presenter's view.
#pragma once

#include "camera.h"
#include "paint.h"
#include "secure.h"
#include "transfer.h"
#include "ui.h"

#include <SDL3/SDL.h>

#include <chrono>
#include <cstdint>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace fileflow::desktop {

using Clock = std::chrono::steady_clock;

inline constexpr std::array<const char*, 3> kCompressionNames = {"Off", "Auto", "Maximum"};

// The settings, remembered between runs in the app's own preferences folder.
struct Settings {
    std::size_t speed = 1;        // Fast: the setting that works on the most receivers
    std::size_t compression = 1;  // Auto
    std::string name;             // what teams see; empty until set
    bool encrypted = false;       // files sealed to the paired phone or a team
    std::string seal_to;          // a team's id (hex); empty for the paired phone

    static Settings Load();
    void Save() const;
};

// What was sent lately, newest first, so it can be sent again with one click.
struct Recent {
    std::string name;
    std::uint64_t bytes = 0;
    std::int64_t when = 0;  // seconds since the epoch
    std::vector<std::string> paths;

    static constexpr std::size_t kKeep = 3;
    static std::vector<Recent> Load();
    static void Save(const std::vector<Recent>& list);
};

// The app's folder in the system's per-user preferences, with a trailing separator.
std::string PrefPath(const char* file);
std::string Ago(std::int64_t when);
std::string Shortcut(const char* key);
void LightGrid(Painter& p, float cx, float cy, float side, float t, float lift);

class App {
  public:
    struct Options {
        bool windowed = false;
        bool code_window = false;  // present in a separate window even on one display (testing)
        std::string open;          // a screen to open at start: teams, pair, join (testing)
        bool no_camera = false;    // never open the camera (unattended runs: no permission prompt)
        std::string snapshot;
        double snapshot_after = 0;
    };

    App(SDL_Window* w, SDL_Renderer* r, Settings settings, Options options);
    ~App();
    App(const App&) = delete;
    App& operator=(const App&) = delete;

    [[nodiscard]] bool done() const noexcept { return done_; }
    void Send(std::vector<std::string> paths);
    void Event(const SDL_Event& e);
    void Frame();

    // Files given on the command line: the home screen is skipped, so a snapshot waits for them.
    std::size_t pending_files = 0;

  private:
    enum class Tab { kSend, kReceive, kReceived, kTeams };
    enum class Phase { kIdle, kPreparing, kSending };

    // Where the code is drawn: the app's window, or a full-screen window of its own on another
    // display, with its own renderer -- and so its own texture.
    struct CodeView {
        SDL_Window* window = nullptr;  // null: the app's own window
        SDL_Renderer* renderer = nullptr;
        SDL_Texture* texture = nullptr;
        bool turned = false;  // drawn a quarter turn round, for a landscape screen
        int tex_w = 0;
        int tex_h = 0;
        int pitch = 0;
        ~CodeView();
    };

    // Screens (screen_*.cpp).
    void Rail(float H);
    void TeamsScreen(float x, float W, float H);
    void TeamDetail(float x, float W, float H, const secure::Team& team);
    void PairScreen(float W, float H);
    float EncryptedRow(float x, float y, float w);  // returns its height
    void SendScreen(float x, float W, float H);
    void SendingOverlay(float W, float H, float alpha);
    void Presenter(float W, float H);
    void Setting(const std::string& id, float x, float y, float w, const char* label,
                 const std::vector<std::string>& options, std::size_t* value, const std::string& hint);

    // Encrypted mode (screen_teams.cpp).
    void StartPairing(secure::PairingFlow::Mode mode, std::optional<secure::Team> team = std::nullopt);
    void PairFinished();
    [[nodiscard]] std::string MyName() const;
    [[nodiscard]] std::optional<secure::SealKey> SealTarget();
    [[nodiscard]] std::vector<secure::SealKey> Keys();
    [[nodiscard]] const secure::PairingKey* Paired() const;

    // Actions.
    void SendClipboard();
    void SendAgain(const Recent& r);
    void Choose();
    void Stop();
    void SetSpeed(std::size_t i);
    void Wake();
    void Remember();
    void TakeChosenFiles();
    void TakePrepared();
    void Restart();
    [[nodiscard]] bool Fullscreen() const;
    static void SDLCALL OnChosen(void* self, const char* const* list, int filter);

    // The code, pixel-exact.
    std::vector<SDL_DisplayID> Displays() const;
    void OpenCodeView();
    void MakeTexture(CodeView& v);
    void Advance();
    void Upload();
    static int Pitch(int w, int h, std::uint32_t gw, std::uint32_t gh);
    void DrawGrid(CodeView& v, int w, int h);
    void Pace(double states_per_second);
    void Measure();
    void MaybeSnapshot();
    [[nodiscard]] bool presenting() const { return code_ != nullptr && code_->window != nullptr; }

    SDL_Window* window_;
    SDL_Renderer* renderer_;
    Painter paint_;
    Ui ui_;
    Settings settings_;
    Options options_;
    Tab tab_ = Tab::kSend;
    Phase phase_ = Phase::kIdle;
    bool done_ = false;

    Clock::time_point began_;
    Clock::time_point last_frame_;
    float dt_ = 0;
    float t_ = 0;
    Ease drag_;
    Ease controls_;
    Ease tab_fade_;
    Clock::time_point controls_until_;

    // Files on their way in.
    std::vector<std::string> dropped_;
    std::optional<Clock::time_point> drag_seen_;
    std::mutex chosen_mutex_;
    std::vector<std::string> chosen_;
    bool chosen_ready_ = false;
    std::future<Prepared> pending_;
    std::string preparing_name_;
    std::string problem_;
    std::vector<std::string> sending_paths_;  // what is being sent, for the recent list
    std::vector<Recent> recent_;
    std::size_t display_choice_ = 0;  // index into Displays(): where the code is shown

    // Encrypted mode.
    secure::TeamStore teams_;
    std::optional<secure::PairingKey> paired_;  // held in memory only, 30 minutes, as on the phone
    std::string team_open_;                     // the team shown in detail, by id
    std::string new_team_;                      // a name being typed for a new team
    bool naming_team_ = false;
    std::string name_edit_;
    bool editing_name_ = false;
    struct PairState {
        std::unique_ptr<secure::PairingFlow> flow;
        secure::PairingFlow::Mode mode = secure::PairingFlow::Mode::kPair;
        std::optional<secure::Team> team;  // inviting to
        Camera camera;
        Clock::time_point last_read;
        std::optional<Clock::time_point> done_at;
        bool finished = false;
        std::string outcome;
    };
    std::unique_ptr<PairState> pair_;

    // Sending.
    std::optional<Prepared> prepared_;
    std::unique_ptr<Transfer> transfer_;
    std::unique_ptr<CodeView> code_;
    SDL_Texture* preview_ = nullptr;  // the code, small, in the presenter's view
    std::vector<std::uint32_t> pixels_;
    bool went_fullscreen_ = false;
    std::uint64_t states_ = 0;
    std::optional<Clock::time_point> next_due_;
    double hz_ = 60.0;  // the code display's refresh rate: stated, then measured
    std::uint64_t presents_ = 0;
    Clock::time_point report_at_;
    std::uint64_t report_states_ = 0;
    std::uint64_t report_presents_ = 0;
    std::optional<Clock::time_point> last_present_;
    std::vector<double> intervals_;
};

}  // namespace fileflow::desktop
