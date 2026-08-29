#!/usr/bin/env python3
"""Kitty graphics passthrough.

libghostty-vt parses the protocol and tracks the images; what a multiplexer
has to add is re-emitting them to the client at the right place, with ids that
cannot collide between panes. That collision is the whole reason tmux and
zellij drop images instead, and it is the first thing tested here.
"""

import base64
import struct
import sys
import zlib

from harness import Session, check, report

# a 4x2 RGB image
PX = base64.b64encode(
    bytes([255, 0, 0, 0, 255, 0, 0, 0, 255, 255, 255, 0] * 2)
).decode()


def png(w=4, h=2, color_type=2):
    """A minimal PNG, built here so the suite needs no binary fixtures.

    `color_type` 2 is RGB and 6 is RGBA; both are worth sending, because the
    decoder is asked to normalise whatever the file is into RGBA.
    """

    def chunk(tag, body):
        c = tag + body
        return struct.pack(">I", len(body)) + c + struct.pack(">I", zlib.crc32(c))

    px = [255, 0, 0] if color_type == 2 else [255, 0, 0, 255]
    raw = b"".join(b"\x00" + bytes(px * w) for _ in range(h))
    return (
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, color_type, 0, 0, 0))
        + chunk(b"IDAT", zlib.compress(raw))
        + chunk(b"IEND", b"")
    )


def sends_image(image_id=7, cols=6, rows=2, after="sleep 5"):
    seq = f"\\033_Ga=T,f=24,s=4,v=2,i={image_id},q=2,c={cols},r={rows};{PX}\\033\\\\"
    return ["/bin/sh", "-c", f'stty raw -echo; printf "{seq}"; {after}']


def places(s):
    return s.api("graphics")["placements"]


def transmits_then_places(place_keys, before_place="", image_id=7):
    """The other half of the protocol: upload once with `a=t`, place later
    with `a=p`. Everything a program that draws repeatedly does."""
    t = f"\\033_Ga=t,f=24,s=4,v=2,i={image_id},q=2;{PX}\\033\\\\"
    p = f"\\033_Ga={place_keys}\\033\\\\"
    return [
        "/bin/sh",
        "-c",
        f'stty raw -echo; printf "{t}"; printf "{before_place}"; printf "{p}"; sleep 5',
    ]


def test_a_pane_image_reaches_the_screen():
    with Session(sends_image(), cols=44, rows=10) as s:
        s.settle(200)
        p = s.pane()
        pl = places(s)
        check("the image is placed", len(pl) == 1, str(pl))
        if not pl:
            return
        check(
            "at the pane's content origin, in screen cells",
            (pl[0]["x"], pl[0]["y"]) == (p["content_x"], p["content_y"]),
            f"{pl[0]} vs content {p['content_x']},{p['content_y']}",
        )
        check(
            "with the size the program asked for",
            (pl[0]["cols"], pl[0]["rows"]) == (6, 2),
            str(pl[0]),
        )


def test_a_placement_that_does_not_say_how_big_it_is():
    """`c=`/`r=` are optional: without them the image covers as many cells as
    its pixels need, which the terminal can only work out if it knows how big
    a cell is. We were passing 0 -- so every such image covered no cells and
    silently did not appear, which is most of what "kitty graphics works"
    was worth."""
    with Session(transmits_then_places("p,i=7,p=1,q=2"), cols=44, rows=10) as s:
        s.settle(200)
        pl = places(s)
        check("it is placed", len(pl) == 1, str(pl))
        if not pl:
            return
        # 4x2 pixels at the default 8x16 cell: one cell each way.
        check(
            "sized from its pixels and the cell size",
            (pl[0]["cols"], pl[0]["rows"]) == (1, 1),
            str(pl[0]),
        )


