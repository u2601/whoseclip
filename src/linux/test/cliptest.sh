#!/usr/bin/env bash
# Puts synthetic payloads on the clipboard and checks what WhoseClip makes of
# each one, so the capture paths can be exercised without a password manager, a
# browser or a VM.
#
#   make test                                 build and run every case
#   src/linux/test/cliptest.sh                the same, run by hand
#   src/linux/test/cliptest.sh text image     only the named cases
#
# Run it from the top of the repository: the paths below are relative to there.
# With no DISPLAY it starts its own Xvfb, so it works over a plain ssh session.
#
# The payloads are served by holdsel, next to this file, rather than by xclip.
# Both xclip and xsel fork after connecting to X, which leaves the server holding
# a process id that has already exited, and the owner is the thing most worth
# testing.

set -u

BIN="${WHOSECLIP:-build/linux/whoseclip}"
HOLD="build/linux/holdsel"
TMP="$(mktemp -d)"
PASS=0
FAIL=0
XVFB_PID=""
HS_PID=""

cleanup() {
    [ -n "$HS_PID" ]   && kill "$HS_PID"   2>/dev/null
    [ -n "$XVFB_PID" ] && kill "$XVFB_PID" 2>/dev/null
    rm -rf "$TMP"
}
trap cleanup EXIT

[ -x "$BIN" ]  || { echo "No binary at $BIN.  Run make first." >&2; exit 1; }
[ -x "$HOLD" ] || { echo "No fixture at $HOLD.  Run make holdsel first." >&2; exit 1; }

# A display of our own, so a headless box works and a real session is left alone.
if [ -z "${DISPLAY:-}" ]; then
    command -v Xvfb >/dev/null 2>&1 || { echo "No DISPLAY and no Xvfb.  sudo apt install xvfb" >&2; exit 1; }
    Xvfb :99 -screen 0 1280x800x24 >"$TMP/xvfb.log" 2>&1 &
    XVFB_PID=$!
    sleep 2
    export DISPLAY=:99
    echo "started Xvfb on :99"
fi

serve() {                       # serve FILE [MIME...]
    [ -n "$HS_PID" ] && { kill "$HS_PID" 2>/dev/null; wait "$HS_PID" 2>/dev/null; }
    "$HOLD" "$@" >/dev/null 2>&1 &
    HS_PID=$!
    sleep 0.5
}

drop() {
    [ -n "$HS_PID" ] && { kill "$HS_PID" 2>/dev/null; wait "$HS_PID" 2>/dev/null; HS_PID=""; }
    sleep 0.3
}

OUT=""
probe() { OUT="$("$BIN" --probe 2>/dev/null)"; }

want() {                        # want "description" "regex"
    if printf '%s' "$OUT" | grep -qE "$2"; then
        echo "    PASS  $1"
        PASS=$((PASS + 1))
    else
        echo "    FAIL  $1"
        echo "          expected to match: $2"
        FAIL=$((FAIL + 1))
    fi
}

head_case() { echo; echo "== $1"; }

case_empty() {
    head_case "empty clipboard"
    drop
    probe
    want "kind is EMPTY"            '^Kind: EMPTY'
    want "says nothing owns it"     '^Origin: nothing owns the clipboard'
}

case_text() {
    head_case "short text"
    printf 'the quick brown fox' > "$TMP/t.txt"
    serve "$TMP/t.txt"
    probe
    want "kind is TEXT"             '^Kind: TEXT'
    want "owner resolves to holdsel" '^Owner: holdsel \(pid [0-9]+\)'
    want "owner path is reported"   '^Owner path: .*holdsel'
    want "origin is local"          '^Origin: copied here'
    want "size is 19 bytes"         '^Size: 19 B'
}

case_multiline() {
    head_case "multi-line text, blank-line and whitespace collapsing"
    printf 'first line\nsecond line\n\n\nfourth after blanks\n   spaced   out\n' > "$TMP/m.txt"
    serve "$TMP/m.txt"
    probe
    want "kind is TEXT"             '^Kind: TEXT'
    want "owner resolves"           '^Owner: holdsel'
}

