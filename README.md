# WhoseClip

If you work across a host and a few VMware guests, you've probably pasted the
wrong thing at least once. VMware hands the clipboard between host and guest
whenever a window takes focus. So what you copied a minute ago on one machine is
often not what's sitting on the clipboard of the machine you just clicked into.

That's the whole problem this solves. WhoseClip sits in the corner of each
machine and tells you what's on *that* machine's clipboard, and which process put
it there.

![WhoseClip, collapsed and expanded](media/strip.png)

Run one copy per machine. The accent colour means the same thing everywhere and
tells you where the content came from: blue for copied on this machine, orange
for arrived from another one, grey for nothing there.

It's not a clipboard manager. There's no history, and no clipboard content is
ever saved.

---

## Get it

Everything is in the **[`release/`](release/)** folder. Take the one for the
machine you're on and run it. No installer, nothing to register.

GitHub shows a preview page for binaries rather than downloading them, so use the
**Download raw file** button, the icon at the top right of the file view.

**Windows** - [`whoseclip-x64.exe`](release/whoseclip-x64.exe), or
[`whoseclip-x86.exe`](release/whoseclip-x86.exe) if you have 32-bit guests. One
file, no runtime, Windows Vista or later. It's unsigned, so SmartScreen will
probably warn you the first time.

**Linux** - [`whoseclip-linux-x64`](release/), then:

```
chmod +x whoseclip-linux-x64 && ./whoseclip-linux-x64
```

