"""Exercise real keyboard navigation and collect the SDL RGB565/layout pairs."""
import ctypes as c
import json
import math
import struct
import wave
import os
from pathlib import Path
import subprocess
import time
from PIL import Image
x11 = c.CDLL("libX11.so.6")
xtst = c.CDLL("libXtst.so.6")
x11.XOpenDisplay.restype = c.c_void_p
x11.XOpenDisplay.argtypes = [c.c_char_p]
x11.XStringToKeysym.restype = c.c_ulong
x11.XStringToKeysym.argtypes = [c.c_char_p]
x11.XKeysymToKeycode.restype = c.c_uint
x11.XKeysymToKeycode.argtypes = [c.c_void_p, c.c_ulong]
x11.XSync.argtypes = [c.c_void_p, c.c_int]
x11.XCloseDisplay.argtypes = [c.c_void_p]
xtst.XTestFakeKeyEvent.argtypes = [c.c_void_p, c.c_uint, c.c_int, c.c_ulong]
display = x11.XOpenDisplay(None)
assert display

def key(name, delay=.38, held=.09):
    code = x11.XKeysymToKeycode(display, x11.XStringToKeysym(name.encode()))
    assert code
    for pressed in (1, 0):
        assert xtst.XTestFakeKeyEvent(display, code, pressed, 0)
        x11.XSync(display, 0)
        time.sleep(held if pressed else delay)


binary = Path(os.environ["FILES_AUDIT_BINARY"])
output = Path(os.environ["UX2_OUTPUT"])
base = Path(os.environ["UX2_ROOT"])

def run(*args):
    return subprocess.check_output(args, text=True, stderr=subprocess.STDOUT).strip()

def wait_for(predicate, timeout=5):
    deadline = time.monotonic() + timeout
    while not predicate():
        assert time.monotonic() < deadline
        time.sleep(.02)

def capture(name, footer="browser", delay=.16, expected=None):
    time.sleep(delay)
    stem = "ux2-" + name
    request = output / "request"
    pending = output / "request.tmp"
    pending.write_text(stem + "\n")
    pending.replace(request)
    wait_for(lambda: not request.exists() and (output / (stem + ".json")).is_file()
             and (output / (stem + ".png")).is_file() and (output / (stem + ".pozadi.png")).is_file())
    with Image.open(output / (stem + ".png")) as image:
        snap = image.convert("RGB")
    assert snap.size == (320, 170), snap.size
    document = json.loads((output / (stem + ".json")).read_text(encoding="utf-8"))
    for obj in document["objects"]:
        if obj.get("font"):
            assert obj["font"]["size"] is not None, (name, obj["font"], obj["text"])
    if expected:
        texts = [obj["text"] for obj in document["objects"] if not obj["hidden"] and obj["text"]]
        assert any(expected in text for text in texts), (name, expected, texts)
    if footer:
        window = {"browser": (0, 147, 320, 170), "dialog": (12, 143, 308, 159),
                  "preview": (267, 149, 320, 170)}[footer]
        left, top, right, bottom = window
        ink = [x for x in range(left, right) if any(min(snap.getpixel((x,y))) > 172 for y in range(top,bottom))]
        assert ink and ink[0] >= 10 and ink[-1] <= 309, (name, ink)
        print(f"{name}: footer ink x={ink[0]}..{ink[-1]}", flush=True)

def disk(name):
    img = base/(name + ".img")
    with img.open("wb") as stream:
        stream.truncate(48 * 1024 * 1024)
    run("mkfs.ext4", "-qF", img)
    dev = run("losetup", "--find", "--show", img)
    with (base/"loop-devices").open("a") as log:
        print(dev, file=log)
    path = Path("/media")/name
    path.mkdir()
    return path, dev

def attach(d):
    path, dev = d
    run("mount", "-o", "nodev,nosuid,noexec", dev, path)
    lost = path/"lost+found"
    if lost.exists():
        lost.rmdir()
    return path

def detach(path, lazy=False):
    run("umount", *(["--lazy"] if lazy else []), path)

