#!/usr/bin/env bash
# 一键启动本目录内的全部服务（可整体拷贝移植）
# 用法：cd 到本目录后 ./start_services.sh

set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
GW_DIR="${ROOT}/mcp_gw"
BRIDGE_DIR="${ROOT}/voice-bridge"
LOG_DIR="${ROOT}/logs"
export PATH="${HOME}/.local/bin:${PATH}"

mkdir -p "${LOG_DIR}"

green() { printf '\033[0;32m%s\033[0m\n' "$*"; }
yellow() { printf '\033[0;33m%s\033[0m\n' "$*"; }
red() { printf '\033[0;31m%s\033[0m\n' "$*"; }

is_listening() {
  local port="$1"
  ss -lntp 2>/dev/null | grep -q ":${port} " && return 0
  return 1
}

# 返回系统 Python 主版本，如 3.12 / 3.14；失败则空
python_minor_ver() {
  python3 -c 'import sys; print(f"{sys.version_info.major}.{sys.version_info.minor}")' 2>/dev/null || true
}

# 尝试自动安装系统 venv/pip（需 sudo；失败则打印手动命令）
try_install_system_venv() {
  local ver pkgs=()
  ver="$(python_minor_ver)"

  if ! command -v apt-get >/dev/null 2>&1; then
    red "[错误] 非 apt 系统，无法自动安装 python3-venv"
    return 1
  fi
  if ! command -v sudo >/dev/null 2>&1; then
    red "[错误] 未找到 sudo，无法自动安装系统包"
    return 1
  fi

  pkgs+=(python3-pip python3-venv)
  if [[ -n "${ver}" ]]; then
    pkgs+=("python${ver}-venv")
  fi

  yellow "[准备] 自动安装系统包: ${pkgs[*]}"
  yellow "        （可能需要输入 sudo 密码）"
  if ! sudo DEBIAN_FRONTEND=noninteractive apt-get update -y; then
    red "[错误] apt-get update 失败"
    return 1
  fi
  # 版本专用包可能不存在，逐个装；至少一个 venv 包成功即可
  local ok=0
  local pkg
  for pkg in "${pkgs[@]}"; do
    if sudo DEBIAN_FRONTEND=noninteractive apt-get install -y "${pkg}"; then
      ok=1
    else
      yellow "[跳过] 未能安装 ${pkg}"
    fi
  done
  if [[ "${ok}" -eq 0 ]]; then
    red "[错误] 系统包安装失败，请手动："
    red "          sudo apt install -y python3-venv python3-pip"
    [[ -n "${ver}" ]] && red "          sudo apt install -y python${ver}-venv"
    return 1
  fi
  green "[成功] 系统 venv/pip 包已安装"
  return 0
}

hint_install_venv() {
  local ver
  ver="$(python_minor_ver)"
  red "[错误] 无法创建 Python 虚拟环境（缺少 ensurepip / venv 模块）"
  if [[ -n "${ver}" ]]; then
    red "          sudo apt update && sudo apt install -y python${ver}-venv python3-pip"
  fi
  red "          sudo apt install -y python3-venv python3-pip"
}

# 半残 venv：有目录但没有可用 python 或 pip
venv_is_broken() {
  local dir="$1"
  local py="${dir}/venv/bin/python"
  local pip="${dir}/venv/bin/pip"
  [[ -d "${dir}/venv" ]] || return 1
  if [[ ! -x "${py}" ]]; then
    return 0
  fi
  if [[ ! -x "${pip}" ]] && [[ ! -x "${dir}/venv/bin/pip3" ]]; then
    return 0
  fi
  # ensurepip 失败时常见：python 在，但 import pip 失败
  if ! "${py}" -c "import pip" >/dev/null 2>&1; then
    return 0
  fi
  return 1
}

# 按服务检查关键 import，缺则触发重装 requirements
venv_needs_deps() {
  local dir="$1"
  local py="${dir}/venv/bin/python"
  case "$(basename "${dir}")" in
    mcp_gw)
      "${py}" -c "import uvicorn, starlette, httpx, zeroconf, aiomqtt" >/dev/null 2>&1 || return 0
      ;;
    voice-bridge)
      "${py}" -c "import dashscope, websockets, httpx" >/dev/null 2>&1 || return 0
      ;;
    *)
      return 1
      ;;
  esac
  return 1
}

install_requirements() {
  local dir="$1"
  local py="${dir}/venv/bin/python"
  local pip="${dir}/venv/bin/pip"
  [[ -x "${pip}" ]] || pip="${dir}/venv/bin/pip3"
  if [[ ! -x "${pip}" ]]; then
    # 某些发行版只有 python -m pip
    if ! "${py}" -m pip --version >/dev/null 2>&1; then
      red "[错误] venv 内无 pip: ${dir}/venv"
      if try_install_system_venv; then
        rm -rf "${dir}/venv"
        return 2  # 告诉调用方：需重建 venv
      fi
      hint_install_venv
      return 1
    fi
    yellow "[准备] 安装依赖: ${dir}"
    "${py}" -m pip install -q -U pip
    "${py}" -m pip install -q -r "${dir}/requirements.txt"
    return 0
  fi
  yellow "[准备] 安装依赖: ${dir}"
  "${pip}" install -q -U pip
  "${pip}" install -q -r "${dir}/requirements.txt"
}

