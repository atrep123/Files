"""Exercise real keyboard navigation and collect the SDL RGB565/layout pairs."""
import ctypes as c
import fcntl
import json
import math
import re
import struct
import wave
import os
import shutil
import shlex
from pathlib import Path
import subprocess
import time
import traceback
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

def key(name, delay=.075, held=.07):
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

def capture(name, delay=.11, expected=None, *, keep=True):
    time.sleep(delay)
    stem = f"{name}-{language}"
    request = output / "request"
    pending = output / "request.tmp"
    deadline = time.monotonic() + 8
    expected = (expected,) if isinstance(expected, str) else expected or ()
    while True:
        pending.write_text(stem + "\n")
        pending.replace(request)
        wait_for(lambda: not request.exists() and (output / (stem + ".json")).is_file()
                 and (output / (stem + ".png")).is_file() and (output / (stem + ".pozadi.png")).is_file())
        document = json.loads((output / (stem + ".json")).read_text(encoding="utf-8"))
        texts = [obj["text"] for obj in document["objects"] if not obj["hidden"] and obj["text"]]
        if all(any(pattern in text for text in texts) for pattern in expected):
            break
        assert time.monotonic() < deadline, (name, expected, texts)
        time.sleep(.10)
    with Image.open(output / (stem + ".png")) as image:
        snap = image.convert("RGB")
    assert snap.size == (320, 170), snap.size
    if name == "video-playing":
        assert snap.crop((8, 24, 312, 146)).getbbox(), "SDL video frame has no pixels"
    if name == "audio-usb":
        clocks = [obj["ink"] for obj in document["objects"] if not obj["hidden"] and "0:" in (obj["text"] or "")]
        assert len(clocks) == 2 and all(y + height <= 126 for _, y, _, height in clocks), clocks
    for obj in document["objects"]:
        if obj.get("font"):
            assert obj["font"]["size"] is not None, (name, obj["font"], obj["text"])
    if keep:
        print(f"captured {stem}", flush=True)
    else:
        for suffix in (".json", ".png", ".pozadi.png"):
            (output / (stem + suffix)).unlink()
    return document


def assert_selected(document, expected):
    visible = [obj for obj in document["objects"] if not obj["hidden"]]
    selected = {obj["id"] for obj in visible if obj["selected"]}
    assert any(obj["parent"] in selected and expected in (obj["text"] or "") for obj in visible), expected


def disk(name):
    img = base/(name + ".img")
    with img.open("wb") as stream:
        stream.truncate(48 * 1024 * 1024)
    run("mkfs.ext4", "-qF", img)
    with open("/tmp/files-layout-audit-loop.lock", "a") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
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

def deb(target, scripts, architecture="all", depends=""):
    tree = base/("pkg-" + target.stem)
    (tree/"DEBIAN").mkdir(parents=True)
    docs = tree/"usr/share/doc/ux2-files-test"
    docs.mkdir(parents=True)
    (docs/"probe.txt").write_text("SDL UI fixture\n")
    (tree/"DEBIAN/control").write_text(
        f"Package: ux2-files-test\nVersion: 1.0.0\nArchitecture: {architecture}\n"
        "Maintainer: Files Test <files-test@example.invalid>\n" +
        (f"Depends: {depends}\n" if depends else "") +
        "Description: Příručka pro Files / Files manual\n" +
        "".join(f" Section {i} describes copying files and safely ejecting a USB drive.\n" for i in range(12)))
    if scripts:
        script = tree/"DEBIAN/postinst"
        script.write_text("#!/bin/sh\nexit 0\n")
        script.chmod(0o755)
    run("dpkg-deb", "--build", "--root-owner-group", tree, target)

def launch(name, root, user=None, **extra):
    with (base/f"{name}.log").open("w") as log:
        executable = Path("/usr/share/APPLaunch/bin/M5CardputerZero-Files") if user else binary
        auth = os.environ["XAUTHORITY"]
        if user:
            readable = base/"xauthority"
            shutil.copyfile(auth, readable)
            readable.chmod(0o644)
            auth = str(readable)
        app = subprocess.Popen([executable], stdout=log, stderr=log,
            user=user, env={**os.environ, "FILES_START_DIR": str(root), "SDL_VIDEODRIVER": "x11",
                 "LC_ALL": "C.UTF-8", "XDG_CONFIG_HOME": str(config), "SDL_AUDIODRIVER": "dummy",
                 "XAUTHORITY": auth, "FILES_LAYOUT_AUDIT_DIR": str(output), **extra})
    time.sleep(.45)
    assert app.poll() is None, f"{name}: SDL exited with {app.returncode}"
    return app

def stop(app):
    if app.poll() is None:
        for _ in range(12):
            key("Escape", delay=.055)
        key("Escape", delay=.10, held=1.0)
        try:
            app.wait(timeout=3)
        except subprocess.TimeoutExpired:
            app.terminate()
            app.wait(timeout=4)
            raise
    assert app.returncode == 0, app.returncode


def down(n):
    for _ in range(n):
        key("x")


def action(index, from_end=False):
    key("Tab")
    for _ in range(index):
        key("f" if from_end else "x")
    key("Return")


def say(en, cs):
    return cs if language == "cs" else en


def type_ascii(text):
    for ch in text:
        key(ch, delay=.035, held=.045)


def fill_name(text, erase=0):
    for _ in range(erase):
        key("BackSpace", delay=.035, held=.045)
    type_ascii(text)


def clear_fixture(name):
    (Path(os.environ["UX2_ROOT"])/name).unlink(missing_ok=True)


def capture_while_held(name, keyname, expected):
    code = x11.XKeysymToKeycode(display, x11.XStringToKeysym(keyname.encode()))
    assert xtst.XTestFakeKeyEvent(display, code, 1, 0)
    x11.XSync(display, 0)
    try:
        capture(name, expected=expected)
    finally:
        assert xtst.XTestFakeKeyEvent(display, code, 0, 0)
        x11.XSync(display, 0)
        time.sleep(.14)


