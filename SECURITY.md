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
- *Forward secrecy.* Pairing keys exist only in memory, for at most 30 minutes or until the app
  closes. Nothing about a pairing is written to storage, so a phone examined later holds nothing
  that opens past paired transfers.
- *Teams.* A team key is a random 256-bit AES key, handed to a new member only over a fresh pairing
  made for the purpose: sealed with AES-256-GCM under that pairing's key, the pairing's key id bound
  in as associated data. The member answers, sealed the same way, with their display name and an
  HMAC under the team key, which is how the leader's phone confirms the hand-over and learns who
  joined. Team keys ARE stored, wrapped by a non-exportable Android Keystore key, so a copy of the
  app's data alone opens nothing. A team has no forward secrecy: anyone who holds its key can open
  every transfer ever sent to it, and a phone that leaves the team keeps whatever it already
  received. Removing a member replaces the key (and its id) with a new one, which the leader hands
  to each remaining member again; the removed phone keeps only the old key. The member list is
  bookkeeping on the leader's phone, not access control: the key is.
- *The desktop app* implements the same formats (desktop/src/secure.cpp, on Mbed TLS), held to
  the phone's own test vectors. Its pairing keys live in memory only, for 30 minutes, as on the
  phone. Its team keys are stored in a file in the user's application-preferences folder, readable
  only by that user (mode 0600) -- but NOT wrapped by a hardware-held key as on Android: anyone who
  can read that user's files can read them. Wrapping them with the system keychain is planned.
- *No downgrade.* A receiver in encrypted mode refuses unencrypted files and files sealed to any
  key it does not hold.

**Not protected.** Without encrypted mode, anyone whose camera can see the sending screen can
receive the file, exactly as with a QR code. Encrypted mode hides a file's contents and name but
not its approximate size or the fact that a transfer is happening. Received files are saved
unencrypted to Downloads, like any other download.
