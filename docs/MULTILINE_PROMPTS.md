# Multiline Prompt Editing

[README](../README.md) | [Testing](TESTING.md)

Record of the interactive prompt work on this branch: `0134c70` (enable
multiline editing), `98497a4` (writes larger than 1024 bytes), `c68db89`
(scrolling fixes), `7f0b9c6` (the layout model below), `0895af3` (prompt echo),
`d83d271` (window resize), and this handover file. It is written as a handoff:
the design will be re-done as
one cohesive renderer, and the point of this file is that the reasoning behind
each current behaviour does not have to be rediscovered. The observation tools
are in appendices A to D, in full, because they lived in `/tmp` and are gone
with the machine that held them.

## The UI being rendered

Three things share one terminal screen and none of them owns it:

* Streaming model output, appended line by line, which must scroll into the
  terminal's own scrollback so the user can scroll up through a long answer.
* A prompt block that grows and shrinks as the user adds or removes lines
  (Ctrl+J inserts a newline, so the block can be one row or ten).
* A status footer (`ctx 1.3k/200k | idle`) pinned to the last row, repainted
  while output streams and while the user types.

The implementation reserves rows at the bottom of the screen with a DECSTBM
scroll region for output (`1..rows-reserved`) and lets linenoise draw the prompt
block inside the reservation. Model output is written by the agent
(`editor_write_scroll_output_preserve_prompt()` in `ds4_agent.c`), the prompt is
drawn by linenoise, and the two coordinate through
`linenoiseSetLayoutCallback()` -> `editor_set_scroll_layout()`.

## Layout model as implemented ("option C")

* **Sticky reservation.** `ed->reserved_rows` never shrinks while a prompt is
  active. A block that gives rows back leaves them inside the prompt area rather
  than handing them to the output region. This is what keeps the transcript from
  moving while the user edits.
* **Footer pinned to the last row.** `status_row = rows - status_rows + 1`,
  always the physical last row; `linenoiseRefreshStatus()` paints it on
  `l->status_row` (falling back to `last_input_row + 1` when unpinned).
* **Output never moves.** The transcript's rows are stable while editing; only
  real output scrolls them. When the region shrinks because the prompt grew, the
  region is scrolled up by the difference instead of being cleared, so the lines
  that had to leave go to scrollback.
* **Saved output cursor.** The output position is kept in the terminal's DECSC
  slot (`\x1b7`/`\x1b8`) plus `ed->output_anchor_row`, the row the next output
  write starts on. `editor_set_scroll_layout()` re-saves it absolutely.
  Growing the region keeps the anchor on the row where the last line actually is
  (`same_size && output_bottom > prev_output_bottom && anchor <= output_bottom`)
  instead of snapping it to the new bottom, which is what makes a released block
  leave no blank band.
* **One scroll path for both shrink directions** (`editor_set_scroll_layout()`):
  a prompt that grows and a terminal that got shorter are handled by the same
  `editor_scroll_output_up(prev_output_last, prev_output_last - output_bottom)`,
  with `prev_output_last` clamped to the new screen height.

`editor_stop()` clears `reserved_rows` and `output_anchor_row` **without** a
relayout. Relaying out there moves the cursor onto the row the next prompt is
drawn on and erases the echo and any `/help` output printed with plain `printf`.

## Bugs found, and what they actually were

Every one of these was found by dragging a real terminal or replaying the byte
stream in pyte; none was visible from the code alone.

1. **Unpinned footer painted on the prompt row.** `linenoiseRefreshStatus()`
   used the last input row instead of the pinned status row. Fixed by preferring
   `l->status_row`.
2. **Model output destroyed by the erase walk.** The clean/hide path anchored on
   the cursor with a relative move and could land inside the output region.
   Fixed with `linenoiseGotoCleanBottom()`: an absolute CUP when
   `screen_cursor_row > 0`, the historic relative move otherwise.
3. **Box not collapsing after submit.** Setting `reserved_rows = 0` with a
   relayout in `editor_stop()` erased the echo. Fixed by clearing the
   reservation without relaying out.
4. **Gap between transcript and the accepted prompt echo.** Two separate causes:
   (a) the accepted prompt was printed with bare `printf` at whatever row the
   teardown left the cursor on, several rows below the transcript; (b) on region
   growth the layout re-saved the output cursor at the new bottom row, which is
   empty, so the first streamed token started there. Fixed by writing the echo
   through `editor_write_scroll_output_preserve_prompt()` (like queued
   messages, which already did) and by honouring `output_anchor_row` on growth.
   `/help`-style output still uses plain `printf`, so `editor_stop()` invalidates
   the anchor and the release stays bottom-anchored (HEAD behaviour).
5. **Vertical shrink destroyed the last lines of output** (`d83d271`). Dragging
   the bottom border up left the footer in place but deleted the lines above it.
   Two destroys, both only reachable after the terminal loses rows:
   * linenoise's erase of the previous block walked *upward* from the block's
     bottom row. The shrink clamps that start address to the new last row, so
     the walk leaves the reserved area and erases transcript rows. Fixed by
     `linenoiseCleanBlockUp()`: clear each row through an absolute address when
     the bottom row is known, so out-of-range addresses clamp to the last row
     instead of climbing into the output.
   * `editor_set_scroll_layout()` cleared `prompt_row..rows`, which after a
     shrink hold transcript lines. Fixed by scrolling the overlap into scrollback
     first, in the same code path that handles prompt growth.

## Terminal facts worth keeping

* Lines scrolled out of the **top of a DECSTBM region** are retained in the
  terminal's scrollback (verified in Ghostty/Zed and in `pyte.HistoryScreen`),
  which is why "scroll the region up" is an acceptable way to dispose of rows.