def exercise_browser():
    sample = base/"browser"
    sample.mkdir()
    (sample/"01-empty").mkdir()
    plenty = sample/"02-plenty"
    plenty.mkdir()
    (plenty/".skrytý").write_text("Hidden")
    for i in range(55):
        (plenty/f"{i:02d} — File with a long distinguishing suffix v{i:02d}.pdf").write_bytes(bytes(850 + i))
    czech = "Příliš žluťoučký kůň úpěl ďábelské ódy.txt"
    (sample/czech).write_text(czech)
    (sample/"zz-binary.bin").write_bytes(bytes(64))
    top = base/"special-first"
    top.mkdir()
    os.mkfifo(top/"00-special")
    for i in range(1, 6):
        (top/f"0{i}.txt").write_text("Visible neighbor")
    os.mkfifo(sample/"zz-special")
    app = launch("browser", sample)
    try:
        capture("startup-without-usb", expected="01-empty")
        key("Return")
        capture("empty-folder", expected=say("Empty folder", "Prázdná složka"))
        key("Tab")
        capture("empty-menu", expected=say("New folder", "Nová složka"))
        key("Escape")
        key("Escape")
        key("x")
        key("Return")
        capture("hidden-file", expected=".skrytý")
        down(10)
        capture("many-items", expected="09")
        key("End")
        key("Page_Down")
        capture("many-items-paged", expected="13")
        for _ in range(11):
            key("Page_Down")
        end = capture("many-items-end", expected=("v54.pdf", "904 B"))
        assert_selected(end, "v54.pdf")
        action(5)
        key("Escape")
        assert_selected(capture("delete-cancelled-scrolled", expected="v54.pdf"), "v54.pdf")
        key("Escape")
        key("x")
        capture("czech-filename", expected="Příliš žluť")
        key("Tab")
        capture("file-menu", expected=say("Details", "Podrobnosti"))
        key("f")
        assert_selected(capture("file-menu-end", expected=say("Language", "Jazyk")), say("Language", "Jazyk"))
        key("x")
        key("x")
        key("Return")
        capture("file-details", expected=czech)
        key("x")
        capture("file-details-scrolled", expected=czech)
        key("Page_Down")
        capture("file-details-end", expected=czech)
        key("Escape")
        key("Return")
        capture("text-preview", expected=czech)
        key("Escape")
        key("h")
        capture("help", expected=say("Help", "Nápověda"))
        key("Escape")
        key("x")
        key("Return")
        capture("binary-details", expected=say("Details", "Podrobnosti"))
        key("Escape")
        key("x")
        key("Return")
        capture("preview-unavailable", expected=say("No preview", "Náhled není dostupný"))
        action(3, from_end=True)
        capture("places-no-usb", expected=say("Home", "Domů"))
        capture_while_held("exit-hint", "Escape", say("Hold Esc", "Podržet Esc"))
        key("Tab")
        capture("places-local-menu", expected=say("Language", "Jazyk"))
        key("x")
        key("Return")
        capture("language-menu", expected="Čeština")
        key("f" if language == "cs" else "x")
        capture("language-other-focused", expected="Čeština")
        key("x" if language == "cs" else "f")
        # Opening the menu highlights the saved locale, so selecting it checks persistence.
        key("Return")
        assert (config/"cardputer-files/language").read_text().strip() == language
        key("Return")
        capture("home", expected=say("Home", "Domů"))
        key("Escape")
        key("x")
        key("Return")
        capture("file-system", expected=say("File system", "Souborový systém"))
    finally:
        stop(app)

    system_config = config/"cardputer-files/language"
    app = launch("system-language", sample, LC_ALL="", LC_MESSAGES=say("en_US.UTF-8", "cs_CZ.UTF-8"))
    try:
        action(1, from_end=True)
        key("f" if language == "en" else "x")
        key("Return")
        assert system_config.read_text().strip() == "system"
        action(1, from_end=True)
        key("x")
        assert_selected(capture("language-system-saved", expected=say("System language", "Systémový jazyk")), "English")
    finally:
        stop(app)
        system_config.write_text(language + "\n")

    multiline = base/"multiline-details"
    multiline.mkdir()
    long_name = "Dlouhý název souboru s více samostatnými slovy a viditelným koncem v03.txt"
    (multiline/long_name).write_text("Details wrap above the first field.")
    app = launch("multiline-details", multiline)
    try:
        action(1)
        capture("file-details-multiline", expected=("v03.txt", say("Kind", "Typ")))
    finally:
        stop(app)

    app = launch("special-first", top)
    try:
        key("Return")
        capture("preview-unavailable-first", expected="00-special")
        down(4)
        attach(other)
        time.sleep(.9)
        capture("usb-connected-fifth-row", expected="UX OTHER")
    finally:
        stop(app)
        if os.path.ismount(other[0]):
            detach(other[0])


def exercise_name_and_delete():
    sample = base/"edit"
    sample.mkdir()
    (sample/"01.txt").write_text("Editable")
    (sample/"02.txt").write_text("Existing")
    app = launch("edit", sample)
    try:
        action(4)
        capture("rename", expected=say("Rename", "Přejmenovat"))
        fill_name("02", 6)
        key("period")
        type_ascii("txt")
        key("Return")
        capture("rename-conflict", expected=say("Name already exists", "Název už existuje"))
        key("BackSpace")
        capture("rename-conflict-correcting", expected="02.tx")
        key("Escape")
        assert_selected(capture("rename-conflict-cancelled", expected="01.txt"), "01.txt")
        action(4)
        fill_name("", 6)
        key("Return")
        capture("rename-invalid", expected=say("Enter a file name.", "Zadejte název souboru."))
        key("slash")
        key("Return")
        capture("rename-invalid-slash", expected=say("cannot contain /", "nesmí obsahovat /"))
        fill_name("renamed", 1)
        key("period")
        type_ascii("txt")
        key("Return")
        capture("rename-success", expected=say("Renamed", "Přejmenováno"))
        assert (sample/"renamed.txt").is_file()
        action(2, from_end=True)
        capture("new-folder", expected=say("New folder", "Nová složka"))
        key("Return")
        capture("new-folder-invalid", expected=say("Enter a folder name", "Zadejte název složky"))
        type_ascii("fresh")
        key("Return")
        capture("new-folder-success", expected=say("Created folder", "vytvořena"))
        assert (sample/"fresh").is_dir()
        action(2, from_end=True)
        type_ascii("fresh")
        key("Return")
        capture("new-folder-conflict", expected=say("Name already exists", "Název už existuje"))
        key("minus")
        key("2")
        capture("new-folder-conflict-correcting", expected=("fresh-2", say("Enter Create", "Enter Vytvořit")))
        key("Escape")
        action(5)
        capture("delete-folder", expected=say("Delete folder?", "Smazat složku?"))
        key("Escape")
        (sample/"fresh"/"content.txt").write_text("Recursive delete fixture")
        action(5)
        capture("delete-folder-with-content", expected=say("All contents", "včetně obsahu"))
        key("Return")
        capture("delete-success", expected=say("Deleted", "Smazáno"))
        assert not (sample/"fresh").exists()
        action(5)
        capture("delete-file", expected=say("Delete file?", "Smazat soubor?"))
        key("Escape")
        assert (sample/"02.txt").exists()
        capture("delete-cancelled", expected="02.txt")
        long_name = "zz-záznam pro ověření bezpečného výběru velmi dlouhého názvu s českým koncem-v03.txt"
        (sample/long_name).write_text("Long delete fixture")
        stop(app)
        app = launch("delete-long-name", sample)
        down(2)
        action(5)
        capture("delete-long-name", expected="v03.txt")
        key("Escape")
        action(4)
        frame = capture("rename-long-name", delay=.3, expected=("v03.txt", say("Enter Save", "Enter Uložit")))
        editor = next(obj for obj in frame["objects"] if obj.get("value_text") == long_name and not obj["hidden"])
        content = next(obj for obj in frame["objects"] if obj["parent"] == editor["id"])
        assert content["rect"][0] < editor["rect"][0] and content["rect"][0] + content["rect"][2] < 304, content
        key("Escape")
    finally:
        stop(app)


