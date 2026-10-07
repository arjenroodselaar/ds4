# Sandbox Subprocess Protocol

`ds4-agent --sandbox CMD` runs the whole agent behind one long-lived child
process, spawned as `/bin/sh -c CMD`.  Every tool call the model makes is sent to
that child and its answer is reported back.  The child is the boundary: when it
is gone, the session is over, because continuing without it is the one failure
mode the mode exists to prevent.

This document is the contract for a sandbox implementation.  The agent side
lives in the "Sandbox Subprocess" section of `ds4_agent.c`.

## Process lifecycle

- The sandbox starts **before** the model is loaded, and the agent does not open
  the engine until the sandbox has announced itself.  A command that cannot run
  therefore fails in milliseconds with the shell's own message instead of after a
  minutes-long load.  See **Startup handshake**.
- Environment is deliberately small: `DS4_SANDBOX=1`, plus `PATH`, `HOME` and
  `TMPDIR` copied from the agent.  Everything else is the sandbox's business.
- The child is its own process group leader, so teardown reaches grandchildren.
- Teardown closes stdin, then escalates SIGTERM to SIGKILL for the group.  It
  runs on every exit path, including the paths that leave through `exit()`, and
  including an agent interrupted while it waits for the startup notice: the wait
  may be abandoned, the process group may not.
- A sandbox that dies or is killed ends the session: the interactive REPL takes
  exactly the `/exit` path (including the offer to save the transcript) and the
  non-interactive mode prints the reason to stderr and exits 1.  Both notice at
  a turn boundary, so a reply already being generated is finished and printed
  first.
- If a write fails with `EPIPE`, the agent reports it.  The agent ignores
  `SIGPIPE` from the moment the child exists.

## Startup handshake

A sandbox announces that it is usable with one notice, written before it reads
anything:

```json
{"id":0,"type":"log","text":"ds4-sandbox-helper 0.1.0 ready: read_lines default 240, edit_upto=4096"}
```

- The **first frame** a sandbox writes is an id-0 notice whose `text` contains the
  word `ready`, matched ASCII case-insensitively.  Everything a user should be
  told about the sandbox — implementation and version, the caps it settled on, the
  directory it works in — belongs in that line, because it is the only thing the
  agent prints on its account.  Detection keys on the word and not on `type`: a
  sandbox that prefers `"type":"ready"` may send it, and one that buries the word
  in a longer sentence is still understood.
- Write the notice and flush it at startup, **before reading stdin**.  A sandbox
  that waits for its first request before announcing itself never receives one,
  because the agent is waiting for the notice.
- The agent blocks on the notice, and blocks before it opens the engine.  This is
  the point of the handshake: without it the agent loads a model for minutes and
  then discovers in the first tool call that there is no sandbox.  The price is
  that a sandbox which stays alive and never says `ready` stops the run with both
  processes healthy and nothing printed.  That trade is accepted for startup;
  the unbounded wait per request is a separate, open question under **Open:
  nothing bounds the wait**.
- The wait ends on the first of these, and on nothing else:

  | Event | Outcome |
  |---|---|
  | a notice whose `text` contains `ready` | startup complete, and the notice is printed |
  | the child exits, or both its pipes reach EOF | startup failure, with the exit status and the newest stderr |
  | a framing or protocol fault on anything the sandbox writes | startup failure, naming the fault |
  | a frame with a non-zero `id` before the agent has sent a request | startup failure: there is nothing that frame could answer |

  A command that is missing, unparseable or instantly broken is caught by the
  second and third rows, so this replaces the short timed grace the agent waits
  out today rather than putting a deadline on top of it.  What remains unbounded
  is a live sandbox that says nothing.  A startup failure is reported on stderr
  and the agent exits without loading a model.
- Until the notice arrives, every complete line the sandbox writes to **stderr**
  is echoed as `sandbox: <line>`, because that is where a misconfigured command
  explains itself.  From the notice onward stderr returns to being a tail, a
  trace mirror and a failure reason: echoing it for the rest of the run would
  break the rule in **Channels** that nothing out of band reaches the
  conversation.  A line is echoed once it is complete, so a sandbox that reports
  progress without newlines is not echoed until it ends.
- The notice itself is printed once, as `sandbox: <text>` on stderr, beside the
  model-loading messages, and appended to the trace log when `--trace` is set.
  It is not part of the conversation and never reaches the model.
- Saying `ready` is not a promise of continued health.  A sandbox that dies
  afterwards takes the ordinary mid-run death path under **Process lifecycle**,
  not the startup failure path.

## Channels

| Child channel | Meaning |
|---|---|
| stdin | requests from the agent |
| stdout | frames only, in the format below — nothing else may ever be printed here |
| stderr | the child's own diagnostics; never a result |