* DECSC/DECRC is a single saved-cursor slot, and it is a **coordinate**: a resize
  does not move it with the content. After a resize, re-anchor with an absolute
  CUP rather than trusting `\x1b8`.
* Terminals differ when the window shrinks: some keep the top row fixed and
  destroy the bottom rows, others keep the cursor row visible and scroll the
  content up. The app cannot tell which it got. The CPR query (`\x1b[6n`) does
  not disambiguate either, because the cursor sits one row above the footer, so
  a clamped cursor and a scrolled one report the same row. The current choice is
  to always compensate, which preserves output on top-anchored terminals and, on
  cursor-anchored ones, costs a few blank rows plus a lagging copy of the erased
  prompt block that scroll out as more output arrives.
* There is **no SIGWINCH handler**. The size is read lazily through
  `editor_get_terminal_size()` (TIOCGWINSZ on stdout) during the next refresh,
  so the terminal has already resized itself by the time the app reacts.
* pyte does not erase on DECSTBM, and `pyte.Screen` exposes no saved-cursor
  attribute; harnesses that emulate a resize must re-emit `\x1b7` themselves.

## Remaining flaws and design notes for the rework

* Two owners of one screen (agent output vs linenoise prompt) with a callback in
  between. The callback fires *after* linenoise has emitted its erase bytes, so
  every ordering hazard has to be neutralised inside the erase itself.
* The output bookkeeping mixes "row of the last written line" with "row the next
  write starts on" (`output_anchor_row`, `output_at_scroll_boundary`,
  `output_bottom`). A redesign should track one of them. A tighter shrink/grow
  scroll is possible if the renderer knows where the last line physically is;
  today the scroll is deliberately conservative, so a shrink can scroll one or
  two rows more than strictly needed.
* Relative cursor movement is still used in places (`editor_move_to_output_cursor`
  uses `\x1b[1A` + `\x1b[nG`). Absolute addressing is what survived resizes.
* Plain `printf` paths (slash commands, startup lines) are outside the layout and
  force the anchor to be invalidated; anything printed that way resets the
  invariant instead of being accounted for.
* A single renderer owning a row map - output region, prompt block, footer - and
  emitting only absolute addressing would remove most of this: sticky
  reservation, anchor re-saving, the two erase anchors, and the resize
  compensation all exist to keep two independent renderers from corrupting each
  other's rows.
* The cursor-anchored-terminal compensation could be made exact if the renderer
  knew the terminal's resize model (a config option would be enough).

## How this work was verified

Two stdlib Python tools do the observing; they are reproduced in the appendices
of this file so they survive a lost `/tmp`. `ptyrec.py` runs any program on a
pty of a chosen size, records the raw bytes, and can shrink the window
partway through, injecting a marker byte sequence because a resize is invisible
to the stream. `termplay.py` replays such a capture through a hand-written VT
model (CUP/CUU/CUD/CUF/CUB/HA, CR/LF/BS/TAB, IL/DL, EL/ED, DECSTBM, DECSC/DECRC,
reverse index, DECAWM, SGR background, sync markers ignored), applies the
synthetic window shrink at the marker with a chosen model, and reports per
phase: last text row, transcript range, numbering gaps, blank rows inside the
transcript, prompt and footer rows, the saved output cursor, and the cursor
position. `--pyte` additionally feeds the same bytes to pyte and compares the
text grids: it reported **zero mismatches on every phase** of the echo capture,
of the resize capture under both shrink models, and of a synthetic pty capture,
so the model can be trusted when pyte is not installed.

```sh
python3 ptyrec.py --rows 24 --cols 80 --resize-rows 20 --resize-after 0.8 \
    --out /tmp/cap.bin -- /bin/sh /tmp/scenario.sh
python3 termplay.py /tmp/cap.bin --rows 24 --resize-rows 20 --resize-model top --pyte
```

`--resize-model top` is the terminal that keeps the first row fixed and destroys
the bottom rows; `--resize-model cursor` is the one that keeps the cursor row
visible and scrolls the content up. Run both: a fix is only good if neither one
loses transcript lines.

**Reproducing a reported terminal bug**, in the order that worked:

1. Capture the real behaviour: `ptyrec.py` around the agent, or `agentprobe.c`
   (appendix D) when the bug is inside the editor functions and there is no way
   to reach it from the keyboard.
2. Replay it twice, `--resize-model top` and `--resize-model cursor`, and read
   `gaps` and `holes`. *Gap* is a missing number in the transcript sequence,
   which means a line was destroyed rather than scrolled; *hole* is a blank row
   inside the transcript. Both are bugs; a gap is the worse one because the text
   is not even in scrollback.
3. Build the same probe against the previous commit and replay the same capture,
   so the claim is a difference and not an impression:

   ```sh
   mkdir -p /tmp/prev
   git show 0895af3:ds4_agent.c > /tmp/prev/ds4_agent.c
   git show 0895af3:linenoise.c > /tmp/prev/linenoise.c
   git show 0895af3:linenoise.h > /tmp/prev/linenoise.h
   cc -O3 -g -march=native -std=c99 -D_GNU_SOURCE -c -o /tmp/prev/linenoise.o /tmp/prev/linenoise.c
   # compile the probe with -DAGENT_SRC=/tmp/prev/ds4_agent.c -I/tmp/prev -I/home/ds4
   # and link /tmp/prev/linenoise.o in place of the working tree's linenoise.o
   ```

4. Read the raw bytes of the phase that changed. More than once the screen state
   looked explainable but the byte stream named the culprit immediately:
   `python3 -c "d=open('/tmp/p.bin','rb').read(); print(d.replace(b'\x1b',b'<ESC>').decode('latin-1'))"`.

