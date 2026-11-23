# Multiple clients

How more than one program could drive or watch one akern session, and what
gets in the way. **Parked: nothing here is built or planned.** akern has one
driver per session, and the work goes into making that one driver good. This
file keeps the reasoning so that it does not have to be done again.

## 1. Today: one driver

- **The session file has one writer.** The kernel locks `path.owner` with
  `fcntl` and refuses to open a session another kernel owns (exit 2). It keeps
  an index of the file in memory (offsets, `file_size`, the last `reset`) that assumes
  no one else appends.
- **The kernel has one input.** Its stdin belongs to whoever started it: the
  `akern-cli` front end, `akern-agent`, or `akern-acp`. Anyone else reaches the
  session through that driver, with `append` on its stdin
  ([roadmap](roadmap.md#appending-to-an-owned-session)).
- **`akern-acp` is one connection.** Each editor or host starts its own
  `akern-acp` over stdio, and each open session is one akern child. A second
  `akern-acp` on the same sessions directory is held off only by the owner
  lock (§5).
- **Readers are fine.** Reading the file while the kernel writes it is safe
  when the reader consumes complete lines only. `akern-acp` already follows
  sub-agents this way, re-reading the child's file as it grows and keeping the
  unfinished last line for later.

## 2. Several writers appending to one file

What the platforms give, and what they do not.

| Fact | Consequence |
|---|---|
| `PIPE_BUF` (512 on macOS, 4096 on Linux) is about pipes and FIFOs only | there is no record size below which a regular-file append is guaranteed atomic |
| `O_APPEND` makes the seek to the end and the write one step | two appends never overwrite each other |
| On local filesystems one `write()` lands as one contiguous block, any size (Linux: the inode lock; macOS: the vnode lock) | relied on widely, but documented by neither platform |
| `write()` can return short (ENOSPC, quota, file-size limit, a fatal signal during a large write) | a retry loop like `write_all` is a second append, and another writer's line can land in the middle |
| stdio flushes when its buffer fills | a line written with `fprintf` can go out as two `write()`s; build the line, then write it once |
| NFS emulates `O_APPEND` on the client; SMB, FUSE, sshfs, Docker-on-Mac mounts and sync folders vary | none of the above holds off a local disk |
| A crash or power loss can cut off the last line, or pad it with NUL bytes | readers skip a last line that does not parse; a writer that appends after a torn line glues onto it |

The bytes are the easy part. A session is linear, so a whole line appended
by anyone is in order. But the kernel's index goes stale, and two writers'
entries interleave: a `reset` or a note can land between a call and its
result. Several writers need either one process that serializes them, or a
lock held across *re-read the tail, write*.

If appends from several processes are ever needed, the options in order of
robustness:

1. **One owner, others send it messages.** The kernel, or a process in front
   of it, is the only writer and orders the entries.
2. **`flock` around each append.** Lock, re-read from the last known size,
   one `write()`, unlock. Cheap at turn frequency.
3. **Lock-free `O_APPEND`.** Only for records with no order between them (logs,
   independent events), on a local disk, with readers that tolerate a bad last
   line.

## 3. Transports into a single owner

If one process owns the session and others send to it, how they reach it.

| Transport | Several writers | From bash | Costs |
|---|---|---|---|
| Kernel stdin, shared | interleaves past `PIPE_BUF`: 512 bytes on macOS, less than one tool result | `\| akern` | stdin is the driver's control channel; mixing producers into it loses that |
| Named pipe (FIFO) | same `PIPE_BUF` limit, and no way to tell writers apart | `> fifo` | `open` blocks until a reader exists; the reader sees EOF each time the last writer closes (it must hold its own write end); writers get SIGPIPE if the owner dies; buffered data is lost on a crash |
| A pipe per child, on a known fd (say fd 3, with `AKERN_FD=3`) | never interleaves: one pipe per call, and the kernel knows which call wrote | `>&3` | only for processes the kernel starts; a background grandchild that inherits the fd keeps the pipe open, so EOF does not mean the call ended |
| Unix socket (`SOCK_STREAM`) | never interleaves: a stream per connection; peer identity from `getpeereid` | needs a client (`nc -U` where supported, `socat`, or a shipped `akern-post`) | `sun_path` is 104 bytes on macOS, and `$TMPDIR` there uses half; a stale socket file after a crash must be unlinked (the owner lock says when it is safe); `SOCK_DGRAM` is small on macOS and `SOCK_SEQPACKET` does not exist there |
| A user-space filesystem (§4) | never interleaves: state per open | `>> ctl` | installing it, especially on macOS |

Two-way transports (the socket, the filesystem) let the owner answer each
record with the entry id it assigned, or refuse it, so the sender learns
where its record went.

A client such as `akern-post` would also wrap raw text: shell commands print
lines, not JSON, so `cmd | akern-post` turns each line into an object without
the command knowing about JSONL.

## 4. The session as a filesystem

The QNX resource manager, where opening and reading a path runs the server's
code, exists here as FUSE: the kernel forwards each `open`, `read`, `write` and
`close` on a mounted path to a user-space process.

| Mechanism | Linux | macOS |
|---|---|---|
| FUSE (libfuse) | built in | macFUSE (a kernel extension: a reboot into recovery to allow it on Apple Silicon) or FUSE-T (a local NFS server: writes cached until close, stale attributes, no caller pid) |
| CUSE, a character device in user space | yes, root to create the node | no |
| FSKit, Apple's user-space filesystem API | no | macOS 15.4 and later: Swift, packaged as an app extension; newer macFUSE can run on it without the kernel extension |
| Local NFS, SMB or WebDAV server | yes | yes, mounts without extensions, but caching hides individual calls |
| Kernel module | possible | kernel extensions are deprecated |

What FUSE would give: a handle per `open`, so lines are buffered per writer and
never interleave; the caller's pid and uid (`fuse_get_context`); an error
returned to the writer for a bad line (`EINVAL` from its own `write()`); and
reads that block until there is data (`direct_io`, not seekable).

The shape would be a directory per session, in the Plan 9 style, not a file
that pretends to be the session:

```
/mnt/akern/<id>/
  ctl            write: prompts, steering, cancel; one JSON line per write
  events         read: the live event stream; each open is its own subscriber
  session.jsonl  read-only view of the real file, for replay
  status         read: a snapshot of current state
```

The real session file stays on a local disk, written, locked and `mmap`ed by
the kernel as today. The filesystem is a front end only.

Against it: installation on macOS; a mount that something must keep alive (a
dead server gives "Transport endpoint is not connected" on Linux, or hangs);
FUSE delivers writes in chunks up to `max_write` (128 KB by default), so
reassembly per handle is still needed; containers need `/dev/fuse` and
usually `CAP_SYS_ADMIN`; and tools that trust `st_size` (`tail -f` on macOS,
editors) misbehave on streaming files.

## 5. ACP

ACP v2 leaves several connections on one session to the agent: it "may reject
the resume or end the other connection's activity"
([acp-v2.md](acp-v2.md#lifecycle)). ACP has no read-only subscribe: watching a
session means resuming it.

What `akern-acp` does today, read from the code and not tested:

- **A second `akern-acp` resumes a session another has open.** It replays the
  whole history, marks the calls still open in the file as abandoned (they are
  still running under the first client), sends `replay_complete`, starts akern,
  which exits on the owner lock, and answers `-32603` with akern's message.
  Nothing is corrupted, but the client gets a full replay and false failures
  for a session it cannot use.
- **The same connection resumes a session it has open.** It answers `-32602`,
  "the session is already open". The spec says this should re-synchronize: work
  continues, and the new MCP servers and roots replace the old.
- **`session/new` with a `sessionId` of an existing file** that no one owns
  continues that file, without a replay. It checks only the sessions open on
  its own connection.

The first two are worth fixing even with one client: check the owner lock
before replaying and reject cleanly, and treat a resume of an open session as a
re-sync.

## 6. If this is picked up

A shape that keeps the kernel as it is:

1. **A broker** that owns the kernel's stdin, listens on a Unix socket, adds
   prompts from any client to the inbox, and fans events out to every
   connection. `akern-acp` would then be one such client per editor
   connection, and several editors could share a session.
2. **Watchers** that only replay and follow the file, as `akern-acp` follows
   sub-agents, with no kernel at all. The file holds the same lines as stdout
   ([akern.md](akern.md#session-file)), so a watcher sees everything, streamed
   text and terminal output included.
3. **A FUSE directory** on Linux, as a thin layer over the socket. On macOS the
   socket client instead.

Open questions:

- Who decides between two clients steering the same run at once: last write
  wins, a queue in order of arrival, or one client holding the floor.
- Whether permission requests (`AKERN_ASK`, ACP's `session/request_permission`)
  go to one client or all, and who answers.
- Whether the broker is a new program or `akern-acp` grows a listening mode.