The sandbox folds the stderr of the work it performs into the `result` text it
reports, so nothing out of band can reach the conversation.  Agent stderr is
drained continuously, kept as a short tail, mirrored to `<trace_path>.sandbox.log`
when `--trace` is set, and included in the failure reason when the sandbox dies.
Startup stderr is the one exception, described in **Startup handshake**.

Everything the sandbox wants the agent to see, including its startup notice, is a
frame.  A banner printed as a bare line on stdout is not a greeting but a fault:
the agent reads the first line of every frame as a byte count, and text is not
one.

## Framing

Both directions use the same frame:

```
<decimal byte count>\n<exactly that many bytes of JSON>
```

- The header is 1-20 ASCII digits followed by a newline, and at most 32 bytes.
- The payload is exactly one JSON object.  It may contain raw newlines; nothing
  is escaped because the length is counted, not delimited.
- Ceilings: **1 MiB** per request, **4 MiB** per response.
- The count is **bytes**, not characters.  Write the payload as bytes and count
  the encoded form: a result containing one non-ASCII character is longer in bytes
  than in characters, and a header that counts characters desynchronises the
  stream and ends the session.
- Requests are strictly sequential: exactly one is in flight at a time, and the
  answer must carry the same `id`.

## Request (agent to sandbox)

```json
{"id":7,"tool":"read","limits":{"read_lines":240},"args":{"path":"ds4_agent.c","start_line":"120"}}
```

- `id` — integer starting at 1, increasing.  `0` is reserved for sandbox
  notices and never appears in a request.
- `tool` — one of the routed names below.
- `args` — a flat object in which **every value is a JSON string**, numbers and
  booleans included (`"timeout_sec":"30"`, `"whole":"true"`).  This mirrors the
  flat string arguments the tools parse today, so a sandbox can pass them
  through unchanged.  Omitted arguments are simply absent; `"args":{}` is valid.
- `limits` — an object of things the sender knows about the model behind the
  request that the sandbox cannot work out for itself.  Its values are JSON
  numbers, unlike `args`.  A sandbox ignores a `limits` member it does not
  recognise, and a `limits` object that is absent or malformed, rather than
  rejecting the request: that is what lets a newer agent name another cap to an
  older sandbox.  Any future cap belongs here rather than beside `id`, so the
  request object itself does not grow a new top-level member every time.
- `limits.read_lines` — how many lines a `read` or `more` that names no size should
  return.  The agent chooses it from the model's context window (120 lines up to
  8192 tokens, 240 up to 16384, 500 above), and sends it every request because the
  sandbox cannot see the model it is answering for.  A sandbox should clamp the
  number it is given (at least 1, at most 500), fall back to its own setting when it
  is absent, and let an explicit `max_lines` or `count` argument win over both.  The
  cap is there so a hand-written frame cannot ask for the whole file; the 128 KiB
  answer limit still applies either way.

Routed tools: `read`, `more`, `write`, `list`, `edit`, `search`, `bash`,
`bash_status`, `bash_stop`.  `bash` honours `timeout_sec` inside the sandbox.

`google_search`, `visit_page` and `view_image` are never sent: the agent answers
them itself with a fixed "not available in sandbox mode" error, because they need
the network, a browser or the vision model that lives in the agent process.  A
name that is neither routed nor one of those three is reported locally as an
unknown tool, also without a request: the sandbox is not asked to guess what a
tool it never advertised should do.

The agent also stops preflighting `edit` calls against its own filesystem while
sandboxed.  Normally an `old` selector that cannot match is rejected while the call
is still being generated; in sandbox mode only the sandbox knows what its file
contains, so the check belongs to the `edit` tool there and the answer arrives as
an ordinary failed call.

A request that would exceed 1 MiB is not written at all; the agent reports the
failed call itself.  This can happen with a large `write` or `edit` body and is
not a sandbox fault.

## Response (sandbox to agent)

```json
{"id":7,"ok":true,"result":"   120  static int main(int argc, char **argv) {\n"}
{"id":7,"ok":false,"error":"read: no such file or directory"}
```

- `ok` is a JSON boolean and is mandatory, and so is the matching text member:
  `result` when true, `error` when false.  Both are strings, already formatted
  for the model, tool stderr included.
- The agent wraps the text as `Tool result N (name):` and truncates it to
  128 KiB on a UTF-8 boundary, so an answer only has to be complete enough to be
  useful.
- A response whose `id` matches nothing currently pending is discarded silently,
  which is what makes an interrupted request safe to abandon: the user can
  interrupt a wait, and the late answer that follows is dropped instead of being
  applied to a later call.