def test_the_cell_size_a_client_reports_is_what_sizes_an_image():
    with Session(transmits_then_places("p,i=7,p=1,q=2"), cols=44, rows=10) as s:
        s.settle(200)
        s.api("resize", cols=44, rows=10, cell_w=2, cell_h=1)
        s.settle(150)
        pl = places(s)
        # The same 4x2 image against a 2x1 cell is 2 cells wide and 2 tall.
        check(
            "a different cell means a different number of cells",
            pl and (pl[0]["cols"], pl[0]["rows"]) == (2, 2),
            str(pl),
        )


def test_a_program_can_read_the_pixel_size_from_its_pty():
    """TIOCGWINSZ has pixel fields, and a program that draws images reads
    them. Zeroes there are why dvd.py fell back to guessing 8x16."""
    prog = (
        "import fcntl, termios, struct, sys, time;"
        "r, c, xp, yp = struct.unpack('HHHH', "
        "fcntl.ioctl(0, termios.TIOCGWINSZ, b'\\0' * 8));"
        "print('WS', c, r, xp, yp); sys.stdout.flush(); time.sleep(5)"
    )
    with Session(["python3", "-c", prog], cols=50, rows=10) as s:
        snap = s.until_text("WS ")
        line = [l for l in snap.text if "WS " in l][0].strip()
        cols, rows, xp, yp = (int(v) for v in line.split()[1:5])
        check("the pty carries pixel dimensions", xp and yp, line)
        check(
            "consistent with the cell size we were told",
            xp == cols * 8 and yp == rows * 16,
            line,
        )


def asks_its_size():
    """A child that asks the terminal how big it is (XTWINOPS) and prints what
    came back, the way ida-tui's splash does before it draws.

    It waits for a keystroke first so the test can change the cell size before
    the question is asked -- the answer is the point, and a sleep would only be
    a guess about when the resize landed.
    """
    prog = (
        "import os, re, select, sys, termios, time, tty\n"
        "fd = os.open('/dev/tty', os.O_RDWR | os.O_NOCTTY)\n"
        "old = termios.tcgetattr(fd); tty.setraw(fd)\n"
        "sys.stdout.write('READY\\r\\n'); sys.stdout.flush()\n"
        "os.read(fd, 1)\n"
        # 14t: text area in pixels, 16t: cell in pixels, 18t: area in cells.
        # DA1 last, because everything answers it: it marks the end of the
        # replies so nothing has to be timed.
        "os.write(fd, b'\\033[14t\\033[16t\\033[18t\\033[c')\n"
        "buf = b''\n"
        "deadline = time.monotonic() + 2\n"
        "while time.monotonic() < deadline:\n"
        "    r, _, _ = select.select([fd], [], [], 0.1)\n"
        "    if not r: continue\n"
        "    buf += os.read(fd, 4096)\n"
        "    if re.search(rb'\\033\\[\\?[0-9;]*c', buf): break\n"
        "termios.tcsetattr(fd, termios.TCSANOW, old)\n"
        "for m in re.finditer(rb'\\033\\[([0-9;]+)t', buf):\n"
        "    sys.stdout.write('R' + m.group(1).decode() + '\\r\\n')\n"
        "sys.stdout.write('END\\r\\n'); sys.stdout.flush()\n"
        "time.sleep(5)\n"
    )
    return ["python3", "-c", prog]


def size_replies(s):
    """{leading number: [params]} from the XTWINOPS replies on screen.

    Read from the pane's content area, not the composited screen: the screen
    rows still have the pane border on them.
    """
    out = {}
    snap = s.snapshot()
    for line in snap.pane_text(s.pane()).split("\n"):
        line = line.strip()
        if not line.startswith("R"):
            continue
        parts = line[1:].split(";")
        if all(p.isdigit() for p in parts) and parts:
            out[int(parts[0])] = [int(p) for p in parts[1:]]
    return out