case_unicode() {
    head_case "unicode, 2 3 and 4 byte sequences"
    printf 'caf\xc3\xa9 \xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e \xf0\x9f\x8e\xaf done\n' > "$TMP/u.txt"
    serve "$TMP/u.txt"
    probe
    want "kind is TEXT"             '^Kind: TEXT'
    want "owner resolves"           '^Owner: holdsel'
}

case_invalid() {
    head_case "malformed UTF-8 (must not blank the strip)"
    printf 'valid then \xff\xfe\xfd then more\n' > "$TMP/bad.txt"
    serve "$TMP/bad.txt"
    probe
    want "kind is TEXT"             '^Kind: TEXT'
    want "still resolves the owner" '^Owner: holdsel'
}

case_large() {
    head_case "large text, preview bounded but full size reported"
    head -c 300000 /dev/urandom | base64 | head -c 250000 > "$TMP/big.txt"
    serve "$TMP/big.txt"
    probe
    want "kind is TEXT"             '^Kind: TEXT'
    want "size reported in KB"      '^Size: [0-9.]+ KB'
    want "owner resolves"           '^Owner: holdsel'
}

case_image() {
    head_case "image, dimensions from the PNG header"
    # A PNG header declaring 3x2. WhoseClip reads only the first 24 bytes of an
    # image, so a real encoder would prove something this does not need to prove.
    printf '\x89PNG\r\n\x1a\n'                                   > "$TMP/i.png"
    printf '\x00\x00\x00\x0dIHDR'                               >> "$TMP/i.png"
    printf '\x00\x00\x00\x03\x00\x00\x00\x02\x08\x02\x00\x00\x00' >> "$TMP/i.png"
    printf '\x00\x00\x00\x00'                                   >> "$TMP/i.png"
    printf '\x00\x00\x00\x00IEND\xae\x42\x60\x82'               >> "$TMP/i.png"
    serve "$TMP/i.png" image/png
    probe
    want "kind is IMAGE with size"  '^Kind: IMAGE 3x2'
    want "owner resolves"           '^Owner: holdsel'
}

case_files() {
    head_case "file list, percent-decoded"
    touch "$TMP/alpha.txt" "$TMP/beta.txt" "$TMP/gamma with space.txt"
    printf 'file://%s/alpha.txt\nfile://%s/beta.txt\nfile://%s/gamma%%20with%%20space.txt\n' \
        "$TMP" "$TMP" "$TMP" > "$TMP/uris.txt"
    serve "$TMP/uris.txt" text/uri-list
    probe
    want "kind is FILES with a count" '^Kind: FILES \(3\)'
    want "owner resolves"             '^Owner: holdsel'
}

case_gnome_files() {
    head_case "gnome file cut, drop effect should read move"
    touch "$TMP/one.txt" "$TMP/two.txt"
    printf 'cut\nfile://%s/one.txt\nfile://%s/two.txt' "$TMP" "$TMP" > "$TMP/gn.txt"
    serve "$TMP/gn.txt" x-special/gnome-copied-files
    probe
    want "kind is FILES (2)"        '^Kind: FILES \(2\)'
    want "verb read as move"        '^Drop effect: move'
}

case_secret() {
    head_case "password-manager hint, recorded but content still shown"
    printf 'correct horse battery staple' > "$TMP/s.txt"
    serve "$TMP/s.txt" UTF8_STRING x-kde-passwordManagerHint
    probe
    want "kind is TEXT"             '^Kind: TEXT'
    want "hint is recorded"         '^Source asked not to be monitored'
    want "owner resolves"           '^Owner: holdsel'
}

ALL="empty text multiline unicode invalid large image files gnome_files secret"
WANT="${*:-$ALL}"

echo "WhoseClip clipboard test"
echo "  binary:  $BIN"
echo "  session: ${XDG_SESSION_TYPE:-unknown}   display: ${DISPLAY:-none}   wayland: ${WAYLAND_DISPLAY:-none}"

for c in $WANT; do
    if ! declare -F "case_$c" >/dev/null; then
        echo "unknown case: $c   (have: $ALL)" >&2
        continue
    fi
    "case_$c"
done

echo
echo "============================================"
echo "  passed $PASS, failed $FAIL"
echo "============================================"
[ "$FAIL" -eq 0 ]