## Sandbox notices

```json
{"id":0,"type":"log","text":"watching 3 paths"}
```

A notice is a frame with `id` 0.  It never completes a request and never reaches
the model.  The agent appends it to `<trace_path>.sandbox.log`, which means it is
dropped entirely without `--trace`, and it does not count as activity for any
timeout.  Extra members are allowed.

The startup notice is the exception: it is the one notice the agent waits for and
the one it prints, as described in **Startup handshake**.  Notices after it stay
diagnostics.

## Failures

Fatal, and the session is torn down:

- a header that is not a byte count, is empty, or is longer than 32 bytes;
- EOF or a short read in the middle of a payload;
- a payload that is not one JSON object, or a response frame missing `id` or a
  boolean `ok`.

Recoverable, and only the affected call fails:

- a response over 4 MiB: the frame is discarded, byte-counted out of the stream
  without buffering it, and the waiting request is answered with an error;
- a request over 1 MiB: never written;
- a response for an unknown or abandoned `id`: dropped.  Before the agent has
  sent its first request there is nothing that frame could be answering, and it
  fails startup instead.

## Open: nothing bounds the wait

The agent waits for a response without a deadline, which is the one way this
protocol can stop a run with both processes still alive.  A sandbox that is
healthy but slow is only delayed; a sandbox that is wedged — blocked in a read of
its own, waiting on a lock inside the container, or a process that the container
runtime started but never managed to launch — holds the session open
indefinitely.  An interactive user can interrupt that wait.  A non-interactive
run cannot: the model has already been answered, the tool has not run, and the
process sits there until it is killed from outside.

A fix has an obvious shape: a per-request deadline that fails **the call** rather
than the session, since the sandbox may be perfectly healthy and merely slow, and
the abandoned-request rule already makes its late answer harmless.  Nothing like
it is implemented yet, so a sandbox that wants to be robust on its own has to
bound the work it does per request (`bash` has `timeout_sec` for exactly this
reason) rather than rely on the agent to time it out.

The startup wait is unbounded by decision and is not this open question.  A
sandbox that never says `ready` has not started, and the agent would rather stop
before a minutes-long model load than after it, so it waits as long as the child
lives and prints nothing.  The two differ in kind as well as in length: the
startup wait happens once, before anything has been said to the model, and the
per-request wait happens mid-conversation, after the model has already been
answered and only the tool is missing.

## Minimal sandbox

The smallest useful shape, in shell, is a loop that reads a count, reads that
many bytes, and answers.  In practice a sandbox is written in a language with a
JSON library and `subprocess`:

```python
import json, subprocess, sys

inp, out = sys.stdin.buffer, sys.stdout.buffer

def frame(obj):
    body = json.dumps(obj).encode("utf-8")           # count bytes, not characters
    out.write(str(len(body)).encode("ascii") + b"\n" + body)
    out.flush()

def run(tool, args):                                 # every arg value is a str
    if tool == "bash":
        p = subprocess.run(args["command"], shell=True, capture_output=True,
                           stdin=subprocess.DEVNULL,
                           timeout=int(args.get("timeout_sec", "3600")))
        return p.returncode == 0, (p.stdout + p.stderr).decode("utf-8", "replace")
    return False, "not implemented by this sandbox: " + tool

frame({"id": 0, "type": "log", "text": "py-sandbox 0.1 ready"})    # before reading stdin

while True:
    header = inp.readline()
    if not header:
        break
    req = json.loads(inp.read(int(header)))
    try:
        ok, text = run(req["tool"], req.get("args", {}))
    except Exception as exc:                         # noqa: BLE001
        ok, text = False, str(exc)                   # a failed tool is a result,
    frame({"id": req["id"], "ok": ok,                # not a broken session
           "result" if ok else "error": text})
```

Four traps in that loop are worth naming, because each one is a silent hang or a
dead session rather than a message:

- Send the startup notice **before** reading stdin.  A sandbox that greets the
  agent only once it has a request is never asked for one: the agent waits for
  the notice and the sandbox waits for the request.
- Anything printed to stdout outside `frame()` corrupts the stream and ends the
  session; diagnostics belong on stderr.
- Read the header and the payload from the **same** stream.  A text-mode
  `sys.stdin.readline()` followed by a byte-mode `sys.stdin.buffer.read(n)` loses
  the bytes the text layer already buffered: the sandbox then waits forever for a
  payload that arrived long ago.
- Never let a tool subprocess inherit the sandbox's stdin.  A command that reads
  stdin consumes the agent's *requests* off the protocol, and the session is
  unrecoverable.  Hand it `subprocess.DEVNULL`, or its own input.
