#!/usr/bin/env bash
# Mint a one-time Clerk sign-in ticket for a lake user and write it to a file (never to stdout).
# Clerk's sign-in form consumes it from `?__clerk_ticket=`, so a browser signs in without an email
# code. See docs/deployment/browser-testing.md.
#
#   tools/e2e/clerk-ticket.sh <clerk-user-id> <output-file> [instance: dev|prod]
set -euo pipefail

user_id=${1:?Clerk user id, e.g. user_3JpUKQFkOXWBelPZAiFWshP0OUH}
output=${2:?output file for the ticket}
instance=${3:-dev}

# The Clerk CLI must be linked to the `lake` application; refuse anything else.
linked=$(clerk whoami --mode agent </dev/null | python3 -c 'import json,sys;t=sys.stdin.read();d=json.loads(t[t.find("{"):]);print((d.get("linked") or {}).get("appName",""))')
if [[ "$linked" != "lake" ]]; then
    echo "clerk-ticket: the Clerk CLI is linked to '${linked:-nothing}', not 'lake'; run clerk link --app app_3JpNNpVUwgRgnkItIPbzzRWa2BP" >&2
    exit 2
fi

umask 077
response=$(clerk api --mode agent --instance "$instance" -X POST /sign_in_tokens \
    -d "{\"user_id\":\"$user_id\",\"expires_in_seconds\":600}" --yes </dev/null)
printf '%s' "$response" | python3 -c 'import json,sys;t=sys.stdin.read();d=json.loads(t[t.find("{"):]);sys.stdout.write(d["token"])' >"$output"
echo "clerk-ticket: wrote a ticket for $user_id to $output (valid 10 minutes, one use)"