def test_a_program_can_ask_how_big_a_cell_is():
    """XTWINOPS, the other way to ask what the pty's pixel fields carry.

    A program that draws images needs the cell size to keep an aspect ratio,
    and through a multiplexer over ssh the pty fields are often zeroed, so
    this query is what it falls back to. We answered none of it: the query
    timed out and the program guessed 10x20, which against a real 9x22 cell
    is every image stretched by a fifth. ida-tui's splash asks exactly this,
    in the same round trip as its graphics query.
    """
    with Session(asks_its_size(), cols=50, rows=12) as s:
        s.until_text("READY")
        s.send("x")
        s.until_text("END")
        r = size_replies(s)
        check("the cell size query is answered (CSI 16 t)", 6 in r, str(r))
        check("with the cell size we were told", r.get(6) == [16, 8], str(r))
        check("the character size query is answered (CSI 18 t)", 8 in r, str(r))
        check("the pixel size query is answered (CSI 14 t)", 4 in r, str(r))
        if 8 in r and 4 in r:
            rows, cols = r[8]
            check("and the three answers agree", r[4] == [rows * 16, cols * 8], str(r))


def test_the_cell_size_reported_is_the_client_s():
    """Not a constant: whatever the attached terminal said its cell was."""
    with Session(asks_its_size(), cols=50, rows=12) as s:
        s.until_text("READY")
        s.api("resize", cols=50, rows=12, cell_w=9, cell_h=22)
        s.send("x")
        s.until_text("END")
        r = size_replies(s)
        check("a different cell means a different answer", r.get(6) == [22, 9], str(r))


def test_an_image_outlives_a_screen_clear():
    """A program transmits once and places every frame, and full-screen
    programs clear the screen. libghostty-vt freed the image data on ED(2),
    so every placement after the first clear drew nothing -- see
    vendor/patches. The placements go, the image stays."""
    with Session(
        transmits_then_places("p,i=7,p=1,q=2,c=6,r=2", before_place="\\033[2J"),
        cols=44,
        rows=10,
    ) as s:
        s.settle(200)
        check(
            "a clear before the placement does not lose the image",
            len(places(s)) == 1,
            str(places(s)),
        )


def test_a_screen_clear_still_removes_what_is_on_screen():
    argv = [
        "/bin/sh",
        "-c",
        f'stty raw -echo; printf "\\033_Ga=T,f=24,s=4,v=2,i=7,q=2,c=6,r=2;{PX}\\033\\\\"; '
        f'sleep 0.2; printf "\\033[2J"; printf CLEARED; sleep 5',
    ]
    with Session(argv, cols=44, rows=10) as s:
        # Both waits are on something observable: the image arrives, then the
        # clear that follows it. A settle would race the sleep between them.
        s.until(lambda _: len(places(s)) == 1)
        check("placed to begin with", len(places(s)) == 1, str(places(s)))
        s.until_text("CLEARED")
        check(
            "the placement is gone with the screen it was on",
            len(places(s)) == 0,
            str(places(s)),
        )


def test_sub_cell_offsets_survive_to_the_client():
    """A program that moves something smoothly places it part-way into a cell
    with X=/Y=. We tracked the placement and dropped the offsets, so anything
    moving jumped a whole cell at a time -- invisible in a still picture and
    the first thing you see when it moves."""
    with Session(
        transmits_then_places("p,i=7,p=1,q=2,c=6,r=2,X=3,Y=5"), cols=44, rows=10
    ) as s:
        s.settle(200)
        pl = places(s)
        check(
            "the offsets are tracked",
            pl and (pl[0]["x_off"], pl[0]["y_off"]) == (3, 5),
            str(pl),
        )

        # And, separately, that they reach the terminal: the model being right
        # is not the same as the bytes being right.
        raw = s.api("graphics", format="bytes")["bytes"]
        place = [c for c in raw.split("\x1b") if c.startswith("_Ga=p")]
        check(
            "and emitted to the client",
            place and "X=3" in place[0] and "Y=5" in place[0],
            str(place),
        )