def exercise_usb():
    sample = base/"usb"
    sample.mkdir()
    (sample/"local.txt").write_text("Test")
    app = launch("usb", sample)
    mount = eject[0]
    second = other[0]
    try:
        attach(eject)
        folder = mount/"Příručky"
        folder.mkdir()
        (folder/"první.txt").write_text("Text")
        for i in range(1, 8):
            (folder/f"z{i}.txt").write_text("Scrollable eject fixture")
        (mount/"Prázdný").mkdir()
        (mount/"USB dokument.txt").write_text("On the drive")
        time.sleep(.85)
        capture("usb-connected", expected="UX EJECT")
        key("Return")
        capture("preview-usb", expected=say("U Open USB", "U Otevřít USB"))
        key("u")
        capture("drive-root", expected="Příručky")
        key("x")
        key("Return")
        capture("drive-folder", expected="/ Příručky")
        key("Tab")
        capture("drive-menu", expected=say("E  Eject", "E  Vysunout"))
        key("e")
        capture("eject-confirm", expected=say("Eject drive?", "Vysunout disk?"))
        key("Escape")
        capture("eject-cancelled", expected="první.txt")
        down(6)
        key("e")
        key("Escape")
        capture("eject-cancelled-scrolled", expected="z6.txt")
        key("Page_Up")
        key("Page_Up")
        key("e")
        with (folder/"první.txt").open() as busy:
            key("Return")
            capture("eject-progress", expected=say("Ejecting drive", "Vysouvám disk"))
            time.sleep(.55)
            capture("eject-busy", expected=say("Drive is busy", "Disk je zaneprázdněn"))
        key("Escape")
        (Path(os.environ["UX2_ROOT"])/"eject-fails").touch()
        key("e")
        capture("eject-busy-retry", expected="UX EJECT")
        key("Return", delay=.6)
        capture("eject-error", expected=say("Keep the drive plugged in", "Disk ponechte připojený"))
        key("Page_Down")
        capture("eject-error-details-end", expected="journal flush returned an I/O error")
        clear_fixture("eject-fails")
        key("Escape")
        key("Escape")
        key("f")
        key("Return")
        capture("drive-empty", expected=say("Empty folder", "Prázdná složka"))
        action(2, from_end=True)
        type_ascii("last")
        key("Return")
        action(6)
        key("Return")
        capture("delete-last-on-usb", expected=(say("Empty folder", "Prázdná složka"), say("Deleted", "Smazáno")))
        key("Escape")
        key("Escape")
        capture("places-one-usb", expected="UX EJECT")
        attach(other)
        (second/"other.txt").write_text("Second")
        time.sleep(.85)
        # Let any earlier connection notice expire before reading the second event.
        capture("second-usb-connected", expected=("UX OTHER", say("U Choose drive", "U Vybrat disk")))
        key("u")
        capture("places-two-usb", expected="UX OTHER")
        assert_selected(capture("second-usb-places", expected="UX OTHER"), "UX EJECT")
        key("Tab")
        capture("places-usb-menu", expected=say("E  Eject", "E  Vysunout"))
        key("Escape")
        key("x")
        key("Tab")
        capture("places-second-usb-menu", expected="UX OTHER")
        key("Escape")
        key("f")
        key("e")
        capture("places-eject-confirm", expected=say("Eject drive?", "Vysunout disk?"))
        key("Return", delay=.65)
        capture("eject-success-neighbor", expected=say("ejected", "vysunut"))
        assert not os.path.ismount(mount)
        time.sleep(5.15)
        capture("after-notice", expected="UX OTHER")
        key("Return")
        key("Return")
        attach(eject)
        time.sleep(.8)
        key("u")
        capture("two-usb-from-preview", expected="UX OTHER")
        detach(mount, lazy=True)
        time.sleep(.85)
        capture("usb-disconnected", expected=say("disconnected", "odpojen"))
        key("e")
        detach(second, lazy=True)
        time.sleep(.85)
        capture("eject-disconnected", expected=say("Drive disconnected", "Disk byl odpojen"))
        key("Escape")
        attach(eject)
        time.sleep(.87)
        key("u")
        key("e")
        key("Return", delay=.005)
        detach(mount, lazy=True)
        time.sleep(.85)
        capture("eject-disconnected-in-progress", expected=say("Eject failed", "Vysunutí selhalo"))
    finally:
        stop(app)
        for path in (mount, second):
            if os.path.ismount(path):
                detach(path, lazy=True)


def exercise_usb_interruptions():
    mount = attach(copy)
    (mount/"01-readme.txt").write_text("USB text opened before disconnection")
    deb(mount/"02-package.deb", False)
    app = launch("usb-interruptions", base)
    try:
        key("u")
        key("x")
        key("Return", delay=1.55)
        detach(mount, lazy=True)
        time.sleep(.88)
        capture("deb-source-disconnected", expected=say("Source drive disconnected", "Zdrojový disk odpojen"))
        key("Escape")
        attach(copy)
        time.sleep(.85)
        key("u")
        action(5)
        detach(mount, lazy=True)
        time.sleep(.9)
        capture("edit-drive-disconnected", expected=say("Rename interrupted", "Přejmenování přerušeno"))
        key("Escape")
        attach(copy)
        time.sleep(.85)
        key("u")
        action(6)
        detach(mount, lazy=True)
        time.sleep(.90)
        capture("delete-drive-disconnected", expected=say("Deletion did not start", "Mazání se nespustilo"))
        key("Escape")
        assert_selected(capture("delete-disconnected-return", expected=say("Places", "Umístění")), say("Home", "Domů"))
        attach(copy)
        time.sleep(.85)
        key("u")
        key("Return")
        detach(mount, lazy=True)
        time.sleep(.9)
        capture("preview-drive-disconnected", expected=say("Places", "Umístění"))
        attach(copy)
        time.sleep(.85)
        key("u")
        key("Return")
        attach(other)
        time.sleep(.85)
        detach(mount, lazy=True)
        time.sleep(.85)
        capture("preview-drive-disconnected-neighbor", expected=("UX OTHER", say("Preview closed", "Náhled zavřen")))
    finally:
        stop(app)
        if not os.path.ismount(mount):
            attach(copy)
        (mount/"01-readme.txt").unlink()
        (mount/"02-package.deb").unlink()
        detach(mount)
        if os.path.ismount(other[0]):
            detach(other[0])


