# Security

## Reporting a problem

Please report security issues privately through GitHub's **Report a vulnerability** button on this
repository's Security tab, rather than in a public issue. You'll get a reply as soon as possible,
and credit in the fix if you'd like it.

## What FileFlow protects, and how

**Integrity, always.** Every transfer ends with a SHA-256 check of the whole file against the
sender's hash; a file that doesn't match is never saved. Damaged frames along the way are caught by
per-frame error correction and a checksum, and discarded rather than used.

**No network.** The app requests one permission, the camera. It has no internet access, so nothing
it sends or receives can leave the two phones by any other route.

**Confidentiality, in encrypted mode.**

- *Pairing.* Each phone generates a fresh P-256 key pair and shows its public key as a QR code; the
  phones, held face to face, read each other's codes with their front cameras. A camera a few
  centimetres from a screen is a channel an outsider can't write to unnoticed, which is what
  authenticates the keys — the approach of Wi-Fi Easy Connect's QR bootstrapping and of
  "Seeing-is-Believing" (McCune, Perrig and Reiter, 2005). Received points are checked to lie on
  the curve before use.
- *Key.* ECDH gives a shared secret, from which HKDF-SHA256 — salted with both public keys —
  derives a 256-bit file key, a key identifier, and the six-digit code each phone displays. Matching
  codes confirm that each phone read the other and not a third screen, in the manner of Signal's
  safety numbers and Bluetooth's numeric comparison.
- *Files.* Each file (and its name) is compressed if that helps, then encrypted with AES-256-GCM
  under a random 96-bit nonce, with the envelope header bound in as associated data. Any change to
  any byte makes the receiver refuse the file.
- *Forward secrecy.* Keys exist only in memory, for at most 30 minutes or until the app closes.
  Nothing is ever written to storage, so a phone examined later holds nothing that opens past
  transfers.
- *No downgrade.* A receiver in encrypted mode refuses unencrypted files and files sealed to any
  other pairing.

**Not protected.** Without encrypted mode, anyone whose camera can see the sending screen can
receive the file, exactly as with a QR code. Encrypted mode hides a file's contents and name but
not its approximate size or the fact that a transfer is happening. Received files are saved
unencrypted to Downloads, like any other download.