The remaining harnesses were throwaway C probes in `/tmp` that drive the real
`editor_start()` / output / `editor_stop()` / echo / stream cycle against a pty
(`openpty`, `LINENOISE_ASSUME_TTY=1`, `linenoiseSetMultilineEdits(1)` - without
that call Ctrl+J does nothing and the box never grows), dump the byte stream,
and replay it in `pyte` (`/tmp/py_pkgs`, latin-1 decode, `HistoryScreen` for
scrollback). Phase boundaries are `\x1b[?9001h`, a synthetic resize marker is
`\x1b[?9002h`. None of them are in the repo; rebuilding one is

```sh
cc -O1 -g -D_GNU_SOURCE -Wno-unused-function -DDS4_NO_GPU \
   -DAGENT_SRC=/home/ds4/ds4_agent.c -I/home/ds4 -c -o probe.o probe.c
cc -O1 -g -o probe probe.o <agent objects> -lm -pthread -lutil
```

Final state of the checks, all passing at `d83d271`:

* 240-key random-edit probe over 24x80, 41x164, 24x60, 12x40 - 0 invariant
  violations (prompt/footer never overlap output, output rows never rewritten).
* Paste-shrink and Ctrl+J-shrink probes over 24x80, 41x164, 12x40 - 0 violations,
  no transcript holes.
* 23-check edit test - all ok.
* Layout probe: grow 16/17/24, shrink 16/17/24, submit 22/23/24, resize 18/19/20
  (`output_bottom`/`prompt_row`/`footer_row`).
* Echo/stream probe on 24x80 with a 3-line box: transcript to row 19, block
  21..23, anchor 20; echo lands on 18 directly under the transcript; streaming
  continues contiguous after the release.
* Resize probe 24->20 replayed under both shrink models: committed `0895af3`
  erased `out 22..24` and left a permanent numbering gap; after the fix the
  transcript is contiguous through the newest line. A 24->16 shrink loses only
  the rows the terminal itself cut.
* `ds4_agent_test` ok; `make cpu`
  exits 0 with the one pre-existing `-Wformat-truncation` warning;
  ASan+UBSan clean on the edit, shrink, echo and resize probes.

Build rules that mattered while debugging: never build in `/home/ds4` (use
`/tmp/ds4build`), and always recompile `linenoise.o` after editing
`linenoise.c` - a stale object once hid a correct fix.

## Source landmarks

Names rather than line numbers, because the line numbers moved four times in
one day. In `ds4_agent.c`: `agent_editor` (fields `output_bottom`, `prompt_row`,
`reserved_rows`, `status_row`, `output_anchor_row`, `output_at_scroll_boundary`,
`output_line_open`, `output_col`, `scroll_region`, `term_rows`, `term_cols`),
`editor_configure_scroll_region`, `editor_set_scroll_layout`,
`editor_scroll_output_up`, `editor_set_scroll_margin`, `editor_clear_row`,
`editor_save_output_cursor`, `editor_restore_output_cursor`, `editor_csi_cursor`,
`editor_move_to_prompt_row`, `editor_write_scroll_output_preserve_prompt`,
`editor_query_cursor`, `editor_get_terminal_size`, `editor_start`, `editor_stop`,
`editor_restore_terminal_layout`, `editor_linenoise_layout_changed`.
In `linenoise.c`: `linenoiseRefreshNewlines`, `refreshMultiLine`,
`linenoiseCleanBlockUp`, `linenoiseGotoCleanBottom`, `linenoiseRefreshStatus`,
`linenoiseSetStatusRow`, `linenoiseSetLayoutCallback`, `linenoiseRenderBuffer`,
`linenoiseEditQueueInput`, `linenoiseEditQueuedInput`, `linenoiseEditFeed`, and
the state the layout depends on: `screen_cursor_row`, `oldrows`,
`oldstatusrows`, `oldstatusgap`, `oldrpos`, `status_row`, `layout_callback`.

## Options tried before the current model

Recorded so the same ground is not walked twice. The labels are the ones used in
the conversation that produced them.

* **Unpinned footer** (footer drawn on the row below the last input row instead
  of the physical last row). Rejected: it drifts up and down as the transcript
  scrolls, and it was the direct cause of the footer being painted over the
  prompt.
* **Reservation that tracks the prompt exactly** (what the code did before
  `7f0b9c6`). Rejected: every Ctrl+J and every backspace over a newline moves
  the whole transcript a row up or down, and the rows handed back by a shrinking
  box were left as a blank band above the prompt.
* **Sticky reservation plus pinned footer plus a transcript that never moves** -
  what is implemented. Chosen over several rounds of dragging a real window
  border, which is the only test that settled any of these.

## Hazards

* Never erase across the region boundary with a relative walk, and never use
  `ESC [ J` (erase down) from inside the prompt block: it deletes the rows below
  and, after a resize, the rows that are no longer the block's.
* Never relayout from `editor_stop()`. It moves the cursor onto the row the next
  prompt is drawn on and erases the accepted-prompt echo and anything printed
  with plain `printf`.
* Never assume `ESC 8` (restore) survived a resize: it is a coordinate, not a
  reference into the content.
* Never print model output with bare `printf` while a scroll region is installed
  - that is exactly how the echo gap was born. Slash commands are the one
  accepted exception, and they invalidate `output_anchor_row` in `editor_stop()`.
* A stale object file has already hidden a correct fix once. Recompiling
  `linenoise.o` after editing `linenoise.c` is not optional.

## Harness limitations

* `termplay.py` counts **bytes, not cells**: UTF-8 wide characters take one
  column each in the model but two in a real terminal. Measured, `ok 你好 end`
  puts the model's cursor at column 13 where pyte says 11. Fine for the ASCII
  transcript probes, wrong for CJK output - use `--pyte` and read
  `pyte-mismatch` when the session contains wide characters.
* `--pyte` needs `pip install pyte` (it pulls `wcwidth`). Without it, drop the
  flag; the model stands on its own.