def exercise_startup_with_usb():
    mount = attach(eject)
    app = launch("startup-with-usb", base)
    try:
        capture("startup-with-usb", expected=say("Places", "Umístění"))
        key("e")
        key("Return", delay=.68)
        capture("eject-success-home", expected=say("Home", "Domů"))
        assert not os.path.ismount(mount)
    finally:
        stop(app)
        if os.path.ismount(mount):
            detach(mount)


def exercise_copy():
    sample = base/"copy"
    sample.mkdir()
    dest = sample/"01-target"
    dest.mkdir()
    orig = sample/"02-source.bin"
    orig.write_bytes(bytes(8 * 1024 * 1024))
    app = launch("copy", sample, LD_PRELOAD=str(base/"slow-copy.so"), FILES_COPY_MARKER=str(marker))
    try:
        key("x")
        action(2)
        capture("copy-clipboard", expected=say("Clipboard", "Ve schránce"))
        key("f")
        key("Return")
        key("Tab")
        capture("paste-menu", expected=say("Paste", "Vložit"))
        key("Return")
        capture("copy-confirm", expected=say("Copy here?", "Zkopírovat sem?"))
        key("Escape")
        capture("copy-confirm-dismissed", expected=("01-target", say("Paste via Tab", "Vložit přes Tab")))
        action(0)
        key("Return")
        wait_for(marker.exists)
        capture("copy-progress", expected=say("Copying", "Kopíruji"))
        key("Escape")
        capture("copy-cancelling", expected=say("Cancelling copy", "Ruším kopírování"))
        time.sleep(2.85)
        capture("copy-cancelled", expected=say("Copy cancelled", "Kopírování zrušeno"))
        assert not (dest/orig.name).exists()
        action(0)
        key("Return", delay=.55)
        capture("copy-success", expected=say("Copied", "Zkopírováno"))
        assert (dest/orig.name).stat().st_size == orig.stat().st_size
        action(4)
        key("Return", delay=.4)
        capture("copy-conflict", expected=say("Name conflict", "Konflikt názvu"))
        key("Escape")
        key("Escape")
        key("x")
        action(3)
        key("f")
        key("Return")
        # Remove only the test fixture created earlier, then refresh the folder.
        (dest/orig.name).unlink()
        key("Escape")
        key("Return")
        action(0)
        key("Return", delay=.55)
        assert not orig.exists() and (dest/orig.name).exists()
        action(2)
        action(4)
        key("Return", delay=.5)
        capture("copy-in-place", expected="02-source copy.bin")
        assert (dest/"02-source copy.bin").is_file()
        action(3)
        action(4)
        key("Return", delay=.5)
        capture("move-already-here", expected=say("The item is already", "Položka už je v této složce"))
    finally:
        stop(app)
        marker.unlink(missing_ok=True)

    app = launch("copy-partial-progress", sample, LD_PRELOAD=str(base/"slow-copy.so"),
                 FILES_COPY_MARKER=str(marker), FILES_COPY_SLOW_AFTER=str(3 * 1024 * 1024))
    try:
        key("Return")
        down(1)
        action(2)
        action(4)
        key("Return")
        wait_for(marker.exists)
        capture("copy-progress-bytes", delay=.20, expected=say("3 MiB of 8 MiB", "3 MiB z 8 MiB"))
        key("Escape")
        time.sleep(3.05)
        capture("copy-cancelled-after-bytes", expected=say("Copy cancelled", "Kopírování zrušeno"))
        assert (dest/orig.name).stat().st_size == 8 * 1024 * 1024
        assert not list(dest.glob(".files-transfer-*"))
        assert not (dest/"02-source copy copy.bin").exists()
    finally:
        stop(app)
        marker.unlink(missing_ok=True)


def exercise_copy_folder():
    sample = base/"copy-folder"
    sample.mkdir()
    (sample/"01-target").mkdir()
    source = sample/"02-source-folder"
    source.mkdir()
    (source/"data.bin").write_bytes(bytes(8 * 1024 * 1024))
    app = launch("copy-folder", sample, LD_PRELOAD=str(base/"slow-copy.so"), FILES_COPY_MARKER=str(marker))
    try:
        key("x")
        action(2)
        key("f")
        key("Return")
        action(0)
        key("Return")
        wait_for(marker.exists)
        capture("copy-folder-progress", expected=("02-source-folder", say("Transferred", "Přeneseno")))
        key("Escape")
        time.sleep(3.05)
        assert not (sample/"01-target"/source.name).exists()
    finally:
        stop(app)
        marker.unlink(missing_ok=True)


