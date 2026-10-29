package dev.fileflow

/** Deliberately maps known protocol codes instead of showing arbitrary server response text. */
class AccountError(message: String, val invalidatesAuthentication: Boolean = false) : Exception(message) {
    companion object {
        fun fromResponse(status: Int, code: String, authenticated: Boolean, deleting: Boolean = false, verifying: Boolean = false): AccountError {
            if (deleting && code == "INVALID_PASSWORD") return AccountError("That password is incorrect. Your account has not been deleted.")
            if (deleting && code == "ADMIN_ACCOUNT_PROTECTED") return AccountError("Remove this account’s administrator access from the DataLux server console before deleting it.")
            if (code == "EMAIL_NOT_VERIFIED") return AccountError("Verify your email before buying or restoring Pro. Send a verification email from this screen, then refresh your account.")
            if (code == "MAIL_NOT_CONFIGURED") return AccountError("Verification email is not available yet. Your existing access is unchanged.")
            if (status == 401) return AccountError(if (authenticated)
                "Please sign in again. Your offline Pro access has been kept." else "Invalid email or password.", authenticated)
            return AccountError(when (status) {
                400 -> if (deleting) "Check your password and type DELETE to confirm. Your account has not been deleted." else "Check your details and try again."
                403 -> if (deleting) "Account deletion could not be authorized. Refresh your account and try again."
                    else if (verifying) "The verification request could not be authorized. Refresh your account and try again."
                    else "This purchase belongs to a different DataLux account. Sign in to the account used to buy Pro."
                409 -> if (deleting) "Account deletion could not be completed. Please refresh your account before trying again."
                    else "This purchase is not ready to activate. Wait for payment confirmation, then restore purchases."
                429 -> "Too many attempts. Please try again later."
                503 -> if (verifying) "Verification email is not available right now. Please try again later."
                    else if (deleting) "Account deletion is unavailable right now. Your account is unchanged. Please try again later."
                    else "Purchases are not available yet. Please try again later."
                else -> "The account request could not be completed. Please try again."
            })
        }
    }
}
