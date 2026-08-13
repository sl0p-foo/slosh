ok listen I've been playing with some things but I don't like any of it.
what we're trying to do is hellbending existing terminal multiplexers (in this case zellij) to do what I we want for the sl0ppi project.
it is (slowly) starting to work but the process is very tedious, for a big part because rustlang is so sluggish with build times, iterating takes time.
anyway, lets MVP a basic opinionated terminal multplexer, not from scratch of course. we stand on the shoulders of mitchell hashimoto (OF HASHICORP FAME),
libghostty-vt should be what we need for the terminal heavy lifting. 

since rust sucks and typescript is for hipsters and lighostty-vt has C bindings and C is badass we'll be doing this entire
thing in C. C is also a good fit because it gives us easy access to lowlevel I/O things we want for keyboard/mouse/output etc.

you should look at ~/dev/sl0ppi to see the direction w ecurrently heading in with our zellij fork .. i think you might be able to distill some requirements
from that.

let's riff a bit on this shit before we do any work brother, let's get on the same page