def exercise_copy_disconnect(from_usb, move=False):
    sample = base/("move-to-usb" if move else ("from-usb" if from_usb else "to-usb"))
    sample.mkdir()
    mount = copy[0]
    app = launch("copy-disconnect", sample, LD_PRELOAD=str(base/"slow-copy.so"), FILES_COPY_MARKER=str(marker))
    try:
        if from_usb:
            attach(copy)
            orig = mount/"source.bin"
            orig.write_bytes(bytes(8 * 1024 * 1024))
            time.sleep(.85)
            key("u")
            action(3)
            key("Escape")
            key("x")
            key("Return")
            # Home contains the deterministic local destination in this namespace.
            action(0)
        else:
            orig = sample/"02-source.bin"
            orig.write_bytes(bytes(8 * 1024 * 1024))
            # Re-enter the test directory so its file is picked up by the list.
            key("Escape")
            # Restart after writing the source so keyboard selection is deterministic.
            stop(app)
            app = launch("copy-disconnect", sample, LD_PRELOAD=str(base/"slow-copy.so"), FILES_COPY_MARKER=str(marker))
            action(3 if move else 2)
            if move:
                capture("move-clipboard", expected=say("Tab › Paste", "Tab › Vložit"))
            attach(copy)
            time.sleep(.85)
            key("u")
            action(1)
        if move:
            capture("move-confirm", expected=say("Move here?", "Přesunout sem?"))
            key("Escape")
            capture("move-confirm-dismissed", expected=say("Paste via Tab", "Vložit přes Tab"))
            key("Tab")
            capture("move-clipboard-paste-menu", expected=say("Paste", "Vložit"))
            key("x")
            key("Return")
        key("Return")
        wait_for(marker.exists)
        if move:
            capture("move-progress", expected="02-source.bin")
            key("Escape")
            capture("move-cancelling", expected=say("Cancelling move", "Ruším přesun"))
            time.sleep(2.75)
            capture("move-cancelled", expected=say("Move cancelled", "Přesun zrušen"))
            assert orig.exists() and not (mount/orig.name).exists()
            action(1)
            key("Return")
            capture("move-success", expected=say("Moved 02-source.bin", "Přesunuto 02-source.bin"))
            assert not orig.exists() and (mount/orig.name).stat().st_size == 8 * 1024 * 1024
            (mount/orig.name).unlink()
            return
        detach(mount, lazy=True)
        time.sleep(3.1)
        name = "copy-source-disconnected" if from_usb else "copy-target-disconnected"
        capture(name, expected=say("Copy interrupted", "Kopírování přerušeno"))
        attach(copy)
        if not from_usb:
            key("Escape")
            time.sleep(.85)
            key("u")
            capture("copy-target-reconnected", expected=say("Empty folder", "Prázdná složka"))
        if from_usb:
            assert orig.stat().st_size == 8 * 1024 * 1024
            orig.unlink()
        else:
            assert orig.exists() and not (mount/orig.name).exists()
        assert not list(mount.glob(".files-transfer-*"))
    finally:
        stop(app)
        marker.unlink(missing_ok=True)
        if os.path.ismount(mount):
            detach(mount, lazy=True)


def exercise_retained_copy():
    sample = base/"retained-copy"
    sample.mkdir()
    original = sample/"source.bin"
    original.write_bytes(bytes(8 * 1024 * 1024))
    mount = copy[0]
    app = launch("retained-copy", sample, LD_PRELOAD=str(base/"slow-copy.so"),
                 FILES_COPY_MARKER=str(marker), FILES_COPY_SLOW_AFTER=str(3 * 1024 * 1024),
                 FILES_AUDIT_KEEP_STAGE="1")
    try:
        action(2)
        attach(copy)
        time.sleep(.9)
        key("u")
        action(1)
        key("Return")
        wait_for(marker.exists)
        detach(mount, lazy=True)
        time.sleep(3.1)
        capture("copy-target-partial-retained", expected=say("A partial copy may remain", "může být neúplná kopie"))
        attach(copy)
        stage, = mount.glob(".files-transfer-*")
        payload = stage/"payload"
        assert 0 < payload.stat().st_size < original.stat().st_size
        key("Escape")
        time.sleep(.85)
        key("u")
        capture("copy-target-partial-reconnected", expected=".files-transfer-")
        shutil.rmtree(stage)
    finally:
        stop(app)
        marker.unlink(missing_ok=True)
        if os.path.ismount(mount):
            detach(mount)


def exercise_move_partial():
    sample = base/"move-partial"
    sample.mkdir()
    orig = sample/"01-source.bin"
    orig.write_bytes(bytes(8 * 1024 * 1024))
    mount = copy[0]
    app = launch("move-partial", sample, LD_PRELOAD=str(base/"slow-copy.so"),
                 FILES_COPY_MARKER=str(marker), FILES_COPY_SLOW_AFTER=str(3 * 1024 * 1024))
    try:
        action(3)
        attach(copy)
        time.sleep(.9)
        key("u")
        action(1)
        key("Return")
        wait_for(marker.exists)
        capture("move-progress-bytes", expected=say("3 MiB of 8 MiB", "3 MiB z 8 MiB"))
        key("Escape")
        capture("move-cancelling-after-bytes", expected=say("Cancelling move", "Ruším přesun"))
        time.sleep(3.05)
        capture("move-cancelled-after-bytes-target", expected=say("source kept", "zdroj zachován"))
        assert orig.stat().st_size == 8 * 1024 * 1024 and not (mount/orig.name).exists()
        assert not list(mount.glob(".files-transfer-*"))
    finally:
        stop(app)
        marker.unlink(missing_ok=True)
        if os.path.ismount(mount):
            detach(mount)
    app = launch("move-partial-source", sample)
    try:
        assert_selected(capture("move-cancelled-after-bytes-source", expected="01-source.bin"), "01-source.bin")
    finally:
        stop(app)


def exercise_eject_long():
    fixtures = [disk(name) for name in ("01 FIRST", "02 SECOND", "03 THIRD", "04 FOURTH",
                  "05 USB with a very long distinguishing name v03", "06 NEIGHBOR", "07 LAST")]
    for fixture in fixtures:
        attach(fixture)
    app = launch("eject-long", base)
    try:
        down(4)
        key("e")
        capture("eject-long-confirm", expected=("v03", say("Eject drive?", "Vysunout disk?")))
        with (fixtures[4][0]/"busy").open("w") as busy:
            key("Return")
            capture("wait-eject-long-busy", expected=say("Drive is busy", "Disk je zaneprázdněn"), keep=False)
        key("Escape")
        key("e")
        key("Return")
        frames = capture("eject-long-retry-success", expected=("06 NEIGHBOR", "v03", say("ejected", "vysunut")))
        assert_selected(frames, "06 NEIGHBOR")
        assert not os.path.ismount(fixtures[4][0])
    finally:
        stop(app)
        for mount, _ in fixtures:
            if os.path.ismount(mount):
                detach(mount)



def exercise_additional_storage_states():
    sample = base/"additional-storage"
    sample.mkdir()
    gone = sample/"gone"
    gone.mkdir()
    app = launch("additional-storage", sample)
    try:
        gone.rmdir()
        gone.write_text("A file replaced the folder after the list was read.")
        key("Return")
        capture("folder-disappeared", expected=say("Folder not found", "Složka nenalezena"))
        attach(copy)
        time.sleep(.9)
        key("u")
        action(2, from_end=True)
        type_ascii("draft")
        detach(copy[0], lazy=True)
        time.sleep(.9)
        capture("new-folder-drive-disconnected", expected=say("Folder creation interrupted", "Vytváření složky přerušeno"))
    finally:
        stop(app)
        if os.path.ismount(copy[0]):
            detach(copy[0])