* The recorder answers `ESC [ 6 n` only with `--answer-cpr`, and a child that
  reads the reply must be in raw mode: the pty slave starts in canonical mode,
  where a CPR reply without a newline is never delivered and the child looks
  hung. Real editors set raw mode, so this only bites hand-written test children.
* Neither tool models mouse reporting, bracketed paste, OSC sequences, or the
  2026 sync-update markers (they are parsed and ignored).
* A resize is invisible on the wire. If a capture was made without `ptyrec.py`'s
  `--resize-marker`, `termplay.py` cannot know where it happened and will report
  `no resize marker` rather than guess.
* `--answer-cpr` keeps its own screen copy, so it inherits the width limitation
  above: CPR replies are byte-column based.

## Picking this up

The intent is to step back and design one cohesive renderer for
output + prompt + footer rather than continue patching the interactions. Read
this file first; the four WIP commits contain the behaviour, and the invariant
that survived every round is: **the transcript never moves, and nothing outside
the prompt block's own rows is ever erased.**

## Appendix A - ptyrec.py

Capture tool, stdlib only. `--answer-cpr` keeps a live copy of
appendix B's screen model and replies to `ESC [ 6 n`, which is what lets
a capture drive code paths that query the cursor. Tested: interactive
stdin forwarding, timed window shrink, marker injection, CPR reply,
replay round trip.

```python
#!/usr/bin/env python3
"""ptyrec.py - run a program on a pty, record its raw output, and optionally
shrink the window partway through so the resize path can be replayed offline
with termplay.py.

  python3 ptyrec.py --rows 24 --cols 80 --resize-rows 20 --resize-after 1.2 \
      --out /tmp/cap.bin -- /bin/sh /tmp/scenario.sh
  python3 termplay.py /tmp/cap.bin --rows 24 --resize-rows 20

Stdin is forwarded, so an interactive program can be driven by hand; pipe a
file in to drive it from a script.  A resize is invisible to the byte stream,
so the recorder injects --resize-marker (default ESC [ ? 9002 h) at the moment
it changes the window size; termplay.py applies the synthetic shrink there.
"""
import argparse
import importlib.util
import fcntl
import os
import pty
import select
import struct
import subprocess
import sys
import termios
import time


def decode_escapes(s):
    return s.encode("latin-1").decode("unicode_escape").encode("latin-1")


def set_winsize(fd, rows, cols):
    fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", rows, cols, 0, 0))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--rows", type=int, default=24)
    ap.add_argument("--cols", type=int, default=80)
    ap.add_argument("--out", default="/tmp/cap.bin")
    ap.add_argument("--resize-rows", type=int, default=0)
    ap.add_argument("--resize-cols", type=int, default=0)
    ap.add_argument("--resize-after", type=float, default=0.0,
                    help="seconds after start; 0 waits for --resize-on")
    ap.add_argument("--resize-on", default="",
                    help="substring of the child output that triggers the resize")
    ap.add_argument("--resize-marker", default=r"\x1b[?9002h")
    ap.add_argument("--timeout", type=float, default=30.0)
    ap.add_argument("--answer-cpr", action="store_true",
                    help="keep a live screen model and reply to ESC [ 6 n")
    ap.add_argument("--model", default="",
                    help="path to termplay.py, needed by --answer-cpr")
    ap.add_argument("cmd", nargs=argparse.REMAINDER)
    a = ap.parse_args()
    cmd = a.cmd[1:] if a.cmd and a.cmd[0] == "--" else a.cmd
    if not cmd:
        sys.exit("no command given (use -- <cmd>)")

    marker = decode_escapes(a.resize_marker)
    pid, master = pty.fork()
    if pid == 0:
        env = dict(os.environ, LINENOISE_ASSUME_TTY="1", TERM=os.environ.get("TERM", "xterm-256color"))
        try:
            os.execvpe(cmd[0], cmd, env)
        except Exception as e:                                # noqa: BLE001
            sys.stderr.write(f"exec failed: {e}\n")
            os._exit(127)

    set_winsize(master, a.rows, a.cols)
    live = None
    if a.answer_cpr:
        path = a.model or os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                       "termplay.py")
        spec = importlib.util.spec_from_file_location("termplay", path)
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        live = module.Screen(a.rows, a.cols)
    out = open(a.out, "wb")
    stdin_fd = sys.stdin.fileno()
    stdin_is_pipe = not sys.stdin.isatty()
    start = time.time()
    resized = not a.resize_rows
    done = False
    while not done:
        rfds = [master]
        if stdin_is_pipe:
            rfds.append(stdin_fd)
        timeout = a.timeout - (time.time() - start)
        if timeout <= 0:
            break
        try:
            r, _, _ = select.select(rfds, [], [], min(timeout, 0.05))
        except (OSError, ValueError):
            break
        if stdin_fd in r:
            data = os.read(stdin_fd, 4096)
            if data:
                os.write(master, data)
            else:
                stdin_is_pipe = False
        if not resized and (
                (a.resize_after and time.time() - start >= a.resize_after) or
                (a.resize_on and r and master in r)):
            out.write(marker)                           # outside the child stream
            set_winsize(master, a.resize_rows, a.resize_cols or a.cols)
            if live is not None:
                live.resize(a.resize_rows, "top")        # live model follows suit
            resized = True
        if master in r:
            try:
                data = os.read(master, 65536)
            except OSError:
                break
            if not data:
                break
            out.write(data)
            out.flush()
            if live is not None:
                live.feed(data)
                at0 = 0
                while True:
                    at = data.find(b"\x1b[6n", at0)
                    if at < 0:
                        break
                    os.write(master, f"\x1b[{live.y + 1};{live.x + 1}R".encode())
                    at0 = at + 4
        else:
            try:                                        # reap check, non-blocking
                got, _ = os.waitpid(pid, os.WNOHANG)
                if got:
                    done = True
            except ChildProcessError:
                done = True
    for _ in range(10):                                 # bounded reap
        try:
            if os.waitpid(pid, os.WNOHANG)[0]:
                break
        except ChildProcessError:
            break
        time.sleep(0.05)
    out.close()
    os.close(master)
    print(f"captured {os.path.getsize(a.out)} bytes to {a.out}", file=sys.stderr)


if __name__ == "__main__":
    main()
```