def test_a_placement_with_no_offset_emits_none():
    with Session(transmits_then_places("p,i=7,p=1,q=2,c=6,r=2"), cols=44, rows=10) as s:
        s.settle(200)
        raw = s.api("graphics", format="bytes")["bytes"]
        place = [c for c in raw.split("\x1b") if c.startswith("_Ga=p")]
        check(
            "nothing is invented",
            place and "X=" not in place[0] and "Y=" not in place[0],
            str(place),
        )


def test_a_natural_image_is_never_rescaled_as_it_moves():
    """`c=`/`r=` mean *scale into this many cells*. Passing on the count a
    natural-size image happens to cover is invisible in a still picture and
    wrong the moment it moves: the count goes up by one whenever the image
    straddles one more cell boundary, so the picture changes size."""
    with Session(transmits_then_places("p,i=7,p=1,q=2,X=3,Y=5"), cols=44, rows=10) as s:
        s.settle(200)
        raw = s.api("graphics", format="bytes")["bytes"]
        place = [c for c in raw.split("\x1b") if c.startswith("_Ga=p")]
        check(
            "no cell count is sent for a natural placement",
            place and "c=" not in place[0] and "r=" not in place[0],
            str(place),
        )


def test_a_scaled_image_keeps_the_cell_count_it_asked_for():
    with Session(transmits_then_places("p,i=7,p=1,q=2,c=6,r=2"), cols=44, rows=10) as s:
        s.settle(200)
        raw = s.api("graphics", format="bytes")["bytes"]
        place = [c for c in raw.split("\x1b") if c.startswith("_Ga=p")]
        check(
            "what the program asked for is passed on",
            place and "c=6" in place[0] and "r=2" in place[0],
            str(place),
        )


def test_the_graphics_stream_leaves_the_cursor_alone():
    """Placing an image parks the cursor on the target cell, and these bytes
    go out after the cell diff -- so without a save/restore the last thing the
    terminal hears every frame is "go to wherever that image is", and the real
    cursor sits there instead of in the pane you are typing in. Reported as
    "the cursor is not rendered in my shell"."""
    with Session(sends_image(), cols=44, rows=10) as s:
        s.settle(200)
        raw = s.api("graphics", format="bytes")["bytes"]
        check(
            "it moves the cursor at all (or this proves nothing)",
            "H" in raw,
            repr(raw[:80]),
        )
        check("saved first", raw.startswith("\x1b7"), repr(raw[:8]))
        check("and restored last", raw.endswith("\x1b8"), repr(raw[-8:]))


def test_a_frame_with_no_images_emits_nothing():
    """The save/restore must not become a cost every idle frame pays."""
    with Session(["/bin/sh", "-c", "stty raw -echo; cat"], cols=44, rows=10) as s:
        s.settle(150)
        check(
            "not one byte",
            s.api("graphics", format="bytes")["bytes"] == "",
            repr(s.api("graphics", format="bytes")["bytes"]),
        )


def test_ids_cannot_collide_between_panes():
    """Two panes, both using image id 7. They must not become one image."""
    with Session(sends_image(image_id=7), cols=90, rows=12) as s:
        s.settle(200)
        s.key("\\\\")
        s.settle(250)
        pl = places(s)
        check("both panes place an image", len(pl) == 2, str(pl))
        if len(pl) != 2:
            return
        check(
            "and they were given different ids for the client",
            pl[0]["image"] != pl[1]["image"],
            str(pl),
        )
        check("side by side, where their panes are", pl[0]["x"] != pl[1]["x"], str(pl))


def test_placement_follows_the_layout():
    with Session(sends_image(), cols=90, rows=12) as s:
        s.settle(200)
        before = places(s)[0]
        s.key("\\\\")  # the pane with the image is now the left half
        s.settle(250)
        s.api("focus", id=s.panes()[0]["id"])
        s.settle(80)
        after = [q for q in places(s) if q["image"] == before["image"]]
        check(
            "the placement is still tracked after a split", after != [], str(places(s))
        )


