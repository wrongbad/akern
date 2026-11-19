def dim: "\u001b[2m\(.)\u001b[0m";
def blocks($t): [select(.type == "message") | .message.content[]? | select(.type? == $t) | .tool_use_id];
def running: "\(.) job\(if . == 1 then "" else "s" end) running";

def show:
  if .type == "text_delta"  then .text
  elif .type == "tool_start"  then "\n" + ("▸ \(.name) \(.input | tostring)" | dim) + "\n"
  elif .type == "message"     then [blocks("tool_pending")[] | "  ↳ \(.) keeps running" | dim + "\n"] | add // ""
  elif .type == "tool_end"    then ("  ✓ \(.call) \(.status)\(if .is_error then " (error)" else "" end)" | dim) + "\n"
  elif .type == "preempt"     then "\n\u001b[33m⏸ interrupted\u001b[0m\n"
  elif .type == "interrupt"   then "\n" + ("(press Ctrl-C again within \(.quit_within)s to quit)" | dim) + "\n"
  elif .type == "notice"      then "\u001b[33m[\(.source)] \(.message)\u001b[0m\n"
  elif .type == "error"       then "\u001b[31merror\(if .where then " (\(.where))" else "" end): \(.message)\u001b[0m\n"
  elif .type == "turn_end"    then "\n"
  elif .type == "run_end" and .reason != "done" then ("[run \(.run) \(.reason)]" | dim) + "\n"
  else "" end;

def status:
  if .run then "… working"
  elif (.jobs | length) > 0 then "⏵ \(.jobs | length | running) · waiting for input"
  else "› waiting for input" end;

foreach inputs as $e ({run: ($ARGS.named.prompted == "true"), jobs: [], status: null, nl: true};
  .run = (if $e.type == "run_start" then true elif $e.type == "run_end" then false else .run end)
  | .jobs = (.jobs + ($e | blocks("tool_pending")) - ($e | blocks("job_result")) | unique)
  | ($e | show) as $out
  | (if $out == "" then .nl else $out | endswith("\n") end) as $nl
  | status as $s
  | if $s == .status then .out = $out | .nl = $nl
    else .out = $out + (if $nl then "" else "\n" end) + ($s | dim) + "\n" | .nl = true | .status = $s end;
  .out)
