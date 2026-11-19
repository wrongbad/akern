---
name: akern
description: Background jobs, interactive programs and sub-agents — run commands without waiting, talk to them while they run, stop them, and delegate tasks to sub-agents.
---

# Jobs and sub-agents

Every command here is an ordinary program; run it with `exec`. A call you do
not wait for is a job, and its name is that exec call's own id.

| Goal | How |
|---|---|
| Start a command without waiting (a server, a watcher, a long build) | any argv, plus `"wait": false` on the exec call. The reply says it is still running and names its pid and output files |
| Look at a job's output so far | `["tail", "-n", "50", "<the .out file from the reply>"]`, and the `.err` file likewise |
| Stop a job | `["kill", "-TERM", "--", "-<pid>"]` — the minus sign signals the job's whole process group; its result still arrives |
| Talk to a program while it runs (a REPL, a sub-agent) | start it with `"stdin_open": true` and `"wait": false`, then `write` with `"job"` set to that exec call's id and the text as `content`; `"close": true` ends its input |
| Delegate a task to a sub-agent | `["akern", "-p", "-s", "agents/name.jsonl", "the full task description"]` with `"wait": false, "stdin_open": true`, so you keep working and can redirect it; its final answer arrives as the job's stdout. `-s` keeps its conversation in that file, so you can read it or continue it later with the same `-s`. Add `"-m", "provider/model"` to use another model |
| Redirect a running sub-agent | `write` a line to its job: plain text reaches it at its next step; `{"type":"send","when":"now","content":"…"}` interrupts it at once. End the line with a newline |

A call you do wait for can also become a job: if a message arrives while it
runs, its reply says it is still running, and it carries on.

A job's result arrives later as a message naming its call. You do not need to
wait or poll: if there is nothing else to do, end your turn, and the result
will start a new one.
