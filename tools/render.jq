if   .type == "text_delta"  then .text
elif .type == "tool_start"  then "\n\u001b[2m▸ \(.name) \(.input | tostring)\u001b[0m\n"
elif .type == "message"     then [.message.content[]? | select(.type? == "tool_pending") | "\u001b[2m  ↳ \(.tool_use_id) keeps running\u001b[0m\n"] | add // empty
elif .type == "tool_end"    then "\u001b[2m  ✓ \(.call) \(.status)\(if .is_error then " (error)" else "" end)\u001b[0m\n"
elif .type == "preempt"     then "\n\u001b[33m⏸ interrupted\u001b[0m\n"
elif .type == "notice"      then "\u001b[33m[\(.source)] \(.message)\u001b[0m\n"
elif .type == "error"       then "\u001b[31merror (\(.where)): \(.message)\u001b[0m\n"
elif .type == "turn_end"    then "\n"
elif .type == "run_end" and .reason != "done" then "\u001b[2m[run \(.run) \(.reason)]\u001b[0m\n"
else empty end
