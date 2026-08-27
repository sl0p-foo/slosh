# The macOS builder

Signed, notarized macOS builds of slosh happen on a dedicated mac, not on a
laptop. This file says what is on that machine, how it got there, and what to
do when it stops working.

Which mac, and whose certificate, is in `macos-release.mk`, the one file of
this flow that is not in the repo. Copy `macos-release.mk.example` to it and
fill in three lines. Examples below call the builder `m1`, which is just what
`BUILDER` happens to say; `$BUILDER` in a shell snippet means that host.

## Everyday use

From a checkout on any machine that can ssh to the builder, **including a
Linux one** (see "Driving it from Linux" below):

```sh
make macos-dist              # this working tree -> signed, notarized zip in dist/
make macos-dist REF=v0.2.0   # a pushed tag instead (this is a release)

make -f Makefile.macos builder-check   # is the builder still healthy?
make -f Makefile.macos remote-dist     # what macos-dist forwards to
```

About 40 seconds end to end, most of it Apple's notary service.

## What is in the repo and what is not

The **mechanism** is tracked; the **identity** is not.

| in git | |
|---|---|
| `Makefile`'s `macos-dist` | the target you invoke; forwards and does nothing else |
| `Makefile.macos` | the pipeline: build, sign, zip, notarize, staple |
| `contrib/macos-builder-setup` | provisions a builder, checks one |
| `contrib/macos-remote-build` | drives a build, verifies and fetches the artifact |
| `macos-release.mk.example` | the template for the file below |
| this file | |

| not in git | |
|---|---|
| `macos-release.mk` | team id, signer name, builder host |
| `~/.config/slosh/notary.env` | which App Store Connect key to notarize with |

So nothing tracked names a host or an identity: `macos-release.mk` supplies
them, and the targets that need one say which is missing rather than composing
`Developer ID Application:  ()` and letting `codesign` fail obscurely a minute
into a build. A clone without the file gets a `make macos-dist` that explains
itself instead of a missing-file error.

That split is also why the builder is sent `macos-release.mk` and nothing else:
`Makefile.macos` and the scripts arrive there from git, at the commit being
released, so the release is signed by the pipeline belonging to it rather than
by whatever the laptop was carrying.

The two source modes are not interchangeable:

| mode | what gets built | version string | use it for |
|---|---|---|---|
| default | this working tree, rsynced | `…-dirty` when dirty | "does it sign, will Apple take it" |
| `REF=…` | a commit **origin publishes**, from a clean tree | the tag or short id | releases |

`REF=` resolves through `git ls-remote origin`, so it builds what the server
has and refuses anything unpushed, including a local branch of the same name.
That matters because the default mode rsyncs `.git` too, so the builder's
`refs/heads/master` can be a commit only your laptop has ever seen. An artifact
named after a commit nobody can fetch is worse than no artifact.

Then the release continues: `contrib/brew-release --tap … v0.2.0`.

## Driving it from Linux

Works, and is tested: `make -f Makefile.macos remote-dist` from a Debian box
produced a notarized `slosh-78ad926-macos-arm64.zip`, and that artifact, after
a round trip through Linux, still verifies as `accepted / source=Notarized
Developer ID` on a mac.

This end needs only **ssh, rsync, git, GNU make** and something that can hash a
file (`sha256sum`, `shasum` or `openssl`). What it does *not* need is a mac:

* **Verification happens on the builder.** `codesign` and `spctl` are macOS
  tools, so the artifact is unpacked and assessed there, before it is fetched.
  The verdict is the same command on the same machine no matter who asked.
* **This end hashes what arrives** and compares it to the hash the builder
  vouched for. Same bytes, same verdict: that is the whole of what a
  non-mac can honestly check, and it is enough.

The scripts come with the checkout; add a `macos-release.mk` and point its
`BUILDER` at a `Host` entry in `~/.ssh/config`, done.

The one step that genuinely requires a mac is **handing the signing key over**,
because it is read out of a login keychain. Run `contrib/macos-builder-setup`
once from the mac that holds the identity; afterwards everything, `--check`
included, works from Linux. Run it from Linux against a builder that has no
identity yet and it says so in those words rather than dying on
`security: not found`.

## What is on the builder

