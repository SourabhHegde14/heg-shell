#!/usr/bin/env python3
"""Interactive job-control test for heg.

Job control (Ctrl-C, Ctrl-Z, fg, bg) only exists when the shell has a real
terminal, so this drives heg through a pseudo-terminal like a human would.

usage: python3 test_jobs.py [path-to-heg]
"""
import os
import pty
import re
import select
import signal
import sys
import time

HEG = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else "./heg")
PROMPT = r"\x1b\[0m\$ "  # the reset code right before "$ " only occurs in the prompt


class Session:
    def __init__(self):
        self.pid, self.fd = pty.fork()
        if self.pid == 0:
            os.environ["TERM"] = "dumb"
            os.environ["HOME"] = "/tmp"
            os.execv(HEG, [HEG])
        self.buf = b""

    def send(self, text, delay=0.15):
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
    s = Session()
    try:
        s.expect(PROMPT)
        step("shell starts and shows a prompt")

        s.send("sleep 30 &\n")
        s.expect(r"\[1\] \d+")
        s.expect(PROMPT)
        step("background job is announced: [1] <pid>")

        s.send("jobs\n")
        s.expect(r"\[1\]\+\s+Running\s+sleep 30 &")
        s.expect(PROMPT)
        step("jobs lists it as Running")

        s.send("fg\n")
        s.expect(r"sleep 30")
        s.send("\x1a")  # Ctrl-Z
        s.expect(r"Stopped\s+sleep 30")
        s.expect(PROMPT)
        step("fg + Ctrl-Z stops the job and returns to the prompt")

        s.send("jobs\n")
        s.expect(r"\[1\]\+\s+Stopped\s+sleep 30")
        s.expect(PROMPT)
        step("jobs lists it as Stopped")

        s.send("bg\n")
        s.expect(r"\[1\]\+ sleep 30 &")
        s.expect(PROMPT)
        step("bg resumes it in the background")

        s.send("kill %1\n")
        s.expect(PROMPT)
        time.sleep(0.3)
        s.send("\n")
        s.expect(r"Terminated\s+sleep 30")
        s.expect(PROMPT)
        step("kill %1 terminates it and the shell reports it")

        s.send("sleep 30\n", delay=0.4)
        s.send("\x03")  # Ctrl-C
        s.expect(PROMPT)
        s.send("echo status=$?\n")
        s.expect(r"status=130")
        s.expect(PROMPT)
        step("Ctrl-C kills the foreground job (status 130), not the shell")

        s.send("echo alive\n")
        s.expect(r"alive")
        s.expect(PROMPT)
        step("shell survives Ctrl-C")

        s.send("echo hello | tr a-z A-Z\n")
        s.expect(r"HELLO")
        s.expect(PROMPT)
        step("pipelines work interactively")

        s.send("cd /tmp\n")
        s.expect(PROMPT)
        s.send("pwd\n")
        s.expect(r"/tmp")
        s.expect(PROMPT)
        step("cd changes the shell's own directory")

        s.send("sleep 0.3 | cat\n", delay=0.1)
        s.send("\x1a")  # Ctrl-Z on a whole pipeline
        s.expect(r"Stopped\s+sleep 0.3 \| cat")
        s.expect(PROMPT)
        s.send("fg\n")
        s.expect(PROMPT)
        step("Ctrl-Z / fg work on a multi-process pipeline")

        s.send("exit\n")
        deadline = time.time() + 3
        while time.time() < deadline:
            pid, _ = os.waitpid(s.pid, os.WNOHANG)
            if pid:
                break
            time.sleep(0.05)
        else:
            raise AssertionError("shell did not exit after 'exit'")
        step("exit terminates the shell")
    except AssertionError as e:
        print("  FAIL  " + str(e))
        s.close()
        sys.exit(1)
    s.close()
    print("\npassed: %d   failed: 0" % passed)


if __name__ == "__main__":
    main()
