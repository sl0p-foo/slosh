# slosh — Homebrew formula (a *formula*, not a cask: casks are for prebuilt
# .app/.pkg/.dmg drops, formulae are for things brew builds and puts in bin).
#
# This file is the source of truth. `contrib/brew-release` rewrites the three
# stanzas below (url/version/sha256) for a new commit or tag and copies the
# result into the tap repo, which is what users actually tap:
#
#     brew tap sl0p-foo/slosh
#     brew install slosh
#
# Try it without publishing anything:
#
#     brew install --build-from-source contrib/homebrew/slosh.rb
class Slosh < Formula
  desc "Terminal multiplexer with detachable sessions and programmable pane chrome"
  homepage "https://sl0p.foo"
  # GitHub's archive of a commit is deterministic per ref: same ref, same
  # bytes, so the sha256 below stays honest. Pinned to a full commit id rather
  # than a branch, because a branch tarball changes under you and brew would
  # keep the stale one.
  url "https://github.com/sl0p-foo/slosh/archive/5987a7f7adfcde60d7757ad858c7435fdaf479e0.tar.gz"
  version "0.1.2"
  sha256 "a5fc5ee33d8b6813a98b597f241e48d8d424c2f83bd93f30b4abbf1477573fb8"
  head "https://github.com/sl0p-foo/slosh.git", branch: "master"

  # zig is the whole toolchain: it compiles the C, builds the vendored
  # libghostty-vt, and links it statically. Nothing is needed at runtime, which
  # is why there is no `depends_on` without `=> :build`.
  depends_on "zig" => :build

  def install
    # zig caches in ~/.cache/zig and ~/.zig-cache by default. Under brew that is
    # either a sandbox denial or pollution of the user's real cache, so both
    # caches go into the build directory and die with it.
    ENV["ZIG_GLOBAL_CACHE_DIR"] = buildpath/".zig-global-cache"
    ENV["ZIG_LOCAL_CACHE_DIR"] = buildpath/".zig-local-cache"

    args = ["ZIG=#{formula_opt_bin("zig")}/zig"]
    # In a snapshot tarball there is no .git, so the Makefile's `git describe`
    # would bake in "unknown" and `slosh --version` could not tell you which
    # build you are looking at. Hand it the commit the tarball came from.
    args << "GITDESC=#{version.to_s.split("-").last}" unless build.head?

    system "make", "vendor", *args
    system "make", "all", *args

    # Deliberately not `make install`: the formula must be able to build any
    # commit, including ones older than that target, and the layout is three
    # lines here. `make install PREFIX=...` exists for everyone not using brew
    # and puts the same files in the same places.
    bin.install "build/slosh"
    # Guarded because the formula must be able to build any commit, including
    # ones older than the manpages.
    man1.install "docs/slosh.1" if File.exist?("docs/slosh.1")
    man5.install "docs/slosh.5" if File.exist?("docs/slosh.5")
    pkgshare.install "config/config.kdl", "config/example.layout"
    %w[themes chrome shaders].each do |d|
      pkgshare.install Dir["contrib/#{d}"] if File.directory?("contrib/#{d}")
    end
    doc.install "README.md"
    doc.install Dir["docs/*.md"]
  end

  def caveats
    <<~EOS
      Example configuration, themes, chrome and shaders are installed in:
        #{opt_share}/slosh

      Start from the shipped config:
        mkdir -p ~/.config/slosh && cp #{opt_share}/slosh/config.kdl ~/.config/slosh/

      Then: slosh          (attach to session "main", creating it if needed)
            C-a ?          (the keys)   C-a p   (every command by name)
    EOS
  end

  test do
    # The binary knows its own version, and it is the version brew installed.
    assert_match "slosh 0.1.0", shell_output("#{bin}/slosh --version")

    # The configuration we ship must parse with the binary we ship.
    assert_match "ok", shell_output("#{bin}/slosh --check #{share}/slosh/config.kdl")

    # And it composes a real screen headlessly: no tty, no session, no socket.
    # This exercises the vendored terminal core, the layout and the renderer.
    screen = shell_output("#{bin}/slosh --headless --cols 40 --rows 12 -- " \
                          "/bin/sh -c 'echo hello-brew'")
    assert_match "hello-brew", screen
    assert_match "pane 1/1", screen
  end
end
