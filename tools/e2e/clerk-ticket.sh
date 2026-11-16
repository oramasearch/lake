#!/usr/bin/env bash
# SPDX-License-Identifier: MIT

# Clerk test users and one-time sign-in tickets for the browser scripts, through Clerk's Backend API. Clerk's sign-in form consumes a ticket from
# `?__clerk_ticket=`, so a browser signs in without an email code.
#
# The instance's secret key comes from the environment, never from an argument or a file:
#
#   export CLERK_SECRET_KEY=...         # the test instance's secret key, from where you keep it
#   export CLERK_INSTANCE=ins_...       # optional: refuse any other instance
#
#   tools/e2e/clerk-ticket.sh create-user <email> <user-id-file>   # a verified test user
#   tools/e2e/clerk-ticket.sh ticket <clerk-user-id> <ticket-file>  # valid 10 minutes, one use
#   tools/e2e/clerk-ticket.sh delete-user <clerk-user-id>
#
# Tickets and ids are written to files with mode 600; the key and tickets are never printed.
set -euo pipefail

: "${CLERK_SECRET_KEY:?set CLERK_SECRET_KEY to the instance secret key (see the header)}"
api=https://api.clerk.com/v1
umask 077

call() { # method path [json]
    local args=(-sS -X "$1" -H "Authorization: Bearer $CLERK_SECRET_KEY" -H "Content-Type: application/json")
    [[ $# -ge 3 ]] && args+=(-d "$3")
    curl "${args[@]}" "$api$2"
}
field() { python3 -c 'import json,sys;d=json.load(sys.stdin);v=d.get(sys.argv[1]);sys.exit("clerk-ticket: Clerk answered "+json.dumps(d.get("errors") or d)[:300]) if v is None else sys.stdout.write(str(v))' "$1"; }

# The key decides the instance; refuse to act on any but the one named.
if [[ -n "${CLERK_INSTANCE:-}" ]]; then
    instance=$(call GET /instance | field id)
    if [[ "$instance" != "$CLERK_INSTANCE" ]]; then
        echo "clerk-ticket: the key belongs to $instance, not $CLERK_INSTANCE; refusing" >&2
        exit 2
    fi
fi

case "${1:-}" in
create-user)
    email=${2:?email, e.g. e2e-1+clerk_test@example.com}
    output=${3:?output file for the Clerk user id}
    call POST /users "{\"email_address\":[\"$email\"],\"first_name\":\"E2e\",\"last_name\":\"Test\",\"skip_password_requirement\":true}" \
        | field id >"$output"
    echo "clerk-ticket: created $(cat "$output") ($email)"
    ;;
ticket)
    user_id=${2:?Clerk user id}
    output=${3:?output file for the ticket}
    call POST /sign_in_tokens "{\"user_id\":\"$user_id\",\"expires_in_seconds\":600}" | field token >"$output"
    echo "clerk-ticket: wrote a ticket for $user_id to $output (valid 10 minutes, one use)"
    ;;
delete-user)
    user_id=${2:?Clerk user id}
    call DELETE "/users/$user_id" | field deleted >/dev/null
    echo "clerk-ticket: deleted $user_id"
    ;;
*)
    sed -n '2,17p' "$0" >&2
    exit 2
    ;;
esac