create_venv_once() {
  local dir="$1"
  yellow "[准备] 创建 venv: ${dir}"
  if ! python3 -m venv "${dir}/venv"; then
    rm -rf "${dir}/venv"
    return 1
  fi
  if venv_is_broken "${dir}"; then
    rm -rf "${dir}/venv"
    return 1
  fi
  return 0
}

ensure_venv() {
  local dir="$1"
  if [[ ! -f "${dir}/requirements.txt" ]]; then
    red "[错误] 缺少 ${dir}/requirements.txt"
    return 1
  fi

  if venv_is_broken "${dir}"; then
    yellow "[清理] 半残 venv，将重建: ${dir}/venv"
    rm -rf "${dir}/venv"
  fi

  if [[ ! -x "${dir}/venv/bin/python" ]]; then
    if ! create_venv_once "${dir}"; then
      yellow "[重试] 创建失败，尝试自动安装系统 python3-venv ..."
      try_install_system_venv || { hint_install_venv; return 1; }
      if ! create_venv_once "${dir}"; then
        hint_install_venv
        return 1
      fi
    fi
    local rc=0
    install_requirements "${dir}" || rc=$?
    if [[ "${rc}" -eq 2 ]]; then
      create_venv_once "${dir}" || { hint_install_venv; return 1; }
      install_requirements "${dir}" || return 1
    elif [[ "${rc}" -ne 0 ]]; then
      return 1
    fi
  elif venv_needs_deps "${dir}"; then
    yellow "[准备] venv 缺少依赖，补装: ${dir}"
    local rc=0
    install_requirements "${dir}" || rc=$?
    if [[ "${rc}" -eq 2 ]]; then
      create_venv_once "${dir}" || { hint_install_venv; return 1; }
      install_requirements "${dir}" || return 1
    elif [[ "${rc}" -ne 0 ]]; then
      return 1
    fi
    if venv_needs_deps "${dir}"; then
      red "[错误] 依赖安装后仍缺少模块，请检查网络或手动："
      red "          ${dir}/venv/bin/pip install -r ${dir}/requirements.txt"
      return 1
    fi
  fi
  return 0
}

start_hermes() {
  if is_listening 8642; then
    yellow "[跳过] 爱马仕 gateway 已在运行 (:8642)"
    return 0
  fi
  if ! command -v hermes >/dev/null 2>&1; then
    red "[警告] 未找到 hermes（语音问答需要）。请先安装爱马仕后再运行本脚本。"
    red "        curl -fsSL https://hermes-agent.nousresearch.com/install.sh | bash"
    return 1
  fi
  green "[启动] 爱马仕 gateway ..."
  nohup hermes gateway >>"${LOG_DIR}/hermes_gateway.log" 2>&1 </dev/null &
  sleep 3
  if is_listening 8642; then
    green "[成功] 爱马仕 → http://127.0.0.1:8642"
  else
    red "[失败] 见日志 ${LOG_DIR}/hermes_gateway.log"
    return 1
  fi
}

start_mcp_gw() {
  if is_listening 9080; then
    yellow "[跳过] mcp_gw 已在运行 (:9080)"
    return 0
  fi
  ensure_venv "${GW_DIR}" || return 1
  if [[ ! -f "${GW_DIR}/.env" && -f "${GW_DIR}/.env.example" ]]; then
    cp "${GW_DIR}/.env.example" "${GW_DIR}/.env"
    # 尝试写入本机局域网 IP
    local ip
    ip="$(hostname -I 2>/dev/null | awk '{print $1}')"
    if [[ -n "${ip}" ]]; then
      if grep -q '^MCP_GW_ADVERTISE_IP=' "${GW_DIR}/.env"; then
        sed -i "s|^MCP_GW_ADVERTISE_IP=.*|MCP_GW_ADVERTISE_IP=${ip}|" "${GW_DIR}/.env"
      else
        echo "MCP_GW_ADVERTISE_IP=${ip}" >>"${GW_DIR}/.env"
      fi
    fi
    yellow "[提示] 已生成 ${GW_DIR}/.env （可按需修改 ADVERTISE_IP）"
  fi
  green "[启动] mcp_gw ..."
  (
    cd "${GW_DIR}"
    set -a
    # shellcheck disable=SC1091
    [[ -f .env ]] && . ./.env
    set +a
    nohup ./venv/bin/python -u -m mcp_gw >>"${LOG_DIR}/mcp_gw.log" 2>&1 </dev/null &
  )
  sleep 5
  if is_listening 9080; then
    green "[成功] mcp_gw → http://127.0.0.1:9080"
  else
    red "[失败] 见日志 ${LOG_DIR}/mcp_gw.log"
    return 1
  fi
}