| | |
|---|---|
| toolchain | Command Line Tools; zig 0.16.0 at `~/zig-0.16.0/zig` (a symlink into `~/zig/`) |
| checkout | `~/src/slosh`, cloned from `https://git.sl0p.foo/slosh.git` |
| keychain | `~/Library/Keychains/slosh-build.keychain-db` |
| keychain password | `~/.config/slosh/build-keychain.pass`, mode 0600 |
| signing key | `Developer ID Application: <SIGNER> (<TEAM_ID>)`, Developer ID certs last five years |
| notary key | `~/.config/slosh/notary/AuthKey_XXXXXXXXXX.p8`, mode 0600 |
| notary profile | `slosh-notary`, stored **in the build keychain**, not the login one |

### Why a separate keychain

Nobody logs into the builder, so its **login keychain is never unlocked** and
anything that needs the private key over ssh dies with `User interaction is not
allowed`. A keychain of its own can be unlocked non-interactively from a
password file, and `Makefile.macos` does exactly that in its `unlock` target
before every sign and every notarization. It also holds nothing but slosh's
signing material, so handing this machine a second project's key is a decision,
not an accident.

Two things make it usable headlessly, and both are easy to lose:

* `security set-keychain-settings` with **no arguments**: no idle timeout and
  no lock on sleep. The default six-hour timeout would relock the keychain
  mid-notarization for no visible reason.
* `security set-key-partition-list -S apple-tool:,apple:,codesign:`. Without
  it macOS wants a click before letting `codesign` use the key, and over ssh
  that is simply a failure.

## Setting up a builder from scratch

On a machine that holds the identity in its login keychain:

```sh
contrib/macos-builder-setup m1
```

Idempotent: every step checks first, so re-running it repairs whatever is
missing and touches nothing else. It installs zig, clones the repo, exports the
signing identity, creates the keychain, stores the notary credential, then
proves the machine can both **build and sign** before it claims success.

It needs `~/.config/slosh/notary.env` on the machine you run it from:

```sh
NOTARY_KEY=/Users/you/.config/slosh/notary/AuthKey_XXXXXXXXXX.p8
NOTARY_KEY_ID=XXXXXXXXXX
NOTARY_ISSUER=<issuer-uuid from App Store Connect>
```

That file, and not the script, is where the credential lives, which is why
the scripts contain no secrets and are in the repo.

### How the private key is moved

`security export` hands over **every** identity in the login keychain, which on
a laptop also means localhost dev certs and whatever Compressor once installed.
The setup script exports the bundle, then picks out the one Developer ID,
pairing certificate to key by `localKeyID`, because the friendly name on a key
bag is the *person*, not the certificate, and name matching alone picks the
wrong key on a machine with more than one identity. It rebuilds a `.p12`
holding that key, its certificate and the Apple chain above it, ships that, and
deletes it on both ends. Nothing else leaves the laptop.

## When it breaks

**`errSecInternalComponent` from codesign.** Almost always the same identity in
**more than one keychain in the search list**. The error names nothing and
blames nothing; `make -f Makefile.macos builder-check` detects it and says so.

```sh
ssh $BUILDER security find-identity -v -p codesigning   # the same hash twice?
ssh $BUILDER security list-keychains -d user            # who else is in the list
ssh $BUILDER security delete-keychain ~/Library/Keychains/<the extra one>-db
```

**`User interaction is not allowed`.** The keychain is locked and the `unlock`
target did not run, or the password file is gone. Check
`~/.config/slosh/build-keychain.pass` exists and is not empty.

**Notarization says `Invalid`.** `Makefile.macos` fails the build and prints
Apple's log for the submission id rather than shipping it. `make -f
Makefile.macos notary-status` on the builder lists recent submissions.

**`spctl --assess --type exec` says "does not seem to be an app".** That is not
a failure: it is what Gatekeeper says about every bare CLI binary ever signed,
because `--type exec` only understands app bundles. The check that means
something, and the one `contrib/macos-remote-build` runs on the builder for
every artifact, is the one a downloaded file gets:

```sh
xattr -w com.apple.quarantine "0083;00000000;Safari;" slosh
spctl -a -t open --context context:primary-signature -vv slosh
# accepted / source=Notarized Developer ID
```

That path looks Apple's ticket up online, so "Notarized Developer ID" means a
stranger's mac will run it, not merely that we submitted something.

## What is deliberately not here

* **A stapled `.pkg`.** `make -f Makefile.macos pkg` needs a *Developer ID
  Installer* certificate, which is a second cert separate from the Application
  one. The zip is the Homebrew-friendly path anyway; a bare binary and a `.zip`
  cannot be stapled, so Gatekeeper verifies them online on first run.
* **A CI trigger.** The builder holds a signing key; it runs when you ask it
  to, from a machine you are sitting at, and not on a push hook.
