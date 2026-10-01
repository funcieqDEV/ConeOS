#!/usr/bin/env python3
"""Exercise IRQ routing through the normal userspace shell and PS/2 keyboard."""

import json
import os
from pathlib import Path
import re
import shutil
import socket
import subprocess
import tempfile
import time


ROOT = Path(__file__).resolve().parents[1]
TIMEOUT = float(os.environ.get("CONEOS_TEST_TIMEOUT", "30"))
FAILURES = re.compile(r"KERNEL PANIC|failed to mount initramfs|User process exception")


class QMP:
    def __init__(self, path, process):
        self.socket = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        deadline = time.monotonic() + TIMEOUT
        while True:
            try:
                self.socket.connect(str(path))
                break
            except (FileNotFoundError, ConnectionRefusedError):
                if process.poll() is not None or time.monotonic() >= deadline:
                    self.socket.close()
                    raise RuntimeError("QEMU did not open QMP")
                time.sleep(0.05)
        self.socket.settimeout(5)
        self.stream = self.socket.makefile("rwb")
        greeting = json.loads(self.stream.readline())
        if "QMP" not in greeting:
            raise RuntimeError(f"invalid QMP greeting: {greeting}")
        self.command("qmp_capabilities")

    def command(self, execute, arguments=None):
        request = {"execute": execute}
        if arguments is not None:
            request["arguments"] = arguments
        self.stream.write(json.dumps(request).encode() + b"\n")
        self.stream.flush()
        while True:
            line = self.stream.readline()
            if not line:
                raise RuntimeError("QMP disconnected")
            response = json.loads(line)
            if "error" in response:
                raise RuntimeError(f"QMP {execute}: {response['error']}")
            if "return" in response:
                return response["return"]

    def type_command(self, text):
        names = {" ": "spc", "-": "minus", "\n": "ret"}
        for character in text + "\n":
            self.command("send-key", {
                "keys": [{"type": "qcode", "data": names.get(character, character)}],
                "hold-time": 20,
            })
            time.sleep(0.08)

    def close(self):
        self.stream.close()
        self.socket.close()


def wait_log(path, process, predicate):
    deadline = time.monotonic() + TIMEOUT
    while time.monotonic() < deadline:
        output = path.read_text(errors="replace") if path.exists() else ""
        if FAILURES.search(output):
            raise RuntimeError("kernel or userspace failure")
        if predicate(output):
            return output
        if process.poll() is not None:
            raise RuntimeError(f"QEMU exited with status {process.returncode}")
        time.sleep(0.1)
    raise RuntimeError("timed out waiting for guest output")


def run_case(name, machine, cpus, cpu="max", extra=(), controller="APIC", firmware=None):
    log = ROOT / "build" / f"apic-{name}.log"
    errors = ROOT / "build" / f"apic-{name}-qemu.log"
    with tempfile.TemporaryDirectory(prefix="coneos-apic-") as temporary:
        if firmware:
            code = Path(firmware).resolve()
            variables = Path(os.environ.get("CONEOS_OVMF_VARS", str(code).replace("CODE", "VARS")))
            copy = Path(temporary) / "vars.fd"
            shutil.copyfile(variables, copy)
            extra = (*extra, "-drive", f"if=pflash,format=raw,unit=0,readonly=on,file={code}",
                     "-drive", f"if=pflash,format=raw,unit=1,file={copy}")
        command = [
            "qemu-system-x86_64", "-M", machine, "-cpu", cpu, "-smp", str(cpus),
            "-m", "256M", "-cdrom", str(ROOT / "ConeOS.iso"), "-boot", "d",
            "-display", "none", "-serial", f"file:{log}", "-no-reboot",
            "-qmp", f"unix:{temporary}/qmp.sock,server=on,wait=off", *extra,
        ]
        with errors.open("w") as stderr:
            process = subprocess.Popen(command, stdout=stderr, stderr=stderr)
            qmp = None
            try:
                qmp = QMP(Path(temporary) / "qmp.sock", process)
                output = wait_log(log, process, lambda text: "user> " in text)
                if f"IRQ controller: {controller}" not in output:
                    raise RuntimeError(f"expected {controller} interrupt controller")
                if controller == "APIC":
                    if f"ACPI MADT: {cpus} enabled CPUs" not in output:
                        raise RuntimeError("MADT CPU count mismatch")
                    if "APIC ISA routes: IRQ0 -> GSI 2, IRQ1 -> GSI 1" not in output:
                        raise RuntimeError("ISA override routing mismatch")
                qmp.type_command("echo apic-keyboard-ok")
                wait_log(log, process, lambda text: "\napic-keyboard-ok\n" in text)
                qmp.type_command("uptime")
                output = wait_log(log, process, lambda text: re.search(r"\bup [1-9][0-9]* ms", text))
                first = int(re.findall(r"\bup ([0-9]+) ms", output)[-1])
                time.sleep(0.15)
                qmp.type_command("uptime")
                wait_log(log, process, lambda text: len(re.findall(r"\bup ([0-9]+) ms", text)) >= 2
                         and int(re.findall(r"\bup ([0-9]+) ms", text)[-1]) > first)
                print(f"APIC boot test passed: {name} ({controller}, {cpus} vCPUs)", flush=True)
            except Exception:
                if log.exists():
                    print(log.read_text(errors="replace"), flush=True)
                print(errors.read_text(errors="replace"), flush=True)
                raise
            finally:
                if qmp:
                    qmp.close()
                process.terminate()
                try:
                    process.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()


def main():
    run_case("q35", "q35", 4)
    run_case("i440fx", "pc", 2)
    run_case("no-acpi", "pc,acpi=off", 1, controller="PIC")
    run_case("no-apic", "pc", 1, cpu="qemu64,-apic", controller="PIC")
    firmware = os.environ.get("CONEOS_OVMF_CODE")
    if not firmware:
        candidates = ("/usr/share/OVMF/OVMF_CODE.fd", "/usr/share/edk2/ovmf/OVMF_CODE.fd")
        firmware = next((path for path in candidates if Path(path).is_file()), None)
    if firmware:
        run_case("uefi", "q35", 4, firmware=firmware)
    else:
        print("UEFI test skipped: set CONEOS_OVMF_CODE to an OVMF code image", flush=True)


if __name__ == "__main__":
    main()
