# Android account autofill

The native sign-in form supplies username/email and password autofill hints to Android's
selected autofill service. The form stays attached during authentication and commits its
autofill session only after sign-in succeeds; failed or abandoned sign-ins cancel it. This
supports installed password managers through Android Autofill, but provider behavior must be
tested on real devices with the user's chosen service. It does not implement Google sign-in.
On the owner's Galaxy phone, both Samsung's provider and Google Password Manager have been
confirmed to interact with the Play-installed staging sign-in after selecting each provider
in the phone's autofill settings. Cross-device password sharing has not been verified.

The Play staging and production builds currently share the same package and app-signing
identity, but use separate account databases. Do not associate the app and both web origins
for shared credentials while that is true: Google's credential-sharing associations can be
transitive, and a production-only association does not distinguish the staging build's identical
app identity. Use unique staging credentials. Website-to-app sharing requires a deliberate
identity-isolation design, the Play **app-signing** certificate (not the upload certificate),
and a verified Digital Asset Links deployment. The server must permit only the precise
`/.well-known/assetlinks.json` path without exposing other dotfiles.

For release acceptance, test save and fill with the selected password manager, success and
incorrect-password cases, keyboard Next/Done, screen rotation or resize, large text, narrow
and wide displays, and a failed network request. Do not record credentials or screenshots of
password-manager entries. Test staging with a disposable account and never enter production
credentials there.

References: [Android Autofill](https://developer.android.com/identity/autofill/autofill-optimize),
[Android password authentication](https://developer.android.com/identity/passwords), and
[Google credential sharing](https://developers.google.com/identity/credential-sharing/set-up).
