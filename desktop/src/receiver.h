// Receiving on the desktop: camera frames into core's LiveReceiver, on a thread of its own so the
// window stays smooth, and the verified file opened (with the pairing and team keys) and saved to
// Downloads/FileFlow -- as the phone's LiveDecoder does.
//
// The sender's grid is found as the phone finds it: frames go to a receiver per known grid in
// turn until one decodes a PAYLOAD -- which spans every cell and carries a CRC, so a near-miss grid
// can't -- and from then on every frame goes to that one.
#pragma once

#include "secure.h"
#include "latest_frame.h"

#include <fileflow/live.h>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace fileflow::desktop {

// Where received files go: Downloads/FileFlow -- made only when something is saved there.
std::filesystem::path ReceivedFolder(bool create = false);

struct SavedFile {
    std::string name;
    std::filesystem::path path;
    std::uint64_t bytes = 0;
};

struct ReceiveResult {
    bool ok = false;
    std::string problem;            // why not, for the person
    std::vector<SavedFile> files;   // one, or a batch's
    std::string name;               // the transfer's: the file's, or "3 files"
    std::uint64_t bytes = 0;        // all of it, as saved
    std::uint64_t link_bytes = 0;   // what crossed the link
    double seconds = 0;             // first data to verified
    std::string sealed_to;          // when it was encrypted
};

struct ReceiveDiagnostics {
    detail::FrameDeliveryStats delivery;
    std::uint64_t decoded = 0;
    double decode_ms_sum = 0;  // full LiveReceiver::Ingest wall time, including core locking
    double decode_ms_max = 0;
    double queue_ms_sum = 0;   // application offer-to-worker time; not sensor capture age
    double queue_ms_max = 0;
    std::uint64_t useful_progress_updates = 0;
    double useful_progress_age_ms = -1;
};

class Receiver {
  public:
    // [dir]: where files are saved; empty for Downloads/FileFlow (a test gives its own).
    Receiver(std::vector<secure::SealKey> keys, bool require_encrypted, std::filesystem::path dir = {});
    ~Receiver();
    Receiver(const Receiver&) = delete;
    Receiver& operator=(const Receiver&) = delete;

    // One camera frame: luma w x h, half-size chroma by default for existing callers. Co-sited
    // RGB-derived planes use full_resolution=true. Copied into a bounded latest pending slot;
    // the worker owns its active frame while newer pending frames can replace each other.
    void Offer(const std::vector<std::uint8_t>& luma, const std::vector<std::uint8_t>& u,
               const std::vector<std::uint8_t>& v, int w, int h, std::uint64_t timestamp_ns,
               bool full_resolution = false);

    [[nodiscard]] LiveStatus Status() const;
    // Set once the file has verified (or failed to), and been saved (or not).
    [[nodiscard]] std::optional<ReceiveResult> Result() const;
    [[nodiscard]] ReceiveDiagnostics Diagnostics() const;
    [[nodiscard]] std::uint64_t decoded() const { return decoded_.load(); }

  private:
    void Run();
    void Finish(const LiveReceiver& rx);

    std::vector<secure::SealKey> keys_;
    bool require_encrypted_;
    std::filesystem::path dir_;
    std::vector<std::unique_ptr<LiveReceiver>> candidates_;
    LiveReceiver* committed_ = nullptr;
    std::size_t next_ = 0;

    mutable std::mutex mu_;
    detail::LatestFrame frames_;
    LiveStatus status_;
    std::optional<ReceiveResult> result_;
    ReceiveDiagnostics diagnostics_;
    std::optional<std::chrono::steady_clock::time_point> last_progress_at_;
    std::atomic<std::uint64_t> decoded_{0};
    std::thread worker_;
};

}  // namespace fileflow::desktop