def test_cropped_at_the_pane_edge():
    with Session(sends_image(cols=40, rows=8), cols=40, rows=10) as s:
        s.settle(200)
        p = s.pane()
        pl = places(s)
        check("an oversized image is placed", len(pl) == 1, str(pl))
        if not pl:
            return
        check(
            "cropped to the columns the pane has",
            pl[0]["cols"] <= p["content_w"],
            f"{pl[0]} vs {p['content_w']}",
        )
        check(
            "and to its rows",
            pl[0]["rows"] <= p["content_h"],
            f"{pl[0]} vs {p['content_h']}",
        )


def test_hidden_panes_place_nothing():
    with Session(sends_image(), cols=120, rows=30) as s:
        s.settle(200)
        s.key("\\\\")
        s.settle(150)
        s.key("\\\\")
        s.settle(200)
        check("three panes, one with an image", len(places(s)) == 3, str(places(s)))
        s.resize(40, 12)  # collapses
        s.settle(200)
        hidden = [q for q in s.panes() if q["hidden"]]
        check("something collapsed", hidden != [], str(s.panes()))
        check(
            "a collapsed pane places nothing",
            len(places(s)) == len(s.panes()) - len(hidden),
            str(places(s)),
        )


def test_placement_goes_away_with_its_pane():
    with Session(sends_image(), cols=90, rows=12) as s:
        s.settle(200)
        s.key("\\\\")
        s.settle(250)
        check("two placements", len(places(s)) == 2, str(places(s)))
        s.key("x")  # close the focused pane
        s.settle(200)
        check(
            "closing a pane removes its placement", len(places(s)) == 1, str(places(s))
        )


def test_partial_visibility_crops_rather_than_squashes():
    """An image taller than its pane is cropped by moving the source rect.

    Asking the terminal for fewer rows alone would *scale* the image into
    them, which is not what "the rest is off-screen" looks like.
    """
    with Session(sends_image(cols=10, rows=20), cols=44, rows=12) as s:
        s.settle(250)
        p = s.pane()
        pl = places(s)
        check("a taller-than-the-pane image is still placed", len(pl) == 1, str(pl))
        if not pl:
            return
        check(
            "clipped to the pane's rows",
            pl[0]["rows"] <= p["content_h"],
            f"{pl[0]} vs {p['content_h']}",
        )


def sends_png(data, place="c=6,r=2", image_id=7):
    """Upload with `a=t,f=100` and place it: what a program with a picture on
    disk does, and what ida-tui's splash does.

    The child is python rather than sh because `printf` mangles adjacent
    backslash escapes -- `\\033\\\\` followed by `\\033` arrives as a literal
    `\\033` -- so a shell-built sequence does not reliably reach the pty as the
    bytes you wrote. That cost an afternoon and a wrong diagnosis.
    """
    b64 = base64.b64encode(data).decode()
    prog = (
        "import sys, time\n"
        f"sys.stdout.write('\\033_Ga=t,f=100,t=d,i={image_id},q=2;{b64}\\033\\\\')\n"
        f"sys.stdout.write('\\033_Ga=p,i={image_id},p=1,{place},q=2\\033\\\\')\n"
        "sys.stdout.write('DONE')\n"
        "sys.stdout.flush()\n"
        "time.sleep(5)\n"
    )
    return ["python3", "-c", prog]


def test_a_png_is_decoded_and_placed():
    """libghostty-vt ships no image codec: built as a library its `decode_png`
    hook is null, and a null hook rejects every `f=100` transmission outright.
    No image was stored, so no placement existed and there was nothing to
    re-emit -- a program that uploaded a PNG drew nothing at all, silently,
    while raw RGB worked. Found because ida-tui's startup logo never appeared."""
    for color_type, what in ((2, "RGB"), (6, "RGBA")):
        with Session(sends_png(png(color_type=color_type)), cols=44, rows=10) as s:
            s.until_text("DONE")
            s.settle(200)
            pl = places(s)
            check(f"a {what} png is placed", len(pl) == 1, str(pl))
            if pl:
                check(
                    "at the size it asked for",
                    (pl[0]["cols"], pl[0]["rows"]) == (6, 2),
                    str(pl[0]),
                )