It uses the GTK 3 your desktop already has, so Ubuntu, Xubuntu, Mint or Debian
from 2022 onward. On Xorg, which is what Xubuntu and Mint give you by default, it
does the whole job; on Wayland it shows what's on the clipboard but not which
application put it there, for the reason under [On Linux](#on-linux).

**Starting it with the machine.** On Windows, put a shortcut to the exe in the
Startup folder: press Win+R and enter `shell:startup`. On Linux, write one
desktop entry, and delete that same file to undo it:

```
mkdir -p ~/.config/autostart
cat > ~/.config/autostart/whoseclip.desktop <<'EOF'
[Desktop Entry]
Type=Application
Name=WhoseClip
Exec=whoseclip
Terminal=false
EOF
```

Labels, colours and where the strip sits are all optional, and live in
[Options](#options) or in `whoseclip.ini` beside the binary.

## Using it

The tray icon carries the accent colour and the first letter of the label, so
you can read the state without opening anything.

| Action | What it does |
| --- | --- |
| Hover the tray icon | the whole readout, as a tooltip |
| Left-click the tray icon | show or hide the strip |
| Right-click the icon or the strip | menu: expand, pause, list formats, copy diagnostics, exit |
| Chevron, or double-click the strip | expand or collapse |
| The x | hide the strip; the tray icon stays behind |
| Drag anywhere else on the strip | move it, and it remembers where |

Collapsed is the glance: which machine, what kind of thing, which process, a
one-line preview, how long ago.

Expanded is for reading. Full origin, full path of the owning process, the whole
preview with its line breaks intact, every file in a multi-file copy with its own
size, and the clipboard formats. Nothing is cut off.

## Copying files out of a VM

A file copied from a guest doesn't exist on the host yet, so Windows can't give
you a path for it. You get a descriptor instead: names and sizes, with the bytes
streamed across only when you paste.

WhoseClip reads that descriptor, so a file dragged out of a VM shows up as:

```
DEV-VM   FILES (1) virtual   4.6 MB   via vmtoolsd.exe
payload_x64.exe
```

That word `virtual` is the giveaway that nothing has actually crossed yet.
Expand the strip to see every file in the set with its own size.

If a file copy ever shows up as `OTHER` with a `DataObject` format, expand the
strip and look at the format list along the bottom. It means the source is using
a shape WhoseClip doesn't handle yet.

## Turning off VMware clipboard sharing

Worth knowing even if you keep it on: everything you copy on the host lands in
the clipboard of whichever VM you click into next. If you'd rather it didn't, use
**VM Settings > Options > Guest Isolation**, or add this to the `.vmx`:

```
isolation.tools.copy.disable = "TRUE"
isolation.tools.paste.disable = "TRUE"
```

---

## Reference

### Options

| Argument | ini key | Meaning |
| --- | --- | --- |
| `--label NAME` | `label` | Machine name shown in the UI. Defaults to the computer name. |
| `--color-local RRGGBB` | `colorLocal` | Accent for content copied on this machine |
| `--color-foreign RRGGBB` | `colorForeign` | Accent for content that came from elsewhere |
| `--strip` / `--no-strip` | `showStrip` | Show the strip at startup |
| `--expanded` / `--collapsed` | `expanded` | Start with the strip expanded |
| `--preview N` | `previewChars` | Characters of preview to keep. Default 1500, max 4088. |

The ini takes a few more that have no argument: `colorIdle` for an empty or
paused clipboard, `colorBg`, `colorFg` and `colorDim` for the strip itself,
`stripX` and `stripY` for its position, and `extraExternals` if you want to add
your own relay process names.

The colour options are there for taste and for eyesight, not for telling
machines apart. Set them the same on every machine or the accent stops meaning
anything. Blue against orange is the default because it survives the common
forms of colour blindness, which red against green does not.

### On Linux

The Linux build reads the clipboard over X11, because X11 is the only one of the
two display protocols that lets a program ask who owns the clipboard, and that is
this program's whole premise. Wayland deliberately doesn't: it hands clipboard
data to the focused window alone and carries no notion of a source application.

So on an Xorg session you get everything, and Xubuntu and Mint log you into Xorg
by default. On a Wayland session the content still reaches you over XWayland, and
anything copied from an X11 application is identified in full. Only something
copied from a Wayland-native application reads `owner hidden by Wayland`, with
cross-machine detection off for that item. Stock Ubuntu on GNOME defaults to
Wayland; "Ubuntu on Xorg" on the login screen gets you the rest.

`whoseclip --probe` prints one reading and exits, naming the session it found and
what that costs. It is the quickest way to see where you stand.

The tray icon goes through Ayatana AppIndicator. Ubuntu ships it, and Xfce 4.18
speaks both the modern StatusNotifier protocol and the older XEmbed tray, so the
icon reaches the panel either way. Without the library the strip becomes the
whole interface, carrying the same menu on right-click.

Settings live in `~/.config/whoseclip/whoseclip.ini`, in the same format as the
Windows one, so a config copied across works unchanged. A `whoseclip.ini` beside
the binary wins over that, which keeps a copied folder self-contained.

### What it reads

| Signal | Where it comes from |
| --- | --- |
| Owning process | `GetClipboardOwner`, then `QueryFullProcessImageNameW` |
| Came from another machine | the owner is a known relay: `vmtoolsd.exe`, `vmware-vmx.exe`, `rdpclip.exe`, `VBoxTray.exe` and a few others |
| Originating web page | the `SourceURL:` header inside `CF_HTML` |
| Kind and size | `CF_UNICODETEXT`, `CF_HDROP`, `CF_DIB`, registered formats |
| Virtual file names and sizes | `FileGroupDescriptorW` |
| Copy or move | `Preferred DropEffect` |
| Source asked not to be monitored | `ExcludeClipboardContentFromMonitorProcessing`, `CanIncludeInClipboardHistory`, `CanUploadToCloudClipboard`. Reported in the diagnostics dump, never acted on. |
| Who is blocking the clipboard | `GetOpenClipboardWindow`, when `OpenClipboard` fails |

Windows doesn't record where clipboard content came from, so "came from another
machine" is inferred from that relay list rather than read from anywhere.
`SourceURL` is the only origin the OS genuinely carries.

On Linux the same signals come from different places:

| Signal | Where it comes from |
| --- | --- |
| Owning process | `XGetSelectionOwner`, then the X-Resource extension maps that window to a pid, falling back to `_NET_WM_PID` up the window tree |
| Process name | `/proc/<pid>/exe`, falling back to `/proc/<pid>/comm` |
| Came from another machine | the owner is a known relay: `vmtoolsd`, `VBoxClient`, `spice-vdagent`, `xrdp-chansrv` and a few others |
| Clipboard changed | `XFIXES` selection-owner notifications, so there's no polling loop |
| Kind and size | the `TARGETS` list, then one bounded conversion. `XGetWindowProperty` reports what it didn't transfer, so a large image is sized without being copied |
| Originating web page | `text/x-moz-url`. X11 has no equivalent of the `SourceURL` header, so this is the only provenance a browser offers |
| Copy or move | the verb on the first line of `x-special/gnome-copied-files` |
| Source asked not to be monitored | `x-kde-passwordManagerHint`. Reported in the diagnostics dump, never acted on. |
| Owner won't answer | a conversion request that times out, which is the closest thing X11 has to a held clipboard |

The relay list is the same idea as the Windows one and rests on the same
assumption: that the guest tools daemon is what owns the selection after the
host pushes the clipboard across.

### What it doesn't do

Clipboard content never touches disk. The only file it writes is
`whoseclip.ini`, which holds the strip position, visibility and expanded state.
The Linux build also writes three tray icons, each a coloured square with a
letter on it, to the runtime directory, because AppIndicator takes an icon by
filename rather than as a bitmap.

There's no network code in the binary at all, which you can check yourself:
`dumpbin /imports release\whoseclip-x64.exe` lists four system DLLs and nothing
else.

Reads are bounded to `previewChars` characters. Anything larger gets measured
rather than read, and the preview buffer is wiped before reuse and on exit.

It also won't hold the clipboard open for longer than it takes to read that
preview, and capture waits 120 ms after a change so it doesn't collide with
whatever application is still writing.

### Building it yourself

On Windows you need Visual Studio 2022, or just the Build Tools, with the
Windows SDK.

```
build.cmd         both architectures
build.cmd x64     64-bit only
build.cmd x86     32-bit only
```

Output lands in `build\x64\whoseclip.exe` and `build\x86\whoseclip.exe`.

On Linux, `make check` names any development packages you are missing and
`make deps` prints the apt line that installs them. Then:

```
make             builds build/linux/whoseclip
make install     copies it to ~/.local/bin
make NO_TRAY=1   builds without the tray icon
```

GTK 3.22 or later is required, which covers every supported Ubuntu. If
`make install` had to create `~/.local/bin`, it will not be on your `PATH` until
you log in again, because Ubuntu adds that directory at login only when it
already exists.

### Testing

`make test` puts synthetic payloads on the clipboard and checks what WhoseClip
makes of each one, so you can confirm a build works on your machine without
needing a password manager, a browser, or a VM. It starts its own Xvfb if you
have no display, which means it also runs over a plain ssh session.

```
make test
  ...
  passed 25, failed 0
```

Twenty-five assertions over an empty clipboard, plain and multi-line text,
unicode across two, three and four byte sequences, deliberately malformed UTF-8,
a payload too large to preview, an image sized from its header, a file list with
percent-encoded names, a cut rather than a copy, and a password-manager hint.
Every case checks the owning process as well as the content, because the owner
is the part most likely to differ between one desktop and the next.

The payloads are served by `src/linux/test/holdsel.c` rather than by xclip.
Both xclip and xsel fork after opening their X connection, which leaves the
server holding the process id of something that has already exited, so neither
can be used to test whether an owner is identified correctly.

## License

MIT. See [LICENSE](LICENSE). Written by u2601.