## Appendix B - termplay.py

Replay and report tool, stdlib only; `--pyte` is optional and needs
`pip install pyte`. Cross-validated against pyte as described above.

```python
#!/usr/bin/env python3
"""termplay.py - replay a captured terminal byte stream and report what a
terminal would show at each phase boundary.

Pure stdlib VT model (no pyte needed) covering the sequences the interactive
prompt work emits: CUP/HA, CUU/CUD/CUF/CUB, CR, LF, BS, IL/DL, EL/ED, DECSTBM,
DECSC/DECRC, DECAWM, SGR (background only), and the 2026 sync markers ignored.

  python3 termplay.py /tmp/rsz_cur.bin --rows 24 --resize-rows 20 --resize-model top
  python3 termplay.py /tmp/echo_cur.bin --pyte

Phase boundaries split the stream; --resize-rows applies a synthetic window
shrink at --resize-marker, because a real resize is invisible to the stream.
"""
import argparse
import re
import sys


def decode_escapes(s):
    return s.encode("latin-1").decode("unicode_escape").encode("latin-1")


class Screen:
    def __init__(self, rows, cols):
        self.rows, self.cols = rows, cols
        self.reset()

    def reset(self):
        self.grid = [[" "] * self.cols for _ in range(self.rows)]
        self.bg = [[0] * self.cols for _ in range(self.rows)]
        self.y = self.x = 0
        self.pend = False           # deferred autowrap
        self.wrap = True
        self.region = [0, self.rows - 1]
        self.saved = (0, 0, False)
        self.attr = 0

    # -- primitives -------------------------------------------------------
    def blank_row(self, r):
        self.grid[r] = [" "] * self.cols
        self.bg[r] = [self.attr] * self.cols

    def scroll_up(self):
        lo, hi = self.region
        r = self.y
        if r == hi:
            for rr in range(lo, hi):
                self.grid[rr] = self.grid[rr + 1][:]
                self.bg[rr] = self.bg[rr + 1][:]
            self.blank_row(hi)
        elif r < self.rows - 1:
            self.y = r + 1

    def scroll_down(self):
        lo, hi = self.region
        for rr in range(hi, lo, -1):
            self.grid[rr] = self.grid[rr - 1][:]
            self.bg[rr] = self.bg[rr - 1][:]
        self.blank_row(lo)

    def put(self, ch):
        if self.pend:
            self.scroll_up()
            self.x = 0
            self.pend = False
        self.grid[self.y][self.x] = ch
        self.bg[self.y][self.x] = self.attr
        if self.x == self.cols - 1:
            self.pend = self.wrap
        else:
            self.x += 1

    def erase_line(self, mode):
        rng = (range(self.x, self.cols) if mode == 0 else
               range(0, self.x + 1) if mode == 1 else range(0, self.cols))
        for c in rng:
            self.grid[self.y][c] = " "
            self.bg[self.y][c] = self.attr

    def erase_display(self, mode):
        rows = range(self.y, self.rows) if mode == 0 else (
            range(0, self.y + 1) if mode == 1 else range(0, self.rows))
        for r in rows:
            for c in range(self.cols):
                self.grid[r][c] = " "
                self.bg[r][c] = self.attr

    def shift(self, n, insert):
        lo, hi = self.region
        if not (lo <= self.y <= hi):
            return
        n = max(1, n)
        if insert:
            for r in range(hi, self.y - 1, -1):
                src = r - n
                self.grid[r] = self.grid[src][:] if src >= self.y else [" "] * self.cols
                self.bg[r] = self.bg[src][:] if src >= self.y else [self.attr] * self.cols
        else:
            for r in range(self.y, hi + 1):
                src = r + n
                self.grid[r] = self.grid[src][:] if src <= hi else [" "] * self.cols
                self.bg[r] = self.bg[src][:] if src <= hi else [self.attr] * self.cols

    def resize(self, rows, model, keep_saved=True):
        """Emulate the window losing or gaining rows.  model='top' keeps the
        first row fixed and destroys the bottom rows; 'cursor' keeps the cursor
        row visible and scrolls the content up."""
        shift = 0
        if model == "cursor" and rows < self.rows:
            shift = max(0, self.y - (rows - 1))
        grid, bg = self.grid, self.bg
        if rows < self.rows:
            grid, bg = grid[shift:shift + rows], bg[shift:shift + rows]
            if rows > len(grid):        # grew after a shift: pad blank
                grid += [[" "] * self.cols] * (rows - len(grid))
                bg += [[0] * self.cols] * (rows - len(bg))
        else:
            grid = grid + [[" "] * self.cols for _ in range(rows - self.rows)]
            bg = bg + [[0] * self.cols for _ in range(rows - self.rows)]
        self.rows = rows
        self.grid, self.bg = grid, bg
        self.region = [max(0, min(self.region[0], rows - 1)),
                       max(0, min(self.region[1], rows - 1))]
        self.y = max(0, min(rows - 1, self.y - shift))
        self.x = max(0, min(self.cols - 1, self.x))
        self.pend = False
        sy = self.saved[0]
        sy = max(0, min(rows - 1, sy - (shift if model == "cursor" else 0)))
        if model != "cursor":
            sy = min(sy, rows - 1)
        self.saved = (sy, self.saved[1], self.saved[2]) if keep_saved else self.saved

    # -- parser -----------------------------------------------------------
    def feed(self, data):
        if isinstance(data, str):
            data = data.encode("latin-1")
        i = 0
        while i < len(data):
            b = data[i]
            if b == 0x1b:
                if i + 1 >= len(data):
                    break
                n = data[i + 1]
                if n == ord("["):
                    j = i + 2
                    while j < len(data) and not (0x40 <= data[j] <= 0x7e):
                        j += 1
                    if j >= len(data):
                        break
                    final = chr(data[j])
                    params = data[i + 2:j]
                    priv = params.startswith(b"?")
                    nums = [int(p) if p.isdigit() else 0
                            for p in params.lstrip(b"?").split(b";")]
                    self.csi(final, priv, nums)
                    i = j + 1
                    continue
                if n == ord("7"):
                    self.saved = (self.y, self.x, self.pend)
                elif n == ord("8"):
                    self.y, self.x, self.pend = self.saved
                elif n == ord("M"):        # reverse index
                    if self.y == self.region[0]:
                        self.scroll_down()
                    else:
                        self.y -= 1
                i += 2
                continue
            if b == 0x0d:
                self.x = 0
                self.pend = False
            elif b == 0x0a:
                self.scroll_up()
            elif b == 0x08:                # backspace, does not wrap
                self.x = max(0, self.x - 1)
                self.pend = False
            elif b == 0x09:                # tab to the next 8 column stop
                self.x = min(self.cols - 1, (self.x // 8 + 1) * 8)
            elif b >= 0x20:
                self.put(chr(b))
            i += 1
        return self

    def csi(self, final, priv, nums):
        if priv:
            if final in ("h", "l") and nums and nums[0] == 7:
                self.wrap = final == "h"
            return                          # 2026, 25, 1049, ... are ignored
        if final in ("H", "f"):
            self.y = max(0, min(self.rows - 1, (nums[0] if nums else 1) - 1))
            self.x = max(0, min(self.cols - 1, (nums[1] if len(nums) > 1 else 1) - 1))
            self.pend = False
        elif final == "A":
            self.y = max(0, self.y - (nums[0] if nums else 1)); self.pend = False
        elif final == "B":
            self.y = min(self.rows - 1, self.y + (nums[0] if nums else 1)); self.pend = False
        elif final == "C":
            self.x = min(self.cols - 1, self.x + (nums[0] if nums else 1)); self.pend = False
        elif final == "D":
            self.x = max(0, self.x - (nums[0] if nums else 1)); self.pend = False
        elif final == "G":
            self.x = max(0, min(self.cols - 1, (nums[0] if nums else 1) - 1)); self.pend = False
        elif final == "r":
            lo = (nums[0] if nums and nums[0] else 1) - 1
            hi = (nums[1] if len(nums) > 1 and nums[1] else self.rows) - 1
            self.region = [max(0, lo), min(self.rows - 1, hi)]
            self.y, self.x = self.region[0], 0
            self.pend = False
        elif final in ("L", "M") and nums:
            self.shift(nums[0], final == "L")
            self.x = 0
            self.pend = False
        elif final == "K":
            self.erase_line(nums[0] if nums else 0)
        elif final == "J":
            self.erase_display(nums[0] if nums else 0)
        elif final == "m":
            k = 0
            if not nums:
                self.attr = 0
            while k < len(nums):
                a = nums[k]
                if a == 0:
                    self.attr = 0
                elif a == 48 and k + 2 < len(nums) and nums[k + 1] == 5:
                    self.attr = nums[k + 2]; k += 3; continue
                elif a == 48 and k + 1 < len(nums):
                    self.attr = nums[k + 1]; k += 2; continue
                k += 1

    # -- views ------------------------------------------------------------
    def lines(self):
        return ["".join(r).rstrip() for r in self.grid]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dump")
    ap.add_argument("--rows", type=int, default=24)
    ap.add_argument("--cols", type=int, default=80)
    ap.add_argument("--marker", default=r"\x1b[?9001h")
    ap.add_argument("--resize-marker", default=r"\x1b[?9002h")
    ap.add_argument("--resize-rows", type=int, default=0)
    ap.add_argument("--resize-model", choices=["top", "cursor"], default="top")
    ap.add_argument("--out", default="out ")
    ap.add_argument("--prompt", default="ds4-agent>")
    ap.add_argument("--footer", default="ctx ")
    ap.add_argument("--show", action="store_true")
    ap.add_argument("--pyte", action="store_true", help="cross-check text grids")
    a = ap.parse_args()

    marker = decode_escapes(a.marker)
    raw = open(a.dump, "rb").read()
    scr = Screen(a.rows, a.cols)

    pre = b""
    chunks = []
    if a.resize_rows:
        rm = decode_escapes(a.resize_marker)
        if rm not in raw:
            sys.exit(f"no resize marker {rm!r} in {a.dump}")
        pre, rest = raw.split(rm, 1)
        chunks = [c for c in rest.split(marker) if c]
    else:
        chunks = [c for c in raw.split(marker) if c]

    pyte_scr = None
    if a.pyte:
        sys.path.insert(0, "/tmp/py_pkgs")
        import pyte
        pyte_scr = pyte.Screen(a.cols, a.rows)
        pyte_st = pyte.Stream(pyte_scr)

    def report(tag, cum_len):
        lines = scr.lines()
        text = [i + 1 for i, l in enumerate(lines) if l.strip()]
        nums = []
        for l in lines:
            s = l.strip()
            if s.startswith(a.out) and s[len(a.out):len(a.out) + 2].isdigit():
                nums.append(int(s[len(a.out):].split()[0]))
        rows_out = [i + 1 for i, l in enumerate(lines) if l.strip().startswith(a.out)]
        holes = [i for i in range(rows_out[0], rows_out[-1] + 1)
                 if not lines[i - 1].strip()] if rows_out else []
        gaps = sum(1 for x, y in zip(nums, nums[1:]) if y != x + 1)
        prompt = [i + 1 for i, l in enumerate(lines) if l.strip().startswith(a.prompt)]
        footer = [i + 1 for i, l in enumerate(lines) if a.footer in l]
        anchor = re.findall(rb"\x1b\[(\d+);\d+H\x1b7", raw[:cum_len])
        mism = ""
        if pyte_scr is not None:
            mine = [l.rstrip() for l in lines]
            theirs = [r.rstrip() for r in pyte_scr.display[:len(lines)]]
            theirs += [""] * (len(mine) - len(theirs))
            bad = sum(1 for x, y in zip(mine, theirs) if x != y)
            mism = f" pyte-mismatch={bad}"
        print(f"{tag}: text rows ..{text[-1] if text else '-'}  "
              f"{a.out.strip()} {nums[0] if nums else '-'}..{nums[-1] if nums else '-'}"
              f"  gaps={gaps} holes={len(holes)}  prompt{prompt} footer{footer}  "
              f"anchor={anchor[-1].decode() if anchor else '-'}  "
              f"cursor={scr.y + 1},{scr.x + 1}{mism}")
        if a.show:
            for i, l in enumerate(lines, 1):
                if l.strip():
                    print(f"   {i:3}| {l.strip()[:36]}")

    cum = 0
    if a.resize_rows:
        scr.feed(pre); cum = len(pre)
        if pyte_scr is not None:
            pyte_st.feed(pre.decode("latin-1"))
        report("pre-resize ", cum)
        saved = scr.saved[0] + 1
        shift = scr.y + 1 - a.resize_rows if a.resize_model == "cursor" else 0
        scr.resize(a.resize_rows, a.resize_model)
        print(f"-- window shrunk to {a.resize_rows} rows "
              f"(model={a.resize_model}, content shifted {shift} rows, "
              f"saved cursor {saved} -> {scr.saved[0] + 1})")
        if pyte_scr is not None:
            keep = [l for l in
                    [r.rstrip() for r in pyte_scr.display][shift:shift + a.resize_rows]]
            pyte_scr = pyte.Screen(a.cols, a.resize_rows)
            pyte_st = pyte.Stream(pyte_scr)
            for i, text in enumerate(keep, 1):
                if text:
                    pyte_st.feed(f"\x1b[{i};1H{text}")
            pyte_st.feed(f"\x1b[{max(1, scr.y + 1)};1H")
            if saved:
                pyte_st.feed(f"\x1b[{max(1, saved - shift)};1H\x1b7")
    for i, part in enumerate(chunks):
        scr.feed(part)
        cum += len(part)
        if pyte_scr is not None:
            pyte_st.feed(part.decode("latin-1"))
        report(f"phase {i + 1}     ", cum)


if __name__ == "__main__":
    main()
```

