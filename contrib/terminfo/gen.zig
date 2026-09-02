//! Print the vendored libghostty-vt's terminfo source to stdout.
//! Run at re-vendor time (see README.md here); the output is committed.
const std = @import("std");
const terminfo = @import("terminfo");

pub fn main(init: std.process.Init) !void {
    var buf: [64 * 1024]u8 = undefined;
    var w = std.Io.File.stdout().writerStreaming(init.io, &buf);
    try terminfo.ghostty.encode(&w.interface);
    try w.interface.flush();
}
