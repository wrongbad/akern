#!/usr/bin/env -S jq -nj --unbuffered -f
def dim: "\u001b[2m\(.)\u001b[0m";
def running: "\(.) job\(if . == 1 then "" else "s" end) running";

def show:
  if .type == "text_delta"  then .text
  elif .type == "tool_start"  then "\n" + ("▸ \(.name) \(.input | tostring)" | dim) + "\n"
  elif .type == "block" and .block.type == "tool_result"
                              then ("  ✓ \(.block.tool_use_id)\(if .block.is_error then " (error)" else "" end)" | dim) + "\n"
  elif .type == "preempt"     then "\n\u001b[33m⏸ interrupted\u001b[0m\n"
  elif .type == "interrupt"   then "\n" + ("(press Ctrl-C again within \(.quit_within)s to quit)" | dim) + "\n"
  elif .type == "notice"      then "\u001b[33m[\(.source)] \(.message)\u001b[0m\n"
  elif .type == "error"       then "\u001b[31merror\(if .where then " (\(.where))" else "" end): \(.message)\u001b[0m\n"
  elif .type == "turn_end"    then "\n"
  elif .type == "run_end" and .reason != "done" then ("[run \(.run) \(.reason)]" | dim) + "\n"
  else "" end;

def status:
  if .run then "… working"
  elif (.open | length) > 0 then "⏵ \(.open | length | running) · waiting for input"
  else "› waiting for input" end;

# open: calls started and not yet answered; told: those already shown as still running, which a
# call is once a turn starts or the run ends without its result.
foreach inputs as $e ({run: ($ARGS.named.prompted == "true"), open: [], told: [], status: null, nl: true};
  .run = (if $e.type == "run_start" then true elif $e.type == "run_end" then false else .run end)
  | (if $e.type == "tool_start" then .open += [$e.call]
     elif $e.type == "block" then .open -= [$e.block.tool_use_id] | .told -= [$e.block.tool_use_id]
     else . end)
  | (if $e.type == "turn_start" or $e.type == "run_end" then .open - .told else [] end) as $kept
  | .told += $kept
  | (([$kept[] | "  ↳ \(.) keeps running" | dim + "\n"] | add // "") + ($e | show)) as $out
  | (if $out == "" then .nl else $out | endswith("\n") end) as $nl
  | status as $s
  | if $s == .status then .out = $out | .nl = $nl
    else .out = $out + (if $nl then "" else "\n" end) + ($s | dim) + "\n" | .nl = true | .status = $s end;
  .out)