def test_a_decoded_png_reaches_the_client_as_raw_pixels():
    """The client is told `f=32`: decoding is the point, and the terminal we
    are re-emitting to should not have to decode it a second time."""
    with Session(sends_png(png()), cols=44, rows=10) as s:
        s.until_text("DONE")
        s.settle(200)
        raw = s.api("graphics", format="bytes")["bytes"]
        tx = [c for c in raw.split("\x1b") if c.startswith("_Ga=t")]
        check("the image is transmitted to the client", tx != [], repr(raw[:120]))
        if tx:
            check(
                "as RGBA, not as the png we were given",
                "f=32" in tx[0] and "f=100" not in tx[0],
                tx[0][:80],
            )


def test_a_png_that_is_not_one_places_nothing():
    """The bytes come off a pane's pty, so the decoder is fed whatever a
    program feels like sending. Refusing it is all that is asked; the pane
    must still be there afterwards."""
    junk = b"\x89PNG\r\n\x1a\n" + b"\xde\xad\xbe\xef" * 32
    with Session(sends_png(junk), cols=44, rows=10) as s:
        s.until_text("DONE")
        s.settle(200)
        check("nothing is placed", places(s) == [], str(places(s)))
        check("and the session survived it", s.pane() is not None)


def test_an_undelivered_frame_stays_owed():
    """A frame's graphics bytes can fail to reach the client -- the server's
    outbox has a ceiling, and one screenshot retransmission is megabytes of
    base64. The model used to advance anyway: the image was marked sent (so it
    was never transmitted again) and the deletions went out with the dropped
    bytes (so they were never said again). The visible half of that bug was a
    screenshot scrolled out of a pi transcript staying parked on the screen,
    at a fixed position, immune to scrolling and pane swaps, forever.

    A flush nobody commits counts as undelivered, which is also what the
    `graphics` control command is: bytes rendered for a CLI, not the client.
    So the contract is observable right here: everything stateful about a
    frame -- transmissions and deletions -- must show up again in the next
    one, until a delivery is confirmed."""
    prog = (
        "stty raw -echo; "
        f'printf "\\033_Ga=T,f=24,s=4,v=2,i=7,q=2,c=6,r=2;{PX}\\033\\\\"; '
        "head -c1 >/dev/null; seq 1 200; cat"
    )
    with Session(["/bin/sh", "-c", prog], cols=44, rows=10) as s:
        s.until(lambda _: len(places(s)) == 1)

        raw = s.api("graphics", format="bytes")["bytes"]
        check("the image is transmitted", "_Ga=t" in raw, repr(raw[:80]))
        raw = s.api("graphics", format="bytes")["bytes"]
        check(
            "and transmitted again: the last frame was never delivered",
            "_Ga=t" in raw,
            repr(raw[:80]),
        )

        s.send("x")  # the seq scrolls the image out of the viewport
        s.until(lambda _: places(s) == [])
        raw = s.api("graphics", format="bytes")["bytes"]
        check("scrolling it away owes a deletion", "_Ga=d,d=i" in raw, repr(raw))
        raw = s.api("graphics", format="bytes")["bytes"]
        check(
            "which is still owed next frame, not fire-and-forget",
            "_Ga=d,d=i" in raw,
            repr(raw),
        )


