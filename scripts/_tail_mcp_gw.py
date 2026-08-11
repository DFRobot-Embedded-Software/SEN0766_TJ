# -*- coding: utf-8 -*-
"""在本机查看远端 mcp_gw 日志。用法:
  python scripts/_tail_mcp_gw.py          # 最近 80 行
  python scripts/_tail_mcp_gw.py -f       # 持续跟踪
  python scripts/_tail_mcp_gw.py -n 200
"""
from __future__ import annotations

import argparse
import sys
import time

import paramiko

sys.stdout.reconfigure(encoding="utf-8", errors="replace")

HOST = "192.168.1.27"
USER = "haha"
PASSWORD = "123456"
REMOTE_LOG = "/home/haha/\u684c\u9762/mcp-sensors-stack/logs/mcp_gw.log"


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("-n", type=int, default=80, help="显示末尾行数")
    ap.add_argument("-f", action="store_true", help="持续跟踪（Ctrl+C 退出）")
    args = ap.parse_args()

    ssh = paramiko.SSHClient()
    ssh.set_missing_host_key_policy(paramiko.AutoAddPolicy())
    ssh.connect(HOST, username=USER, password=PASSWORD, timeout=15)

    def run(cmd: str, timeout: int = 20) -> str:
        _i, o, e = ssh.exec_command(cmd, timeout=timeout)
        o.channel.settimeout(timeout)
        try:
            return o.read().decode("utf-8", errors="replace")
        except Exception as ex:
            return f"<err {ex}>"

    print(f"=== {HOST}:{REMOTE_LOG} ===")
    print(run(f"ls -la '{REMOTE_LOG}'; echo '---'; curl -sS -m 2 http://127.0.0.1:9080/health; echo; echo '--- devices ---'; curl -sS -m 2 http://127.0.0.1:9080/api/devices; echo"))
    print("--- log ---")
    print(run(f"tail -n {args.n} '{REMOTE_LOG}'"))

    if not args.f:
        ssh.close()
        return

    print("\n(跟踪中，Ctrl+C 停止)\n")
    # 简单轮询 tail
    last_size = run(f"stat -c%s '{REMOTE_LOG}'").strip()
    try:
        while True:
            time.sleep(1.5)
            size = run(f"stat -c%s '{REMOTE_LOG}'").strip()
            if size != last_size:
                print(run(f"tail -n 20 '{REMOTE_LOG}'"))
                print("---")
                last_size = size
    except KeyboardInterrupt:
        pass
    finally:
        ssh.close()


if __name__ == "__main__":
    main()