## Appendix C - scenario.sh

A fake prompt block drawn with raw escapes. It exercises the harness
itself - region scroll, prompt block, footer, shrink, region shrink -
without the agent, so a harness bug cannot be mistaken for a product bug.

```sh
#!/bin/sh
printf '\033[1;20r\033[20;1H'
i=1
while [ $i -le 22 ]; do printf '\r\nout %02d' "$i"; i=$((i+1)); done
printf '\033[21;1Hds4-agent> hello\033[22;1Hsecond line\033[23;1Hthird line'
printf '\033[24;1H\033[48;5;238;38;5;252mctx 1.3k/200k | idle\033[0m'
sleep 1.5
printf '\033[1;16r\033[16;1H'
for j in 23 24 25; do printf '\r\nout %02d' "$j"; done
printf '\033[17;1H\033[0Kds4-agent> hello\033[18;1H\033[0Ksecond line\033[19;1H\033[0Kthird line'
printf '\033[20;1H\033[48;5;238;38;5;252mctx 1.3k/200k | idle\033[0m'
```

## Appendix D - agentprobe.c

Drives the real `editor_start()` / output / `editor_stop()` cycle. The two
things that are easy to forget: the probe `#include`s `ds4_agent.c` to
reach the static functions, which is why `DS4_NO_GPU` and
`-DAGENT_SRC=<path>` are on the command line; and the child must call
`linenoiseSetMultilineEdits(1)` or Ctrl+J does nothing and the prompt box
never grows, which silently turns every multiline check into a no-op.
Tested at 24x80 with and without a resize and at 12x40, all phases
matching pyte.

