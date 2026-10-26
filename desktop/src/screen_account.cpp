#include "app.h"

#include <algorithm>

namespace fileflow::desktop {
namespace th = theme;

void App::AccountScreen(float x, float W, float /*H*/) {
    paint_.Clear(th::kBackground);
    const float width = std::min(W - x - 48, 680.0F);
    const float left = x + (W - x - width) / 2;
    paint_.Text("Account", left, 28, 28, Weight::kSemiBold, th::kText);
    const SDL_FRect card{left, 96, width, 142};
    paint_.RoundRect(card, 24, th::kSurface);
    const bool community = !license::StoreBuild();
    const std::string title = community ? "FileFlow community build" : pro_grant_ ? "FileFlow Pro" : "FileFlow Free";
    paint_.Text(title, left + 24, 118, 23, Weight::kSemiBold, th::kText);
    const std::string status = account_session_ ? account_session_->email : community ? "All features are available in this build." :
        pro_grant_ ? "Activated on this computer. Works offline." : "Send and receive single files for free.";
    paint_.Text(paint_.Fit(status, width - 48, 15, Weight::kRegular), left + 24, 158, 15, Weight::kRegular, th::kTextDim);
    paint_.Text(paint_.Fit("Your files and encryption stay available with every plan.", width - 48, 13, Weight::kRegular),
                left + 24, 194, 13, Weight::kRegular, th::kTextDim);

    const bool busy = account_request_.valid() || account_reply_.has_value();
    if (account_device_) {
        paint_.Text(account_device_->user_code, left, 264, 30, Weight::kSemiBold, th::kText);
        paint_.Text("Enter this code in your browser, then approve this computer.", left, 310, 14, Weight::kRegular, th::kTextDim);
        if (ui_.Button("account/browser", {left, 344, width, 42}, "Open sign-in page", true)) SDL_OpenURL(account_device_->verification_uri.c_str());
        if (ui_.Button("account/cancel", {left, 398, width, 42}, "Cancel sign-in", false)) {
            account_device_.reset(); account_cancelled_ = true; account_message_ = "Sign-in canceled.";
        }
    } else {
        if (account_session_) {
            const float half = (width - 12) / 2;
            if (ui_.Button("account/refresh", {left, 266, half, 46}, busy ? "Connecting…" : account_needs_sign_in_ ? "Sign in again" : "Refresh Pro", true) && !busy) {
                if (account_needs_sign_in_) BeginAccountSignIn(); else RefreshAccount();
            }
            if (ui_.Button("account/logout", {left + half + 12, 266, half, 46}, "Sign out", false) && !busy) SignOutAccount();
        } else if (ui_.Button("account/signin", {left, 266, width, 46}, busy ? "Connecting…" : "Sign in through your browser", true) && !busy) {
            BeginAccountSignIn();
        }
        if (ui_.Button("account/open", {left, 330, width, 42}, "Manage FileFlow account", false)) {
            const auto url = account::Origin() + "/app.html#account";
            if (!SDL_OpenURL(url.c_str())) account_message_ = "Could not open your browser. Visit the FileFlow website.";
        }
        if (license::TrustConfigured()) {
            if (ui_.Button("account/import", {left, 388, width, 42}, "Import offline activation…", false) && !busy) ChooseLicense();
        } else if (!community) {
            paint_.Text("Pro activation is not available in this build yet.", left, 398, 14, Weight::kRegular, th::kTextDim);
        }
    }
    if (!account_message_.empty()) {
        paint_.Text(paint_.Fit(account_message_, width, 14, Weight::kRegular), left, 452, 14, Weight::kRegular, th::kText);
    }
}

void App::ChooseLicense() {
    if (phase_ != Phase::kIdle || receiver_ || pair_) {
        account_message_ = "Finish the current transfer before activating Pro.";
        return;
    }
    static constexpr SDL_DialogFileFilter filters[] = {{"FileFlow Pro activation", "fileflow-license"}};
    SDL_ShowOpenFileDialog(OnChosen, new std::shared_ptr<ChosenFiles>(chosen_license_), window_, filters, 1, nullptr, false);
}

void App::TakeChosenLicense() {
    std::vector<std::string> paths;
    {
        std::lock_guard lock(chosen_license_->mutex);
        if (!chosen_license_->ready) return;
        chosen_license_->ready = false;
        paths.swap(chosen_license_->paths);
    }
    if (paths.size() == 1) ImportLicense(paths.front());
}

void App::ImportLicense(const std::string& path) {
    if (phase_ != Phase::kIdle || receiver_ || pair_) {
        account_message_ = "Finish the current transfer before activating Pro.";
        return;
    }
    tab_ = Tab::kAccount;
    const auto text = license::ReadFile(path);
    const auto grant = license::VerifyTrusted(text);
    if (!grant) {
        account_message_ = license::TrustConfigured() ? "This activation file could not be verified. Download it again from your account." :
            "Pro activation is not available in this build yet.";
        return;
    }
    if (account_session_ && grant->account_id != account_session_->user_id) {
        account_message_ = "This activation belongs to a different account. Sign out before importing it.";
        return;
    }
    const auto target = PrefPath("pro.fileflow-license");
    if (target.empty()) {
        account_message_ = "Could not find a safe place to save your activation.";
        return;
    }
    try {
        if (account_session_) {
            auto next = *account_session_; next.license = text;
            account::SaveSession(PrefPath("account-session.json"), next); account_session_ = std::move(next);
        } else secure::WritePrivateFile(target, text);
        pro_grant_ = grant;
        account_message_ = "Pro is activated on this computer.";
    } catch (...) {
        account_message_ = "Could not save your activation. Check disk space and folder permissions.";
    }
}

void App::BeginAccountSignIn() {
    if (phase_ != Phase::kIdle || receiver_ || pair_) { account_message_ = "Finish the current transfer before signing in."; return; }
    account_cancelled_ = false; account_message_ = "Opening secure browser sign-in…";
    account_request_ = std::async(std::launch::async, account::Start);
}

void App::RefreshAccount() {
    if (!account_session_ || account_request_.valid()) return;
    const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    if (account_needs_sign_in_ || account_session_->expires_at <= milliseconds) {
        account_needs_sign_in_ = true; account_refresh_at_ = Clock::now() + std::chrono::hours(24);
        account_message_ = "Sign in again to refresh your account. Offline Pro access is unchanged."; return;
    }
    account_refresh_at_ = Clock::now() + std::chrono::minutes(5);
    account_request_ = std::async(std::launch::async, account::Refresh, *account_session_);
}

void App::SignOutAccount() {
    if (!account_session_ || phase_ != Phase::kIdle || receiver_ || pair_) {
        account_message_ = "Finish the current transfer before signing out."; return;
    }
    try {
        // Clear local access even offline. The captured server token is only used for revocation.
        const auto license_path = PrefPath("pro.fileflow-license"), session_path = PrefPath("account-session.json");
        if (license_path.empty() || session_path.empty()) throw std::runtime_error("Private account path unavailable");
        secure::WritePrivateFile(license_path, "");
        secure::WritePrivateFile(session_path, "");
        const auto previous = *account_session_; account_session_.reset(); pro_grant_.reset(); account_needs_sign_in_ = false;
        account_request_ = std::async(std::launch::async, account::Logout, previous);
        account_message_ = "Signed out locally.";
    } catch (...) { account_message_ = "Could not clear this account. Check your preferences folder permissions."; }
}

void App::PumpAccount() {
    const auto now = Clock::now();
    const bool idle = phase_ == Phase::kIdle && !receiver_ && !pair_;
    if (account_request_.valid() && account_request_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        try { account_reply_ = account_request_.get(); }
        catch (...) { account_reply_ = account::Reply{}; }
        if (account_cancelled_) { account_reply_.reset(); account_cancelled_ = false; }
    }
    if (account_reply_) {
        const auto kind = account_reply_->kind;
        const bool access_change = kind == account::Reply::Kind::kSession || kind == account::Reply::Kind::kAccess;
        // A server revocation is applied only after the current transfer has ended.
        if (access_change && !idle) return;
        auto reply = std::move(*account_reply_); account_reply_.reset();
        account_message_ = reply.message;
        if (kind == account::Reply::Kind::kDevice) {
            account_device_ = reply.device;
            account_device_until_ = now + std::chrono::seconds(reply.device.expires_in);
            account_poll_at_ = now + std::chrono::seconds(reply.device.interval);
            if (!SDL_OpenURL(reply.device.verification_uri.c_str())) account_message_ = "Open the FileFlow sign-in page in your browser and enter this code.";
        } else if (kind == account::Reply::Kind::kPending) {
            account_poll_at_ = now + std::chrono::seconds(std::max(account_device_ ? account_device_->interval : 5, reply.retry_after));
        } else if (kind == account::Reply::Kind::kSession || kind == account::Reply::Kind::kAccess) {
            try {
                auto next = kind == account::Reply::Kind::kSession ? reply.session : *account_session_;
                next.license = reply.license;
                account::SaveSession(PrefPath("account-session.json"), next);
                account_session_ = std::move(next); pro_grant_ = license::VerifyTrusted(reply.license);
                account_needs_sign_in_ = false;
                account_device_.reset(); account_refresh_at_ = now + std::chrono::hours(6);
            } catch (...) { account_message_ = "Could not save the account update. Existing offline access is unchanged."; }
        } else if (kind == account::Reply::Kind::kError) {
            if (reply.authentication_required) account_needs_sign_in_ = true;
            account_device_.reset(); account_refresh_at_ = now + std::chrono::minutes(5);
        }
    }
    if (account_device_ && now >= account_device_until_) { account_device_.reset(); account_message_ = "Sign-in expired. Choose Sign in to get a new code."; }
    if (account_request_.valid() || account_reply_ || !idle) return;
    if (account_device_ && now >= account_poll_at_) account_request_ = std::async(std::launch::async, account::Poll, *account_device_);
    else if (account_session_ && now >= account_refresh_at_) RefreshAccount();
}

}  // namespace fileflow::desktop