def deb(target, scripts):
    tree = base/("pkg-" + str(scripts))
    (tree/"DEBIAN").mkdir(parents=True)
    docs = tree/"usr/share/doc/ux2-files-test"
    docs.mkdir(parents=True)
    (docs/"probe.txt").write_text("SDL UI fixture\n")
    (tree/"DEBIAN/control").write_text(
        "Package: ux2-files-test\nVersion: 1.0.0\nArchitecture: all\n"
        "Maintainer: Files Test <files-test@example.invalid>\n"
        "Description: SDL test package\n Contains a text document for the Files UI session.\n")
    if scripts:
        script = tree/"DEBIAN/postinst"
        script.write_text("#!/bin/sh\nexit 0\n")
        script.chmod(0o755)
    run("dpkg-deb", "--build", "--root-owner-group", tree, target)

def launch(name, root, **extra):
    with (base/f"{name}.log").open("w") as log:
        app = subprocess.Popen([binary], stdout=log, stderr=log,
            env=dict(os.environ, FILES_START_DIR=str(root), SDL_VIDEODRIVER="x11",
                     FILES_LAYOUT_AUDIT_DIR=str(output), **extra))
    time.sleep(.75)
    assert app.poll() is None
    return app

def stop(app):
    if app.poll() is None:
        for _ in range(7):
            key("Escape", delay=.055)
        key("Escape", delay=.10, held=1.0)
        try:
            app.wait(timeout=3)
        except subprocess.TimeoutExpired:
            app.terminate()
            app.wait(timeout=4)
            raise
    assert app.returncode == 0, app.returncode

start = base/"start"
start.mkdir()
(start/"Notes").mkdir()
(start/"Notes"/"readme.txt").write_text("Preview USB navigation\n")
deb(start/"balíček.deb", True)
deb(start/"plain.deb", False)
czech = "Příliš žluťoučký kůň úpěl ďábelské ódy.txt"
(start/czech).write_text(czech)
eject, other, copy = [disk(n) for n in ("UX EJECT", "UX OTHER", "UX COPY")]
app = launch("browser", start)
try:
    key("x")
    key("x")
    capture("start-no-disk")
    drive = attach(eject)
    folder = drive/"Podadresář"
    folder.mkdir()
    (folder/"první.txt").write_text("One")
    (folder/"druhý.txt").write_text("Two")
    (drive/"Prázdný").mkdir()
    (drive/"USB dokument.txt").write_text("On the drive")
    time.sleep(.85)
    capture("connected")
    key("Return", delay=.16)
    capture("deb-first-paint", "dialog", .20)
    time.sleep(1.8)
    capture("deb-ready", "dialog")
    key("x")
    capture("deb-scroll", "dialog")
    key("Escape")
    time.sleep(4)
    capture("after-notice")
    key("Return", delay=1.8)
    key("u")
    capture("drive")
    key("Return")
    capture("drive-folder")
    key("Tab", delay=.65)
    capture("drive-menu")
    key("Tab")
    key("e")
    capture("eject-confirm", "dialog")
    with (folder/"první.txt").open() as busy:
        key("Return", delay=.65)
        capture("eject-busy", "dialog")
    key("Escape")
    key("e")
    key("Return", delay=.85)
    capture("eject-success")
    assert not os.path.ismount(drive)

    attach(eject)
    second = attach(other)
    (second/"Other.txt").write_text("Second drive")
    time.sleep(.9)
    key("u")
    time.sleep(5.7)
    capture("multi-usb")
    key("x")
    key("Return")
    key("Return")
    capture("preview-usb", "preview")
    key("u")
    capture("multi-usb-from-preview")
    key("Return")
    detach(second, lazy=True)
    time.sleep(1)
    capture("removed")
    detach(drive, lazy=True)
finally:
    stop(app)

attach(eject)
app = launch("startup-drive", start)
try:
    capture("places")
    key("e")
    capture("places-eject-confirm", "dialog")
    key("Escape")
    key("u")
    key("x")
    key("Return")
    capture("drive-empty")
finally:
    stop(app)
    detach(drive)