```c
/* agentprobe.c - drive the real interactive editor on a pty and record the raw
 * bytes for termplay.py.  Includes ds4_agent.c so it can call the static
 * editor_* functions; link against the objects the normal build produces.
 *
 *   cc -O1 -g -D_GNU_SOURCE -Wno-unused-function -DDS4_NO_GPU \
 *      -DAGENT_SRC=/home/ds4/ds4_agent.c -I/home/ds4 -c -o probe.o agentprobe.c
 *   cc -O1 -g -o probe probe.o ds4_json_cpu.o ds4_help.o ds4_prompt_prefix.o \
 *      ds4_web.o ds4_kvstore.o linenoise.o ds4_cpu.o ds4_image.o \
 *      ds4_distributed.o ds4_tp.o ds4_ssd.o ds4_layer_pack.o -lm -pthread -lutil
 *
 * Environment:
 *   PROBE_ROWS/COLS     terminal size at start          (default 24x80)
 *   PROBE_LINES         model output lines before keys  (default 20)
 *   PROBE_KEYS          keys fed to the editor, C escapes: "a\x1bb\n"
 *   PROBE_TAIL          output lines written after the keys (default 4)
 *   PROBE_NEWROWS       shrink to this many rows, 0 = no resize (default 0)
 *   PROBE_DUMP          output file                       (default /tmp/probe.bin)
 *
 * Phase marker  is ESC [ ? 9001 h  (termplay.py splits on it)
 * Resize marker is ESC [ ? 9002 h  (termplay.py applies the shrink there)
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pty.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>

#define DS4_AGENT_TEST_NO_MAIN
#define G_STR(x) #x
#define G_INC(x) G_STR(x)
#include G_INC(AGENT_SRC)

#define MARKER "\x1b[?9001h"
#define RESIZE_MARK "\x1b[?9002h"

static void emit(const char *s) { write_all(STDOUT_FILENO, s, strlen(s)); }

/* Feed bytes to the editor one at a time, running the edit state machine after
 * each one, until the input is consumed or a line was submitted. */
static void feed(agent_editor *ed, const char *bytes, size_t n) {
    for (size_t i = 0; i < n; i++) {
        linenoiseEditQueueInput(&ed->edit, bytes + i, 1);
        for (;;) {
            errno = 0;
            char *line = linenoiseEditFeed(&ed->edit);
            if (line == linenoiseEditMore) {
                if (linenoiseEditQueuedInput(&ed->edit) == 0) break;
                continue;
            }
            free(line);
            break;
        }
    }
}

static void out_line(agent_editor *ed, int n) {
    char text[64];
    int len = snprintf(text, sizeof(text), "out %02d\n", n);
    editor_write_scroll_output_preserve_prompt(ed, text, (size_t)len, true);
}

/* "\n" -> newline, "\xNN" -> byte, anything else literal. */
static size_t unescape(const char *s, char *out, size_t cap) {
    size_t o = 0;
    while (*s && o + 1 < cap) {
        if (s[0] == '\\' && s[1] == 'x' && s[2] && s[3]) {
            char hex[3] = {s[2], s[3], 0};
            out[o++] = (char)strtol(hex, NULL, 16);
            s += 4;
        } else if (s[0] == '\\' && s[1] == 'n') {
            out[o++] = '\n'; s += 2;
        } else if (s[0] == '\\' && s[1] == 'r') {
            out[o++] = '\r'; s += 2;
        } else {
            out[o++] = *s++;
        }
    }
    return o;
}

static void phases(void) {
    int lines = atoi(getenv("PROBE_LINES") ? getenv("PROBE_LINES") : "20");
    int tail = atoi(getenv("PROBE_TAIL") ? getenv("PROBE_TAIL") : "4");
    int new_rows = atoi(getenv("PROBE_NEWROWS") ? getenv("PROBE_NEWROWS") : "0");
    const char *keys = getenv("PROBE_KEYS") ? getenv("PROBE_KEYS") : "";
    char keybuf[512];
    size_t key_len = unescape(keys, keybuf, sizeof(keybuf));

    agent_editor ed;
    memset(&ed, 0, sizeof(ed));
    if (editor_start(&ed, "ds4-agent> ", "ctx 1.3k/200k | idle", NULL) != 0) return;
    emit(MARKER);                                       /* phase 1: start */

    for (int i = 1; i <= lines; i++) out_line(&ed, i);
    emit(MARKER);                                       /* phase 2: output */

    if (key_len) feed(&ed, keybuf, key_len);
    emit(MARKER);                                       /* phase 3: keys */

    for (int i = lines + 1; i <= lines + tail; i++) out_line(&ed, i);
    emit(MARKER);                                       /* phase 4: more output */

    if (new_rows > 0) {
        struct winsize ws = {.ws_row = (unsigned short)new_rows, .ws_col = 80,
                             .ws_xpixel = 0, .ws_ypixel = 0};
        if (ioctl(STDOUT_FILENO, TIOCSWINSZ, &ws) != 0) perror("resize");
        emit(RESIZE_MARK);
        emit(MARKER);                                   /* phase 5: resized */
        feed(&ed, "\x01", 1);      /* no-op key: the refresh reads the new size */
        emit(MARKER);                                   /* phase 6: relayout */
        for (int i = lines + tail + 1; i <= lines + tail + 3; i++)
            out_line(&ed, i);
        emit(MARKER);                                   /* phase 7: output */
    }

    tcdrain(STDOUT_FILENO);
    editor_stop(&ed);
    editor_restore_terminal_layout(&ed);
}

int main(void) {
    int rows = atoi(getenv("PROBE_ROWS") ? getenv("PROBE_ROWS") : "24");
    int cols = atoi(getenv("PROBE_COLS") ? getenv("PROBE_COLS") : "80");
    const char *dump = getenv("PROBE_DUMP") ? getenv("PROBE_DUMP") : "/tmp/probe.bin";

    int master, slave;
    struct winsize ws = {.ws_row = (unsigned short)rows, .ws_col = (unsigned short)cols,
                         .ws_xpixel = 0, .ws_ypixel = 0};
    if (openpty(&master, &slave, NULL, NULL, &ws) != 0) return 2;
    pid_t pid = fork();
    if (pid < 0) return 2;
    if (pid == 0) {
        close(master);
        dup2(slave, STDIN_FILENO);
        dup2(slave, STDOUT_FILENO);
        dup2(slave, STDERR_FILENO);
        close(slave);
        setenv("LINENOISE_ASSUME_TTY", "1", 1);
        linenoiseSetMultilineEdits(1);      /* without this Ctrl+J does nothing */
        phases();
        _exit(0);
    }
    close(slave);
    FILE *out = fopen(dump, "wb");
    char buf[8192];
    ssize_t n;
    while ((n = read(master, buf, sizeof(buf))) > 0) fwrite(buf, 1, (size_t)n, out);
    fclose(out);
    close(master);
    int status = 0;
    waitpid(pid, &status, 0);
    fprintf(stderr, "wrote %s\n", dump);
    return 0;
}
```
