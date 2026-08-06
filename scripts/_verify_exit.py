# -*- coding: utf-8 -*-
import sys
import paramiko

sys.stdout.reconfigure(encoding="utf-8", errors="replace")
REMOTE = "/home/dfrobot/\u684c\u9762/hermes-voice-bridge"

ssh = paramiko.SSHClient()
ssh.set_missing_host_key_policy(paramiko.AutoAddPolicy())
ssh.connect("192.168.0.73", username="dfrobot", password="123456", timeout=15)


def run(cmd, timeout=25):
    _i, o, e = ssh.exec_command(cmd, timeout=timeout)
    o.channel.settimeout(timeout)
    try:
        out = o.read().decode("utf-8", errors="replace")
    except Exception as ex:
        out = f"<err {ex}>"
    try:
        err = e.read().decode("utf-8", errors="replace")
    except Exception:
        err = ""
    return (out + err).strip()


print("=== log files ===")
print(run(f"ls -la '{REMOTE}'/*.log* 2>/dev/null; ls /tmp/*bridge* 2>/dev/null || true"))
print("\n=== EXIT_PHRASES / is_exit ===")
print(run(f"sed -n '87,165p' '{REMOTE}/bridge.py'"))
print("\n=== exit decision ===")
print(run(f"sed -n '676,705p' '{REMOTE}/bridge.py'"))
print("\n=== match test ===")
print(
    run(
        f"cd '{REMOTE}' && ./venv/bin/python -c \""
        "from bridge import is_exit_command, looks_like_farewell_reply; "
        "print('退下吧', is_exit_command('退下吧。')); "
        "print('Bye bye', is_exit_command('Bye bye.')); "
        "print('farewell', looks_like_farewell_reply('好的，再见！有需要随时叫我。'))"
        "\""
    )
)
ssh.close()
