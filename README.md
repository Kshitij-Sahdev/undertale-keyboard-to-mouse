# Mouse Joystick Overlay

Converts your mouse into a joystick that sends arrow keys to a specific window. Built for keyboard-only games (Undertale, etc.) that completely ignore mouse input.

Move mouse away from window center = arrow keys held down. Move back to center = keys released. That's the whole idea.

---

## What it does

- You pick a target window from a list on startup
- From that point on, moving your mouse around that window's center sends arrow keys to it
- A transparent overlay sits on top of the window showing you the joystick position, deadzone, and which keys are currently active
- Mouse clicks also map to keys: left click = Z, right click = X, middle click = C (standard Undertale controls)
- If you alt-tab away, input stops automatically so you don't accidentally spam arrows into whatever you switched to

---

## How the code is structured

Everything is one `.cpp` file split into six sections. No external libraries beyond what ships with Windows.

**Section 1 — Window Targeting**
Walks all visible windows using `EnumWindows`, grabs their title and process name, lets you pick one. After that a background thread polls `GetWindowRect` every 500ms and moves the overlay if the window gets dragged.

**Section 2 — Mouse Input Engine**
Installs a low-level mouse hook (`WH_MOUSE_LL`) that intercepts every mouse move and click system-wide. On each move it calculates the offset from the window center, applies a deadzone (ignores movement within 30px of center), clamps to a max radius, and normalizes everything to a -1 to 1 vector.

**Section 3 — Key Sender Engine**
Takes that vector and translates it to key presses. Keeps track of what's currently held so it only sends a keydown when a key first activates and a keyup when it first deactivates — no spamming the same message every frame. Uses `PostMessage` with `WM_KEYDOWN`/`WM_KEYUP` so it works even when the game window isn't focused.

**Section 4 — Overlay GUI**
A frameless, always-on-top, click-through window that renders with GDI+. Background is magenta which gets color-keyed to transparent by the layered window API, so only the drawn elements show up. Draws a crosshair, deadzone circle, max-radius ring, a vector arrow showing where your mouse is, live X/Y/distance readout, and key badges that light up green when active.

**Section 5 — Window Picker**
Spawns a console on startup, lists all windows, takes your input, then closes the console and gets out of the way.

**Section 6 — Main / Update Loop**
Runs at ~60fps on a worker thread. Each frame: recalculates center, reads mouse state, checks if the target window is in the foreground, updates keys, pushes new state to the overlay. The main thread just runs the Win32 message pump (required for the mouse hook to work).

---

## Controls

| Key | Action |
|-----|--------|
| F8 | Pause/resume input |
| F9 | Recenter (snaps back to window center) |
| ESC | Exit |

---

## Building

You need MinGW-w64. If you don't have it, grab the standalone version from winlibs.com — just unzip it, no install required.

Then either run the batch file:

```
build.bat
```

Or manually:

```
g++ -O2 -std=c++20 -mwindows -o MouseJoystick.exe MouseJoystick.cpp -luser32 -lgdi32 -lgdiplus -lpsapi -ldwmapi
```

Output is a single `MouseJoystick.exe`. No DLLs, no installer, no registry. Works on any Windows 10/11 machine, just copy and run.

---

## Notes

The deadzone is 30px and max radius is 200px. These are hardcoded right now but easy to change at the top of the mouse engine section. If the overlay feels too big or small, that's because it matches the exact size of your target window — intentional, it needs to line up with the center point.

The key sending uses `PostMessage` by default which works for most games. If a game uses DirectInput and ignores posted messages, there's a `SendInput` path in the key sender that you can switch to — it requires the game window to be in the foreground but is more compatible with lower-level input systems.