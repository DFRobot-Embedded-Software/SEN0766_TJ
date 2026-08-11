# -*- coding: utf-8 -*-
"""部署 server_stack/mcp_gw 到远端 mcp-sensors-stack（兼容旧入口）。"""
from __future__ import annotations

import sys
from pathlib import Path

# 转调整栈部署
sys.path.insert(0, str(Path(__file__).resolve().parent))
import runpy

runpy.run_path(str(Path(__file__).resolve().parent / "_deploy_server_stack.py"), run_name="__main__")