def test_scrolled_away_placements_are_dropped():
    prog = (
        "stty raw -echo; "
        f'printf "\\033_Ga=T,f=24,s=4,v=2,i=7,q=2,c=6,r=2;{PX}\\033\\\\"; '
        "seq 1 200; cat"
    )
    with Session(["/bin/sh", "-c", prog], cols=44, rows=10) as s:
        s.settle(400)
        check(
            "an image scrolled out of the viewport is not placed",
            places(s) == [],
            str(places(s)),
        )


def test_a_float_occludes_a_placement():
    """The cell compositor gets occlusion free from paint order; placements
    are sent after the diff and get it from clipping (D22): a clean edge
    crops -- the pane-edge arithmetic aimed at another clipper -- and a
    float in the middle is a shape one placement cannot express, so that
    placement is suppressed for the frame and returns when the float moves.

    Every pane here runs the same command, so the float shows the image too;
    the tiled pane's placement is told apart by the image id it had before
    the split."""
    with Session(sends_image(cols=60, rows=6, after="cat"), cols=100, rows=28) as s:
        s.settle(120)
        base = places(s)
        check("one placement to start", len(base) == 1, str(base))
        if not base:
            return
        img = base[0]
        mine = lambda: [p for p in places(s) if p["image"] == img["image"]]

        s.api("split", dir="rows")
        s.settle(40)

        # A float over the image's right side: cropped at the float's edge.
        s.api("float", x=img["x"] + 20, y=0, w=60, h=28)
        s.settle(20)
        got = mine()
        check(
            "a clean edge crops the placement",
            len(got) == 1 and got[0]["cols"] == 20 and got[0]["x"] == img["x"],
            str(got),
        )

        # Across the middle: no single rect can say what remains.
        s.api("float", x=img["x"] + 10, y=0, w=30, h=28)
        s.settle(20)
        check("a middle strip suppresses it", mine() == [], str(places(s)))

        # Moved clear: the placement returns whole.
        s.api("float", x=img["x"], y=14, w=30, h=12)
        s.settle(20)
        got = mine()
        check(
            "and it returns whole when the float moves off",
            len(got) == 1 and got[0]["cols"] == img["cols"],
            str(got),
        )

        # Over the top rows: cropped from the top, the source origin moving.
        s.api("float", x=0, y=0, w=100, h=img["y"] + 3)
        s.settle(20)
        got = mine()
        check(
            "a top cover crops from the top",
            len(got) == 1 and got[0]["y"] > img["y"] and got[0]["rows"] < img["rows"],
            str(got),
        )


if __name__ == "__main__":
    test_a_pane_image_reaches_the_screen()
    test_a_png_is_decoded_and_placed()
    test_a_decoded_png_reaches_the_client_as_raw_pixels()
    test_a_png_that_is_not_one_places_nothing()
    test_a_placement_that_does_not_say_how_big_it_is()
    test_the_cell_size_a_client_reports_is_what_sizes_an_image()
    test_a_program_can_read_the_pixel_size_from_its_pty()
    test_a_program_can_ask_how_big_a_cell_is()
    test_the_cell_size_reported_is_the_client_s()
    test_an_image_outlives_a_screen_clear()
    test_a_screen_clear_still_removes_what_is_on_screen()
    test_sub_cell_offsets_survive_to_the_client()
    test_a_placement_with_no_offset_emits_none()
    test_a_natural_image_is_never_rescaled_as_it_moves()
    test_a_scaled_image_keeps_the_cell_count_it_asked_for()
    test_the_graphics_stream_leaves_the_cursor_alone()
    test_a_frame_with_no_images_emits_nothing()
    test_ids_cannot_collide_between_panes()
    test_placement_follows_the_layout()
    test_cropped_at_the_pane_edge()
    test_partial_visibility_crops_rather_than_squashes()
    test_hidden_panes_place_nothing()
    test_placement_goes_away_with_its_pane()
    test_scrolled_away_placements_are_dropped()
    test_an_undelivered_frame_stays_owed()
    test_a_float_occludes_a_placement()
    sys.exit(report())
