// FileFlow for the desktop: a laptop, monitor, TV or projector as the sending screen.
//
//   fileflow-send [--speed steady|fast|fastest] [--compression off|auto|max] [--windowed]
//                 [--window WxH] [--code-window] [--seconds N] [--snapshot OUT.bmp [--snapshot-after S]]
//                 [FILE...]
//
// Started with files, it sends them at once, full screen. Started without, it opens: drop files on
// it, or choose them, and pick the speed and compression (remembered), and -- with a second display
// attached -- which screen shows the code. Any phone running FileFlow receives from it exactly as
// from another phone: the frames come from the same core TransmitSession, the file travels in the
// same envelope (core/envelope.h), and each cell is drawn by the same rule as the app's shader -- a
// whole number of screen pixels a side, its low three bits lighting red, green and blue. A bigger
// screen than a phone's means bigger cells, so receivers can stand further back and further off to
// the side: one screen, a room of phones.
//
// On a landscape screen the grid is drawn TURNED a quarter: the same frames, rotated, with cells
// nearly twice the size of the upright grid's on a 16:10 screen (9 px against 5 at 2560x1600). The
// receiver needs nothing for it -- a phone's camera sees the grid sideways anyway, and the detector
// resolves all four rotations from the corner markers.
//
// Keys: Ctrl/Cmd+O choose files · Ctrl/Cmd+V send the clipboard's text · 1 2 3 speed · F full
// screen · Esc stop sending.
//
// --snapshot OUT.bmp saves the screen exactly as drawn -- once the controls have faded, while
// sending -- then quits: what a receiver's camera would be pointed at, for checking against the
// decoder. --snapshot-after S takes it S seconds in instead, whatever is showing. --code-window
// presents in a window of its own even with one display, to try the presenter's view.
#include "app.h"
#ifndef __APPLE__
#include "icon_art.h"
#endif

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <cstdio>
#include <cstdlib>
#include <exception>
#include <string>
#include <vector>

namespace {

using namespace fileflow::desktop;

int Usage() {
    std::fprintf(stderr,
                 "usage: fileflow-send [--speed steady|fast|fastest] [--compression off|auto|max] [--windowed]\n"
                 "                     [--window WxH] [--code-window] [--seconds N]\n"
                 "                     [--snapshot OUT.bmp [--snapshot-after S]] [FILE...]\n"
                 "With no files, the app opens: drop files on it, or choose them.\n");
    return 2;
}

}  // namespace

int main(int argc, char** argv) {
    Settings settings = Settings::Load();
    App::Options options;
    double seconds = 0.0;  // > 0: stop after this long (for unattended runs)
    int win_w = 1100;
    int win_h = 760;
    std::vector<std::string> files;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--speed" && i + 1 < argc) {
            const std::string v = argv[++i];
            settings.speed = v == "steady" ? 0 : v == "fast" ? 1 : v == "fastest" ? 2 : kSpeeds.size();
            if (settings.speed == kSpeeds.size()) return Usage();
        } else if (a == "--compression" && i + 1 < argc) {
            const std::string v = argv[++i];
            if (v == "off") settings.compression = 0;
            else if (v == "auto") settings.compression = 1;
            else if (v == "max") settings.compression = 2;
            else return Usage();
        } else if (a == "--windowed") {
            options.windowed = true;
        } else if (a == "--code-window") {
            options.code_window = true;
        } else if (a == "--open" && i + 1 < argc) {
            options.open = argv[++i];
        } else if (a == "--no-camera") {
            options.no_camera = true;
        } else if (a == "--window" && i + 1 < argc) {
            // The window's size in points, e.g. 640x600: for checking the layout at a size.
            if (std::sscanf(argv[++i], "%dx%d", &win_w, &win_h) != 2 || win_w < 320 || win_h < 320) return Usage();
        } else if (a == "--seconds" && i + 1 < argc) {
            seconds = std::atof(argv[++i]);
        } else if (a == "--snapshot" && i + 1 < argc) {
            options.snapshot = argv[++i];
        } else if (a == "--snapshot-after" && i + 1 < argc) {
            options.snapshot_after = std::atof(argv[++i]);
        } else if (a == "--help" || a == "-h" || (!a.empty() && a[0] == '-')) {
            // macOS hands an app launched from Finder a "-psn_..." argument: not ours, not an error.
            if (a.rfind("-psn_", 0) == 0) continue;
            return Usage();
        } else {
            files.push_back(a);
        }
    }

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::fprintf(stderr, "SDL: %s\n", SDL_GetError());
        return 1;
    }
    SDL_SetAppMetadata("FileFlow", FILEFLOW_VERSION, "dev.fileflow.desktop");
    // Straight to full screen when there are files to send; otherwise a window.
    SDL_WindowFlags flags = SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_RESIZABLE;
    if (!files.empty() && !options.windowed && !options.code_window) flags |= SDL_WINDOW_FULLSCREEN;
    SDL_Window* window = SDL_CreateWindow("FileFlow", win_w, win_h, flags);
    SDL_Renderer* renderer = window != nullptr ? SDL_CreateRenderer(window, nullptr) : nullptr;
    if (renderer == nullptr) {
        std::fprintf(stderr, "SDL: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }
    SDL_SetWindowMinimumSize(window, 520, 560);
    // On macOS SDL_SetWindowIcon replaces NSApp's Dock icon. Keep the native multi-resolution
    // .icns from the bundle, with its transparent margin and rounded silhouette.
#ifndef __APPLE__
    // The taskbar/window icon uses the same artwork as the packaged .ico and Linux launcher.
    auto icon_pixels = icon::Draw(128);
    if (SDL_Surface* surface = SDL_CreateSurfaceFrom(128, 128, SDL_PIXELFORMAT_RGBA32, icon_pixels.data(), 128 * 4)) {
        SDL_SetWindowIcon(window, surface);
        SDL_DestroySurface(surface);
    }
#endif
    // Presenting waits for the display's refresh: the clock every frame is counted against.
    SDL_SetRenderVSync(renderer, 1);

    try {
        App app(window, renderer, settings, options);
        app.pending_files = files.size();
        app.Send(files);
        const auto began = Clock::now();
        while (!app.done()) {
            if (seconds > 0.0 && std::chrono::duration<double>(Clock::now() - began).count() > seconds) break;
            SDL_Event e;
            try {
                while (SDL_PollEvent(&e)) app.Event(e);
                app.Frame();
            } catch (const std::exception& error) {
                app.ReportFailure(error.what());
            }
        }
    } catch (const std::exception& error) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "FileFlow", error.what(), window);
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