start_bridge() {
  if is_listening 8765; then
    yellow "[跳过] 语音 bridge 已在运行 (:8765)"
    return 0
  fi
  ensure_venv "${BRIDGE_DIR}" || return 1
  if [[ ! -f "${BRIDGE_DIR}/.env" ]]; then
    if [[ -f "${BRIDGE_DIR}/.env.example" ]]; then
      cp "${BRIDGE_DIR}/.env.example" "${BRIDGE_DIR}/.env"
      yellow "[提示] 已生成 ${BRIDGE_DIR}/.env ，请填入 DASHSCOPE_API_KEY"
    else
      red "[错误] 缺少 ${BRIDGE_DIR}/.env"
      return 1
    fi
  fi

  # HERMES_API_KEY 为空时，自动从 ~/.hermes/.env 的 API_SERVER_KEY 同步
  sync_hermes_api_key || true

  # shellcheck disable=SC1091
  set -a; . "${BRIDGE_DIR}/.env"; set +a
  if [[ -z "${HERMES_API_KEY:-}" ]]; then
    red "[错误] HERMES_API_KEY 未设置"
    red "        在 ~/.hermes/.env 设置 API_SERVER_KEY=任意字符串，"
    red "        再把同一值写入 voice-bridge/.env 的 HERMES_API_KEY="
    return 1
  fi
  if [[ -z "${DASHSCOPE_API_KEY:-}" ]]; then
    yellow "[警告] DASHSCOPE_API_KEY 为空（ASR/TTS 会失败）"
  fi

  green "[启动] 语音 bridge ..."
  (
    cd "${BRIDGE_DIR}"
    set -a
    # shellcheck disable=SC1091
    # 去掉 CRLF，避免 Windows 编辑的 .env 把 \\r 带进 API Key（aiohttp 会报 header injection）
    if [[ -f .env ]]; then
      tmp="$(mktemp)"
      tr -d '\r' < .env >"${tmp}"
      # shellcheck disable=SC1090
      . "${tmp}"
      rm -f "${tmp}"
    fi
    set +a
    nohup ./venv/bin/python -u bridge.py >>"${LOG_DIR}/bridge.log" 2>&1 </dev/null &
  )
  sleep 2
  if is_listening 8765; then
    green "[成功] bridge → ws://0.0.0.0:8765/ws"
  else
    red "[失败] 见日志 ${LOG_DIR}/bridge.log"
    return 1
  fi
}

sync_hermes_api_key() {
  local bridge_env="${BRIDGE_DIR}/.env"
  local hermes_env="${HOME}/.hermes/.env"
  local current key
  current="$(grep -E '^HERMES_API_KEY=' "${bridge_env}" 2>/dev/null | head -1 | cut -d= -f2- | tr -d '\r' || true)"
  if [[ -n "${current}" ]]; then
    return 0
  fi
  if [[ ! -f "${hermes_env}" ]]; then
    return 1
  fi
  key="$(grep -E '^API_SERVER_KEY=' "${hermes_env}" | head -1 | cut -d= -f2- | tr -d '\r' || true)"
  if [[ -z "${key}" ]]; then
    return 1
  fi
  if grep -q '^HERMES_API_KEY=' "${bridge_env}"; then
    sed -i "s|^HERMES_API_KEY=.*|HERMES_API_KEY=${key}|" "${bridge_env}"
  else
    echo "HERMES_API_KEY=${key}" >>"${bridge_env}"
  fi
  if ! grep -q '^HERMES_API_BASE=' "${bridge_env}"; then
    echo "HERMES_API_BASE=http://127.0.0.1:8642" >>"${bridge_env}"
  fi
  green "[同步] HERMES_API_KEY ← ~/.hermes/.env 的 API_SERVER_KEY"
}

show_status() {
  echo
  echo "======== ${ROOT} ========"
  if is_listening 9080; then green "mcp_gw     : 运行中  :9080"; else red "mcp_gw     : 未运行"; fi
  if is_listening 8765; then green "语音 bridge: 运行中  :8765"; else red "语音 bridge: 未运行"; fi
  if is_listening 8642; then green "爱马仕 API : 运行中  :8642"; else red "爱马仕 API : 未运行"; fi
  echo "日志: ${LOG_DIR}"
  echo "=============================="
}

echo ">>> 启动（目录: ${ROOT}）"
# 先起 mcp_gw，再起爱马仕，避免爱马仕连 127.0.0.1:9080 时 Connection refused
start_mcp_gw || true
start_hermes || true
start_bridge || true
show_status
echo
echo "停止: ./stop_services.sh"