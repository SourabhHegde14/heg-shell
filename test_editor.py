#!/usr/bin/env python3
"""Line-editor test for heg: history, cursor keys, Ctrl shortcuts, tab completion.

Drives heg through a pseudo-terminal with TERM=xterm (the editor is only
active on a capable terminal) and presses real keys.

usage: python3 test_editor.py [path-to-heg]
"""
import os
import pty
import re
import select
import shutil
import signal
import sys
import tempfile
import time

HEG = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else "./heg")
PROMPT = r"\x1b\[0m\$ "
UP, DOWN, RIGHT, LEFT = "\x1b[A", "\x1b[B", "\x1b[C", "\x1b[D"
CTRL_A, CTRL_C, CTRL_D, CTRL_U, CTRL_W = "\x01", "\x03", "\x04", "\x15", "\x17"


class Session:
    def __init__(self, cwd):
        self.pid, self.fd = pty.fork()
        if self.pid == 0:
            os.chdir(cwd)
            os.environ["TERM"] = "xterm"
            os.environ["HOME"] = cwd
            os.environ["PATH"] = "/usr/bin:/bin"
            os.execv(HEG, [HEG])
        self.buf = b""

    def send(self, text, delay=0.08):
        os.write(self.fd, text.encode())
        time.sleep(delay)

    def expect(self, pattern, timeout=5.0):
        rx = re.compile(pattern.encode())
        end = time.time() + timeout
        while time.time() < end:
            m = rx.search(self.buf)
            if m:
                self.buf = self.buf[m.end():]
                return
            ready, _, _ = select.select([self.fd], [], [], 0.1)
            if ready:
                try:
                    data = os.read(self.fd, 4096)
                except OSError:
                    break
                if not data:
                    break
                self.buf += data
        raise AssertionError("timed out waiting for %r; buffer: %r" % (pattern, self.buf))

    def line(self, text):
        """type a command and press Enter, wait for the next prompt"""
        self.send(text + "\r")
        self.expect(PROMPT)

    def close(self):
        try:
            os.kill(self.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        try:
            os.waitpid(self.pid, 0)
        except ChildProcessError:
            pass


passed = 0


def step(name):
    global passed
    passed += 1
    print("  ok    " + name)


def main():
    tmp = tempfile.mkdtemp()
    for name, content in [("alpha_file.txt", "A1"), ("alpha_other.txt", "A2"), ("beta.txt", "BETA-CONTENT")]:
        with open(os.path.join(tmp, name), "w") as f:
            f.write(content + "\n")
    os.mkdir(os.path.join(tmp, "subdir"))
    s = Session(tmp)
    try:
        s.expect(PROMPT)
        step("editor starts on a capable terminal")

        s.line("echo first-cmd")
        s.expect(r"first-cmd")
        s.line("echo second-cmd")
        s.send(UP + UP + "\r")
        s.expect(r"\r\nfirst-cmd\r\n")
        s.expect(PROMPT)
        step("Up arrow recalls older history entries")

        s.send("echo second-cmd")
        s.send(UP + DOWN)
        s.send("\r")
        s.expect(r"\r\nsecond-cmd\r\n")
        s.expect(PROMPT)
        step("Down arrow returns to the line being typed")

        s.send("echo ac" + LEFT + "b\r")
        s.expect(r"\r\nabc\r\n")
        s.expect(PROMPT)
        step("Left arrow + insert in the middle of a line")

        s.send("cho Z" + CTRL_A + "e\r")
        s.expect(r"\r\nZ\r\n")
        s.expect(PROMPT)
        step("Ctrl-A jumps to start of line")

        s.send("garbage" + CTRL_U + "echo cleared\r")
        s.expect(r"\r\ncleared\r\n")
        s.expect(PROMPT)
        step("Ctrl-U deletes to start of line")

        s.send("echo one two" + CTRL_W + "\r")
        s.expect(r"\r\none\r\n")
        s.expect(PROMPT)
        step("Ctrl-W deletes the previous word")

        s.send("echo abc" + LEFT + LEFT + "\x1b[3~\r")  # Delete key removes 'b'
        s.expect(r"\r\nac\r\n")
        s.expect(PROMPT)
        step("Delete key removes the character under the cursor")

        s.send("cat beta.t\t")
        s.send("\r")
        s.expect(r"BETA-CONTENT")
        s.expect(PROMPT)
        step("Tab completes a unique file name")

        s.send("cat alpha_\t")
        s.expect(r"alpha_file\.txt")
        s.expect(r"alpha_other\.txt")
        s.send(CTRL_C)
        s.expect(PROMPT)
        step("Tab lists candidates when the prefix is ambiguous")

        s.send("cat alpha_f\t\r")
        s.expect(r"\r\nA1\r\n")
        s.expect(PROMPT)
        step("Tab finishes the name once it is unambiguous")

        s.send("ech\t")
        s.send("hi\r")
        s.expect(r"\r\nhi\r\n")
        s.expect(PROMPT)
        step("Tab completes a command name (builtin/PATH)")

        s.send("cd sub\t\r")
        s.expect(PROMPT)
        s.line("pwd")
        s.send("")
        step("Tab completes a directory and appends /")
        s.send("echo in:$PWD\r")
        s.expect(r"in:[^\r]*subdir\r\n")
        s.expect(PROMPT)
        step("cd into the completed directory worked")

        s.send("abc" + CTRL_C)
        s.expect(r"\^C")
        s.expect(PROMPT)
        step("Ctrl-C abandons the current line")

        s.send("if true; then\r")
        s.expect(r"> ")
        s.send("echo multi\r")
        s.expect(r"> ")
        s.send("fi\r")
        s.expect(r"\r\nmulti\r\n")
        s.expect(PROMPT)
        step("multi-line input shows a continuation prompt")

        s.send("if true; then\r")
        s.expect(r"> ")
        s.send(CTRL_C)
        s.expect(PROMPT)
        step("Ctrl-C at the continuation prompt cancels the command")

        s.send(CTRL_D)
        deadline = time.time() + 3
        while time.time() < deadline:
            pid, _ = os.waitpid(s.pid, os.WNOHANG)
            if pid:
                break
            time.sleep(0.05)
        else:
            raise AssertionError("shell did not exit on Ctrl-D")
        step("Ctrl-D on an empty line exits")
    except AssertionError as e:
        print("  FAIL  " + str(e))
        s.close()
        shutil.rmtree(tmp, ignore_errors=True)
        sys.exit(1)
    s.close()
    shutil.rmtree(tmp, ignore_errors=True)
    print("\npassed: %d   failed: 0" % passed)


if __name__ == "__main__":
    main()