def exercise_transfer_unplug_variants():
    cases = (
        ("move-source-disconnected", True, False, True),
        ("move-target-disconnected", False, True, True),
        ("copy-both-disconnected", True, True, False),
    )
    for name, source_usb, target_usb, move in cases:
        sample = base/name
        sample.mkdir()
        source = attach(copy) if source_usb else sample
        origin = source/"probe.bin"
        origin.write_bytes(bytes(8 * 1024 * 1024))
        app = launch(name, sample, LD_PRELOAD=str(base/"slow-copy.so"), FILES_COPY_MARKER=str(marker))
        try:
            if source_usb:
                key("Return")
            action((3 if move else 2) + source_usb)
            if target_usb:
                target = attach(other)/"00-unplug-target"
                target.mkdir(exist_ok=True)
                time.sleep(.9)
                key("u")
                if source_usb:
                    key("x")
                    key("Return")
                key("Return")
            else:
                key("Escape")
                key("x")
                key("Return")
            action(int(target_usb))
            key("Return")
            wait_for(marker.exists)
            for path, active in ((copy[0], source_usb), (other[0], target_usb)):
                if active:
                    detach(path, lazy=True)
            expected = say("Move interrupted", "Přesun přerušen") if move else say(
                "Source and target drives disconnected", "Zdrojový i cílový disk odpojen")
            capture(name, delay=3.1, expected=expected)
        finally:
            stop(app)
            marker.unlink(missing_ok=True)
            if source_usb and not os.path.ismount(source):
                attach(copy)
            origin.unlink()
            for path, _ in (copy, other):
                if os.path.ismount(path):
                    detach(path)


def exercise_package():
    sample = base/"package"
    sample.mkdir()
    deb(sample/"01-balíček.deb", True)
    deb(sample/"02-plain.deb", False)
    deb(sample/"03-wrong-architecture.deb", False, "s390x")
    deb(sample/"06-dependencies.deb", False, depends="bash (>= 5.0), debian-archive-keyring (>= 2023.3)")
    (sample/"04-corrupt.deb").write_text("Not a Debian archive")
    damaged = sample/"05-damaged.deb"
    shutil.copyfile(sample/"02-plain.deb", damaged)
    archive = base/"ar-damage"
    archive.mkdir()
    member = next(name for name in run("ar", "t", damaged).splitlines() if name.startswith("data.tar"))
    payload = archive/member
    payload.write_bytes(b"Corrupt compressed payload")
    run("ar", "r", damaged, payload)
    assert subprocess.run(["dpkg-deb", "--info", damaged], capture_output=True).returncode == 0
    assert subprocess.run(["dpkg-deb", "--contents", damaged], capture_output=True).returncode != 0
    def wait_for_install_review():
        capture("wait-package-review", delay=.02, expected=say("Enter Install", "Enter Instalovat"), keep=False)

    app = launch("package", sample)
    try:
        key("Return")
        capture("deb-checking", expected=say("Checking package", "Ověřuji balíček"))
        time.sleep(1.35)
        capture("deb-root-scripts", expected=say("Install scripts will run as root", "Při instalaci poběží skripty jako root"))
        key("Page_Down")
        capture("deb-scrolled", expected="ux2-files-test")
        (Path(os.environ["UX2_ROOT"])/"require-password").touch()
        key("Return")
        capture("install-password", expected=say("Password to install", "Heslo pro instalaci"))
        type_ascii("fixture")
        capture("install-password-masked", expected="*******")
        key("Escape")
        capture("install-password-cancelled", expected="01-balíček.deb")
        key("Return")
        wait_for_install_review()
        key("Return")
        capture("install-password-reopened", expected=say("Enter your account password", "Zadejte heslo svého účtu"))
        type_ascii("fixture")
        (Path(os.environ["UX2_ROOT"])/"reject-password").touch()
        key("Return", delay=.53)
        capture("install-password-retry", expected=say("Incorrect password", "Nesprávné heslo"))
        key("a")
        capture("install-password-retry-one", expected="*")
        key("Return", delay=.5)
        capture("wait-password-retry", expected=say("Incorrect password", "Nesprávné heslo"), keep=False)
        type_ascii("fixture")
        capture("install-password-last-attempt", expected=say("Attempt 3 of 3", "Pokus 3 ze 3"))
        key("Return")
        capture("install-password-failed", expected=say("verify your account", "Účet se nepodařilo ověřit"))
        clear_fixture("reject-password")
        key("Escape")
        clear_fixture("require-password")
        key("Return")
        wait_for_install_review()
        (Path(os.environ["UX2_ROOT"])/"install-fails").touch()
        key("Return")
        capture("install-progress", expected=say("Installing package", "Instaluji balíček"))
        time.sleep(.6)
        capture("install-failure", expected=(say("Installation failed", "Instalace selhala"),
                                             say("isn’t enough disk space", "Na disku není dost místa")))
        key("Page_Down")
        key("Page_Down")
        capture("install-failure-details-end", expected=say("Press F to return", "Klávesou F se vraťte"))
        clear_fixture("install-fails")
        key("Escape")
        key("Return")
        wait_for_install_review()
        key("Return")
        time.sleep(2.5)
        capture("install-success-no-store", expected=say("Installation complete", "Instalace dokončena"))
        assert "1.0.0" in run("dpkg-query", "-W", "ux2-files-test")
        key("Escape")
        key("x")
        (Path(os.environ["UX2_ROOT"])/"apt-fails").touch()
        key("Return", delay=1.6)
        capture("deb-apt-unavailable", expected=say("Installation blocked", "Instalace zablokována"))
        down(2)
        capture("deb-apt-unavailable-details", expected=(say("Installation blocked", "Instalace zablokována"),
                                                       "dependency resolution failed"))
        clear_fixture("apt-fails")
        key("Escape")
        key("Return", delay=1.6)
        capture("deb-no-scripts", expected="1.0.0 · all")
        key("Escape")
        stage_links = set(Path("/var/tmp").glob("cardputer-files-deb-*/package.deb"))
        key("Return")
        wait_for_install_review()
        staged, = set(Path("/var/tmp").glob("cardputer-files-deb-*/package.deb")) - stage_links
        staged.unlink()
        key("Return")
        capture("install-prepared-unavailable", expected=say("prepared package changed", "Balíček se po kontrole změnil"))
        key("Escape")
        key("Return")
        wait_for_install_review()
        helper = Path("/usr/share/APPLaunch/bin/M5CardputerZero-Files")
        disabled = helper.with_suffix(".disabled")
        helper.rename(disabled)
        try:
            key("Return", delay=.60)
            capture("install-files-helper-missing", expected=say("installer helper is missing", "Chybí instalační nástroj Files"))
        finally:
            disabled.rename(helper)
        key("Escape")
        key("Return", delay=1.55)
        capture("deb-review-reopened", expected="1.0.0 · all")
        helper.chmod(0o775)
        try:
            key("Return")
            capture("install-helper-unsafe", expected=say("unsafe permissions", "nezabezpečená oprávnění"))
        finally:
            helper.chmod(0o755)
        key("Escape")
        key("x")
        key("Return", delay=.65)
        capture("deb-wrong-architecture", expected=say("Unsupported architecture", "Nepodporovaná architektura"))
        key("Page_Down")
        capture("deb-wrong-architecture-scrolled", expected=say("Unsupported architecture", "Nepodporovaná architektura"))
        key("Escape")
        key("x")
        key("Return", delay=.45)
        capture("deb-invalid-package", expected=say("Invalid or unreadable", "Neplatný nebo nečitelný"))
        key("Escape")
        key("x")
        key("Return", delay=.9)
        capture("deb-damaged-package", expected=say("Damaged Debian package", "Poškozený balíček Debian"))
        for _ in range(8):
            key("Page_Down", delay=.02, held=.04)
        capture("deb-error-details-end", expected=("returned error exit status 2",
                                                   say("Damaged Debian package", "Poškozený balíček Debian")))
        key("Escape")
        key("x")
        key("Return")
        wait_for_install_review()
        capture("deb-with-dependencies", expected="debian-archive-")
        key("Escape")
        for _ in range(4):
            key("f")
        store = helper.with_name("M5CardputerZero-AppStore")
        store.write_text("#!/bin/sh\nexit 2\n")
        store.chmod(0o755)
        try:
            key("Return", delay=1.4)
            capture("deb-store-available", expected=say("APT plan verified · Store", "Plán APT ověřen · Store"))
        finally:
            store.unlink()
        key("Escape")
    finally:
        stop(app)
        clear_fixture("require-password")
        clear_fixture("install-fails")