app = launch("package", start)
try:
    key("x")
    key("x")
    key("x")
    key("Return", delay=1.9)
    capture("deb-no-scripts", "dialog")
    key("Escape")
    key("f")
    key("f")
    capture("diakritika-seznam")
    key("Tab", delay=.65)
    key("x")
    key("Return")
    capture("diakritika-info", None)
    key("Escape")
    key("Return")
    capture("diakritika", None)
    key("Escape")
    key("x")
    key("Return", delay=1.9)
    auth = base / "require-password"
    auth.touch()
    key("Return", delay=.2)
    capture("install-password", "dialog", expected="System password")
    auth.unlink()
    key("Escape")
    key("Return", delay=1.8)
    key("Return", delay=.08)
    capture("install-progress", "dialog", .15, expected="Installing package")
    time.sleep(2.6)
    capture("install-complete", "dialog", expected="Installation complete")
    assert "1.0.0" in run("dpkg-query", "-W", "ux2-files-test")
    key("Escape")
    key("Tab", delay=.55)
    key("f")
    key("Return")
    capture("places-no-disk")
finally:
    stop(app)

local = base/"destination"
marker = base/"copy-started"
app = launch("copy", local, LD_PRELOAD=str(base/"slow-copy.so"), FILES_COPY_MARKER=str(marker))
try:
    mount = attach(copy)
    orig = mount/"soubor.bin"
    orig.write_bytes(bytes(8 * 1024 * 1024))
    time.sleep(.9)
    key("u")
    key("Tab", delay=.6)
    for _ in range(3):
        key("x", delay=.14)
    key("Return")
    key("Escape")
    key("x")
    key("Return")
    key("Tab", delay=.55)
    key("Return", delay=.08)
    wait_for(marker.exists)
    capture("copy-in-progress", "dialog", .55)
    detach(mount, lazy=True)
    time.sleep(3.25)
    capture("copy-disconnected", "dialog")
    assert not list(local.iterdir()), list(local.iterdir())
    attach(copy)
    assert orig.stat().st_size == 8 * 1024 * 1024
    detach(mount)
finally:
    stop(app)

marker.unlink()
source = local/"na-disk.bin"
source.write_bytes(bytes(8 * 1024 * 1024))
app = launch("copy-to-drive", local, LD_PRELOAD=str(base/"slow-copy.so"), FILES_COPY_MARKER=str(marker))
try:
    key("Tab", delay=.55)
    for _ in range(2):
        key("x", delay=.14)
    key("Return")
    mount = attach(copy)
    time.sleep(.9)
    key("u")
    key("Tab", delay=.6)
    for _ in range(5):
        key("x", delay=.14)
    key("Return", delay=.08)
    wait_for(marker.exists)
    detach(mount, lazy=True)
    time.sleep(3.25)
    capture("copy-destination-disconnected", "dialog")
    assert source.stat().st_size == 8 * 1024 * 1024
    attach(copy)
    assert not (mount/source.name).exists() and not list(mount.glob(".files-transfer-*"))
    detach(mount)
finally:
    stop(app)


def exercise_previews_and_dialogs():
    sample = base / "previews"
    sample.mkdir()
    (sample / "01-example.txt").write_text("Příliš žluťoučký kůň\n", encoding="utf-8")
    artwork = Image.new("RGB", (144, 90), (25, 45, 90))
    artwork.paste((229, 181, 28), (12, 11, 57, 70))
    artwork.save(sample / "02-image.png")
    with wave.open(str(sample / "03-audio.wav"), "wb") as sound:
        sound.setparams((1, 2, 8000, 0, "NONE", "not compressed"))
        sound.writeframes(b"".join(struct.pack("<h", round(12000 * math.sin(i / 12))) for i in range(16000)))
    app = launch("previews", sample)
    try:
        key("Tab", delay=.65)
        capture("file-menu", expected="Open")
        for _ in range(4):
            key("x", delay=.22)
        key("Return", delay=.55)
        capture("rename-dialog", None, expected="Rename file")
        key("Escape", delay=.62)
        key("Tab", delay=.65)
        for _ in range(5):
            key("x", delay=.22)
        key("Return", delay=.55)
        capture("delete-dialog", None, expected="Delete file?")
        key("Escape", delay=.62)
        key("x")
        key("Return", delay=.75)
        capture("image-preview", None)
        key("4")
        capture("image-fullscreen", None)
        key("Escape")
        key("x")
        key("Return", delay=.6)
        capture("audio-preview", None, expected="03-audio")
        key("Escape")
        for _ in range(3):
            key("space", delay=.10)
        capture("magic", None, .27)
    finally:
        stop(app)


exercise_previews_and_dialogs()

x11.XCloseDisplay(display)
print("SDL layout audit scenarios passed", flush=True)
