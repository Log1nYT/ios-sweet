#!/bin/bash
# Polls bridge/to-opencode, runs each task through the bundled opencode CLI, writes the reply to bridge/to-claude.
cd "$(dirname "$0")/../.." || exit 1
OC=/Applications/OpenCode.app/Contents/Resources/opencode-cli
PRE="You are OpenCode, working with Claude Code on the repository $(pwd) through a file mailbox. Read tools/bridge/PROTOCOL.md first and obey it (research and drafts only; no git, no phone, no downloads; never touch anything tied to iOS activation; do not edit xnu/patches or NOTES.md). Reply as plain text; your reply is saved for Claude automatically. The task follows."
while :; do
  for f in bridge/to-opencode/*.md; do
    [ -f "$f" ] || continue
    id=$(basename "$f" .md); mv "$f" "bridge/work/$id.md"
    { printf 'From: opencode\nTo: claude\nRe: %s\nKind: reply\n\n' "$id"
      "$OC" run --standalone --title "bridge:$id" "$PRE

$(cat "bridge/work/$id.md")" 2>&1
    } > "bridge/to-claude/$id.reply.md.tmp"
    mv "bridge/to-claude/$id.reply.md.tmp" "bridge/to-claude/$id.reply.md"
    mv "bridge/work/$id.md" "bridge/done/$id.md"
  done
  sleep 3
done