def exercise_previews():
    sample = base/"previews"
    sample.mkdir()
    (sample/"01-text.txt").write_text("Příliš žluťoučký kůň\n" * 29 + "Poslední řádek: šťastný konec.", encoding="utf-8")
    artwork = Image.new("RGB", (144, 90), (25, 45, 90))
    artwork.paste((229, 181, 28), (12, 11, 57, 70))
    artwork.save(sample/"02-image.png")
    (sample/"03-broken.png").write_text("Broken")
    with wave.open(str(sample/"04-audio.wav"), "wb") as sound:
        sound.setparams((1, 2, 8000, 0, "NONE", "not compressed"))
        sound.writeframes(b"".join(struct.pack("<h", round(12000 * math.sin(i / 12))) for i in range(64000)))
    run("ffmpeg", "-hide_banner", "-loglevel", "error", "-threads", "1",
        "-f", "lavfi", "-i", "testsrc2=size=144x90:rate=15", "-t", "8",
        "-c:v", "mpeg4", "-threads", "1", sample/"05-video.mp4")
    (sample/"06-broken.mp4").write_text("Broken")
    (sample/"07-broken.wav").write_text("Broken")
    (sample/"08-empty.txt").touch()
    (sample/"09-wide.txt").write_text("/" + "široká-cesta/" * 23 + "poslední-znak-Ž")
    long_video = "10-Dlouhý název videa s odlišitelným koncem v03.mp4"
    run("ffmpeg", "-hide_banner", "-loglevel", "error", "-threads", "1",
        "-f", "lavfi", "-i", "smptebars=size=144x90:rate=15", "-t", "22",
        "-c:v", "mpeg4", "-threads", "1", sample/long_video)
    media_tools = base/"media-tools"
    media_tools.mkdir()
    slow_video = media_tools/"ffmpeg"
    ffmpeg = shutil.which("ffmpeg")
    slow_video.write_text("#!/bin/sh\nsleep 0.65\nexec " + shlex.quote(ffmpeg) + ' "$@"\n')
    slow_video.chmod(0o755)
    app = launch("previews", sample, PATH=str(media_tools) + ":" + os.environ["PATH"])
    try:
        key("Return")
        key("Page_Down")
        capture("text-scrolled", expected="Příliš")
        for _ in range(5):
            key("Page_Down")
        capture("text-end", expected="Poslední řádek")
        key("Escape")
        key("x")
        key("Return")
        capture("image-preview", expected="02-image")
        key("7")
        capture("image-zoom", expected="02-image")
        key("8")
        capture("image-rotated", expected=say("Rotate", "Otočit"))
        key("4")
        capture("image-fullscreen")
        key("Escape")
        key("x")
        key("Return")
        capture("image-error", expected=say("Preview unavailable", "Náhled není dostupný"))
        key("Escape")
        key("x")
        key("Return")
        capture("audio-ready", expected="04-audio")
        key("6")
        capture("audio-playing", expected=say("Pause", "Pauza"))
        key("8")
        capture("audio-speed", expected="2×")
        key("8")
        capture("audio-speed-5", expected="5×")
        key("6")
        capture("audio-paused", expected=say("Play", "Hrát"))
        key("6")
        time.sleep(2.0)
        capture("audio-ended", expected="0:08")
        key("Escape")
        for _ in range(3):
            key("space", delay=.07)
        capture("magic", delay=.27)
        key("Escape")
        capture("magic-dismissed", expected="04-audio")
        key("x")
        key("Return")
        capture("video-opening", expected=say("Opening video", "Otevírám video"))
        time.sleep(.86)
        capture("video-playing", expected=say("Space Pause", "Mezera Pauza"))
        key("space")
        capture("video-paused", expected=say("Space Play", "Mezera Hrát"))
        key("space")
        time.sleep(9)
        capture("video-ended", expected=("05-video", say("Files", "Soubory")))
        key("x")
        key("Return", delay=1.0)
        capture("video-error", expected=say("Cannot play video", "Video nelze přehrát"))
        key("Escape")
        key("x")
        key("Return")
        capture("audio-error", expected=say("Cannot play audio", "Zvuk nelze přehrát"))
        key("Escape")
        key("x")
        key("Return")
        capture("text-empty", expected=say("Empty file", "Prázdný soubor"))
        key("Escape")
        key("x")
        key("Return")
        capture("text-wide", expected="/široká-cesta/")
        key("Page_Down")
        end = capture("text-wide-end", expected=("poslední-znak-Ž", "F↑"))
        assert not any("X↓" in (obj["text"] or "") for obj in end["objects"] if not obj["hidden"])
        key("Escape")
        key("x")
        key("Return")
        capture("wait-video-frame", expected=say("Space Pause", "Mezera Pauza"), keep=False)
        key("space")
        pause = say("Ⅱ Paused", "Ⅱ Pauza")
        before = capture("video-paused-long-name", expected=("v03.mp4", pause))
        key("c")
        after = capture("video-paused-after-seek", expected=(pause, say("Space Play", "Mezera Hrát")))

        def pause_seconds(frame):
            title = next(obj["text"] for obj in frame["objects"] if not obj["hidden"] and pause in (obj["text"] or ""))
            minutes, seconds = map(int, re.search(r"(\d+):(\d+)$", title).groups())
            return 60 * minutes + seconds

        assert pause_seconds(after) - pause_seconds(before) == 10
        key("Escape")
    finally:
        stop(app)

    app = launch("audio-with-usb", sample)
    try:
        down(3)
        key("Return")
        key("6")
        attach(other)
        time.sleep(.90)
        capture("audio-usb", expected=("UX OTHER", "U USB", say("Esc Back", "Esc Zpět")))
        key("Escape")
    finally:
        stop(app)
        if os.path.ismount(other[0]):
            detach(other[0])


def exercise_filesystem_errors():
    source = base/"errors"
    source.mkdir()
    (source/"protected").mkdir(mode=0o700)
    unreadable = base/"unreadable"
    unreadable.mkdir()
    unreadable_text = unreadable/"protected.txt"
    unreadable_text.write_text("Read error fixture")
    (source/"source.bin").write_bytes(bytes(8 * 1024 * 1024))
    target = source/"target"
    target.mkdir()
    (config/"cardputer-files/language").chmod(0o644)
    permission = launch("permission", source, user="nobody")
    try:
        key("Return")
        capture("permission-denied", expected=say("Permission denied", "Přístup odepřen"))
        key("Escape")
        action(1, from_end=True)
        key("x")
        key("Return")
        capture("language-save-failed", expected=say("Could not save language", "Jazyk se nepodařilo uložit"))
        assert (config/"cardputer-files/language").read_text().strip() == language
    finally:
        stop(permission)
    permission = launch("text-permission", unreadable, LD_PRELOAD=str(base/"read-fault.so"),
                        FILES_AUDIT_READ_FAULT=str(unreadable_text))
    try:
        key("Return")
        capture("text-media-error", expected=say("Read failed", "Čtení se nezdařilo"))
    finally:
        stop(permission)
    permission = launch("text-denied", unreadable, LD_PRELOAD=str(base/"read-fault.so"),
                        FILES_AUDIT_READ_FAULT=str(unreadable_text), FILES_AUDIT_DENY_READ="1")
    try:
        key("Return")
        capture("text-read-error", expected=say("Permission to read this file was denied", "Chybí oprávnění ke čtení"))
    finally:
        stop(permission)
    app = launch("filesystem-errors", source)
    try:
        down(2)
        action(2)
        key("f")
        run("mount", "-t", "tmpfs", "-o", "size=1M", "tmpfs", target)
        key("Return")
        action(0)
        key("Return", delay=.5)
        capture("disk-full", expected=say("Not enough space", "Nedostatek místa"))
        key("Escape")
        (target/"existing.txt").write_text("Read-only fixture")
        key("Escape")
        key("Return")
        run("mount", "-o", "remount,ro", target)
        action(4)
        key("Return", delay=.5)
        capture("read-only", expected=say("Disk is read-only", "Disk je jen pro čtení"))
        key("Escape")
        action(5)
        key("a")
        key("Return")
        capture("rename-read-only", expected=("existing.txta", say("Disk is read-only", "Disk je jen pro čtení")))
        key("Escape")
        action(6)
        key("Return")
        capture("delete-read-only", expected=(say("Delete failed", "Mazání selhalo"), say("Disk is read-only", "Disk je jen pro čtení")))
        key("Escape")
        action(2, from_end=True)
        type_ascii("new")
        key("Return")
        capture("new-folder-read-only", expected=(say("New folder", "Nová složka"), say("Disk is read-only", "Disk je jen pro čtení")))
        key("Escape")
        key("Escape")
        action(6)
        key("Return")
        capture("delete-mounted-filesystem", expected=say("Folder contains a mounted filesystem", "Složka obsahuje připojený souborový systém"))
        key("Escape")
    finally:
        stop(app)
        if os.path.ismount(target):
            detach(target)


failures = []
for language in os.environ.get("FILES_AUDIT_LANGUAGES", "cs,en").split(","):
    language_base = base / language
    language_base.mkdir()
    config = language_base/"config"
    (config/"cardputer-files").mkdir(parents=True)
    (config/"cardputer-files/language").write_text(language + "\n")
    os.chmod(language_base, 0o755)
    base_original, base = base, language_base
    # Namespace helpers resolve their control flags from the outer sandbox path.
    (base/"slow-copy.so").symlink_to(base_original/"slow-copy.so")
    (base/"read-fault.so").symlink_to(base_original/"read-fault.so")
    (base/"loop-devices").symlink_to(base_original/"loop-devices")
    marker = base/"copy-started"
    eject, other, copy = [disk(name) for name in ("UX EJECT", "UX OTHER", "UX COPY")]
    scenarios = {
        "browser": exercise_browser, "edit": exercise_name_and_delete, "usb": exercise_usb,
        "startup-usb": exercise_startup_with_usb, "usb-interruptions": exercise_usb_interruptions, "copy": exercise_copy, "copy-source": lambda: exercise_copy_disconnect(True),
        "copy-target": lambda: exercise_copy_disconnect(False),
        "move": lambda: exercise_copy_disconnect(False, move=True), "package": exercise_package,
        "previews": exercise_previews, "errors": exercise_filesystem_errors,
        "folder-copy": exercise_copy_folder, "move-partial": exercise_move_partial,
        "eject-long": exercise_eject_long, "retained-copy": exercise_retained_copy,
        "additional-storage": exercise_additional_storage_states,
        "transfer-unplug-variants": exercise_transfer_unplug_variants,
    }
    selected = os.environ.get("FILES_AUDIT_SCENARIOS", ",".join(scenarios)).split(",")
    for scenario in selected:
        try:
            scenarios[scenario]()
        except Exception:
            traceback.print_exc()
            failures.append(f"{scenario}-{language}")
    for path, _ in (eject, other, copy):
        path.rmdir()
    for flag in ("eject-fails", "install-fails", "require-password", "reject-password", "apt-fails"):
        (base_original/flag).unlink(missing_ok=True)
    base = base_original

x11.XCloseDisplay(display)
if failures:
    raise SystemExit("Failed scenarios: " + ", ".join(failures))
print("SDL layout audit scenarios passed", flush=True)