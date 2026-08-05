#!/usr/bin/env python3
"""
设备语音中转：WebSocket PCM → 千问流式 ASR → 爱马仕(MCP) → CosyVoice TTS → 回传设备

设备协议（与 ESP32 对齐）：

上行（设备 → 网关）：
  1) {"op":"uplink_start","rate":16000,"format":"s16le","channels":1}
  2) Binary：16-bit LE PCM，16 kHz mono
  3) {"op":"uplink_stop","reason":"vad"|"max"|"error"|"manual"}
     - vad: AFE 连续静音 ≥ EOS_MS（默认 1s）
     - max: 单次上行达到 MAX_MS（默认 15s）
  uplink_stop 后设备保持连接，等待下行 TTS。

下行（网关 → 设备）：
  - {"op":"ready", ...}
  - {"op":"listen_stop"} / {"op":"transcript",...} / {"op":"processing"}  （processing 为处理中心跳，设备续命 await）
  - {"op":"reply","text":"..."}
  - {"op":"play_start"}
  - Binary：16k s16le PCM → ns4168_amp_write
  - {"op":"play_stop"}   ← 网关 TTS 播完必须发送
  播放期间不重开上行。
  设备在 play_stop 后保持连接，等待下一轮 uplink_start；
  网关同样保持连接，不再每轮强制 close（避免下行被掐断、二次建连失败）。
  设备等待下行不靠「猜总耗时」，而靠 processing 心跳 + play_stop 结束。

用法：
  export DASHSCOPE_API_KEY=sk-xxx
  source venv/bin/activate
  python bridge.py
"""

from __future__ import annotations

import asyncio
import json
import logging
import os
import threading
import time
from typing import Optional

import dashscope
import httpx
from dashscope.audio.asr import Recognition, RecognitionCallback, RecognitionResult
from dashscope.audio.tts_v2 import AudioFormat, ResultCallback, SpeechSynthesizer
from websockets.asyncio.server import ServerConnection, serve

# ---------------------------------------------------------------------------
# 配置
# ---------------------------------------------------------------------------

DASHSCOPE_API_KEY = os.environ.get("DASHSCOPE_API_KEY", "")
DEVICE_WS_HOST = os.environ.get("BRIDGE_HOST", "0.0.0.0")
DEVICE_WS_PORT = int(os.environ.get("BRIDGE_PORT", "8765"))

ASR_MODEL = os.environ.get("ASR_MODEL", "qwen-audio-3.0-asr-flash-streaming")
ASR_SAMPLE_RATE = int(os.environ.get("ASR_SAMPLE_RATE", "16000"))
ASR_FORMAT = "pcm"

TTS_MODEL = os.environ.get("TTS_MODEL", "cosyvoice-v3-flash")
TTS_VOICE = os.environ.get("TTS_VOICE", "longanyang")
# 与设备播放格式对齐：默认 16k s16le PCM
TTS_SAMPLE_RATE = int(os.environ.get("TTS_SAMPLE_RATE", "16000"))
# play_stop 后稍等，给设备排空播放缓冲；不再关连接
PLAY_STOP_SETTLE_MS = int(os.environ.get("PLAY_STOP_SETTLE_MS", "500"))
# listen_stop 后给设备停上行的时间，避免边收 TTS 边 send_bin → poll_write(0)
LISTEN_STOP_SETTLE_MS = int(os.environ.get("LISTEN_STOP_SETTLE_MS", "1200"))
# 单帧二进制上限：须小于设备 esp_websocket buffer_size（当前 8KB）
PCM_CHUNK_BYTES = int(os.environ.get("PCM_CHUNK_BYTES", str(640)))  # 20ms @16k mono s16le
# 下发倍率：略高于实时，保持设备预缓冲水位，减少中途 underrun
PCM_SEND_SPEED = float(os.environ.get("PCM_SEND_SPEED", "1.05"))
# 单帧 send 超时；配合写缓冲反压，避免长音频中途永久卡在 drain
PCM_SEND_TIMEOUT_S = float(os.environ.get("PCM_SEND_TIMEOUT_S", "12"))
# 网关侧写缓冲超过该值则等待设备消化（字节）
PCM_WRITE_BUF_LIMIT = int(os.environ.get("PCM_WRITE_BUF_LIMIT", str(24 * 1024)))
# 爱马仕 HTTP 超时（可较长）；设备靠 processing 心跳续命，不依赖此值对齐
HERMES_HTTP_TIMEOUT_S = float(os.environ.get("HERMES_HTTP_TIMEOUT_S", "120"))
# 处理中心跳间隔：须明显小于设备 AUDIO_WS_DOWNLINK_WAIT_MS（静默看门狗）
PROCESSING_HEARTBEAT_S = float(os.environ.get("PROCESSING_HEARTBEAT_S", "5"))
# 1=流式 CosyVoice；0=批量合成再分块下发（更稳，推荐）
USE_STREAM_TTS = os.environ.get("USE_STREAM_TTS", "0") == "1"


def _tts_audio_format() -> AudioFormat:
    mapping = {
        8000: AudioFormat.PCM_8000HZ_MONO_16BIT,
        16000: AudioFormat.PCM_16000HZ_MONO_16BIT,
        22050: AudioFormat.PCM_22050HZ_MONO_16BIT,
        24000: AudioFormat.PCM_24000HZ_MONO_16BIT,
        44100: AudioFormat.PCM_44100HZ_MONO_16BIT,
        48000: AudioFormat.PCM_48000HZ_MONO_16BIT,
    }
    fmt = mapping.get(TTS_SAMPLE_RATE)
    if fmt is None:
        raise RuntimeError(f"不支持的 TTS_SAMPLE_RATE={TTS_SAMPLE_RATE}")
    return fmt

HERMES_API_BASE = os.environ.get("HERMES_API_BASE", "http://127.0.0.1:8642").rstrip("/")
HERMES_API_KEY = os.environ.get("HERMES_API_KEY", "")
HERMES_MODEL = os.environ.get("HERMES_MODEL", "")

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s [%(levelname)s] %(message)s",
)
log = logging.getLogger("bridge")

dashscope.api_key = DASHSCOPE_API_KEY
dashscope.base_websocket_api_url = "wss://dashscope.aliyuncs.com/api-ws/v1/inference"


def _json(op: str, **kwargs) -> str:
    return json.dumps({"op": op, **kwargs}, ensure_ascii=False)


# ---------------------------------------------------------------------------
# 爱马仕
# ---------------------------------------------------------------------------

def ask_hermes(user_text: str) -> str:
    text = (user_text or "").strip()
    if not text:
        return "我没有听清，请再说一次。"

    if not HERMES_API_KEY:
        log.warning("未配置 HERMES_API_KEY，使用 mock 回复")
        return _mock_hermes(text, reason="no_key")

    url = f"{HERMES_API_BASE}/v1/chat/completions"
    headers = {
        "Authorization": f"Bearer {HERMES_API_KEY}",
        "Content-Type": "application/json",
    }
    body = {
        "messages": [
            {
                "role": "system",
                "content": (
                    "你是语音助手。用户会询问传感器温湿度等。"
                    "优先使用可用工具/MCP 查询真实数据，用一两句中文简洁回答。"
                ),
            },
            {"role": "user", "content": text},
        ],
        "stream": False,
    }
    if HERMES_MODEL:
        body["model"] = HERMES_MODEL

    try:
        with httpx.Client(timeout=HERMES_HTTP_TIMEOUT_S) as client:
            r = client.post(url, headers=headers, json=body)
            r.raise_for_status()
            data = r.json()
        reply = data["choices"][0]["message"]["content"]
        return (reply or "").strip() or "查询完成，但没有返回内容。"
    except Exception as e:
        log.warning("调用爱马仕失败，回退 mock: %s", e)
        return _mock_hermes(text, reason=str(e))


def _mock_hermes(text: str, reason: str = "") -> str:
    if any(k in text for k in ("温湿", "温度", "湿度", "sensor", "温")):
        return "当前温度二十五点三摄氏度，湿度百分之六十。这是模拟数据。"
    if reason:
        return f"我听到了：{text}。助手暂时响应超时，请稍后再试。"
    return f"我收到了：{text}。"


# ---------------------------------------------------------------------------
# CosyVoice TTS（流式）→ 边合成边下发设备
# ---------------------------------------------------------------------------

_TTS_DONE = object()


async def stream_tts_pcm(text: str, send_pcm):
    """用 CosyVoice 流式合成，每收到一段 PCM 就 await send_pcm(chunk)。"""
    if not DASHSCOPE_API_KEY:
        raise RuntimeError("缺少 DASHSCOPE_API_KEY")

    loop = asyncio.get_running_loop()
    queue: asyncio.Queue = asyncio.Queue()
    error_box: list[str] = []
    total = 0
    finished = threading.Event()
    done_once = threading.Lock()
    signaled = {"v": False}

    def _signal_done() -> None:
        with done_once:
            if signaled["v"]:
                return
            signaled["v"] = True
        finished.set()
        try:
            loop.call_soon_threadsafe(queue.put_nowait, _TTS_DONE)
        except RuntimeError:
            pass

    class _Cb(ResultCallback):
        def on_open(self):
            log.info("TTS WebSocket 已连接")

        def on_data(self, data: bytes) -> None:
            if data:
                try:
                    loop.call_soon_threadsafe(queue.put_nowait, data)
                except RuntimeError:
                    pass

        def on_complete(self):
            log.info("TTS 合成完成")
            _signal_done()

        def on_error(self, message: str):
            error_box.append(str(message))
            log.error("TTS on_error: %s", message)
            _signal_done()

        def on_close(self):
            # 不要仅凭 on_close 收尾：会出现“先 close、后 complete、音频全丢”
            log.info("TTS WebSocket 已关闭")

    def _run():
        try:
            synthesizer = SpeechSynthesizer(
                model=TTS_MODEL,
                voice=TTS_VOICE,
                format=_tts_audio_format(),
                callback=_Cb(),
            )
            synthesizer.call(text)
            if not finished.wait(timeout=60):
                error_box.append("TTS 超时")
                _signal_done()
        except Exception as e:
            error_box.append(str(e))
            _signal_done()

    worker = asyncio.create_task(asyncio.to_thread(_run))
    try:
        while True:
            item = await queue.get()
            if item is _TTS_DONE:
                break
            total += len(item)
            await send_pcm(item)
    finally:
        await worker

    if error_box:
        raise RuntimeError(f"TTS 失败: {error_box[0]}")
    if total == 0:
        raise RuntimeError("TTS 未返回音频数据")
    return total


def synthesize_tts_pcm(text: str) -> bytes:
    """非流式一次性合成（回调为空时 call 直接返回完整 PCM）。"""
    if not DASHSCOPE_API_KEY:
        raise RuntimeError("缺少 DASHSCOPE_API_KEY")
    synthesizer = SpeechSynthesizer(
        model=TTS_MODEL,
        voice=TTS_VOICE,
        format=_tts_audio_format(),
    )
    audio = synthesizer.call(text)
    if not audio:
        raise RuntimeError("TTS 未返回音频数据")
    return audio


async def send_pcm_chunks(send_pcm, pcm: bytes) -> int:
    """把 PCM 切成小帧下发并限速，避免撑爆设备 WS buffer / TCP 窗口。"""
    if not pcm:
        return 0
    n = 0
    bytes_per_sec = float(ASR_SAMPLE_RATE * 2)
    speed = PCM_SEND_SPEED if PCM_SEND_SPEED > 0.1 else 1.0
    last_log = 0
    t0 = time.monotonic()
    # 先快速灌约 1.0s，配合设备预缓冲；再略快于实时，维持水位
    preburst = min(len(pcm), int(bytes_per_sec * 1.0))
    paced = False
    for i in range(0, len(pcm), PCM_CHUNK_BYTES):
        chunk = pcm[i : i + PCM_CHUNK_BYTES]
        await send_pcm(chunk)
        n += len(chunk)
        if n - last_log >= 32000:
            log.info("下行进度 %s/%s bytes", n, len(pcm))
            last_log = n
        if n < preburst:
            await asyncio.sleep(0)
            continue
        if not paced:
            # 预灌结束后，把墙钟对齐到「已发送时长」，再按实时限速
            t0 = time.monotonic() - (n / bytes_per_sec) / speed
            paced = True
        due = t0 + (n / bytes_per_sec) / speed
        delay = due - time.monotonic()
        if delay > 0.001:
            await asyncio.sleep(delay)
        else:
            await asyncio.sleep(0)
    return n


async def wait_conn_writable(conn: ServerConnection, limit: int = PCM_WRITE_BUF_LIMIT) -> None:
    """设备 TCP 窗口变小时等待，防止 send 卡在 drain 导致长音频发不完。"""
    for _ in range(400):  # 最多约 20s
        if getattr(conn, "close_code", None) is not None:
            raise ConnectionError("device disconnected")
        transport = None
        protocol = getattr(conn, "protocol", None)
        if protocol is not None:
            transport = getattr(protocol, "transport", None)
        if transport is None:
            transport = getattr(conn, "transport", None)
        size = 0
        if transport is not None and hasattr(transport, "get_write_buffer_size"):
            try:
                size = int(transport.get_write_buffer_size())
            except Exception:
                size = 0
        if size <= limit:
            return
        await asyncio.sleep(0.05)
    raise TimeoutError(f"write buffer stuck above {limit} bytes")


async def forward_pcm_chunked(send_pcm, chunk: bytes) -> None:
    """流式路径：CosyVoice 单次 on_data 可能很大，必须再切帧。"""
    if not chunk:
        return
    if len(chunk) <= PCM_CHUNK_BYTES:
        await send_pcm(chunk)
        return
    await send_pcm_chunks(send_pcm, chunk)


# ---------------------------------------------------------------------------
# 设备会话
# ---------------------------------------------------------------------------

class DeviceSession:
    VALID_STOP_REASONS = frozenset({"vad", "max", "error", "manual"})

    def __init__(self, conn: ServerConnection, loop: asyncio.AbstractEventLoop):
        self.conn = conn
        self.loop = loop
        self.recognition: Optional[Recognition] = None
        self.uplink_enabled = False
        self.playing = False  # 下行播放中：忽略新的 uplink_start
        self.last_partial = ""
        self.got_final = False
        self._busy = False
        self._lock = threading.Lock()
        self._closed = False
        self._asr_stopping = False  # 主动 stop 后忽略迟到的 timeout
        self._finalize_task: Optional[asyncio.Task] = None

    def start_asr(self) -> None:
        """每次 uplink_start 新建一路 ASR；不要在整个 WS 生命周期空挂。"""
        self.stop_asr(reason="restart")
        if not DASHSCOPE_API_KEY:
            raise RuntimeError("缺少 DASHSCOPE_API_KEY")

        session = self
        self._asr_stopping = False

        class CB(RecognitionCallback):
            def on_open(self) -> None:
                log.info("ASR 通道已打开")

            def on_close(self) -> None:
                log.info("ASR 通道已关闭")

            def on_complete(self) -> None:
                log.info("ASR 识别完成")

            def on_error(self, message) -> None:
                err = getattr(message, "message", message)
                if session._asr_stopping or session._closed:
                    log.info("ASR 已主动结束，忽略后续错误: %s", err)
                    return
                log.error("ASR 错误: %s", err)

            def on_event(self, result: RecognitionResult) -> None:
                if session._asr_stopping or session._closed or not session.uplink_enabled:
                    return
                sentence = result.get_sentence() or {}
                text = sentence.get("text") or ""
                if not text:
                    return
                session.last_partial = text
                log.info("ASR partial: %s", text)
                if RecognitionResult.is_sentence_end(sentence):
                    log.info("ASR sentence end: %s", text)
                    # 禁止在 ASR 回调线程里 rec.stop()（会 cannot join current thread）
                    # 停 ASR + 业务放到事件循环线程执行
                    asyncio.run_coroutine_threadsafe(
                        session._on_sentence_from_asr(text), session.loop
                    )

        self.recognition = Recognition(
            model=ASR_MODEL,
            format=ASR_FORMAT,
            sample_rate=ASR_SAMPLE_RATE,
            semantic_punctuation_enabled=False,
            callback=CB(),
        )
        self.recognition.start()
        log.info("ASR 已启动 model=%s", ASR_MODEL)

    def stop_asr(self, reason: str = "") -> None:
        if not self.recognition:
            return
        self._asr_stopping = True
        rec = self.recognition
        self.recognition = None

        def _stop():
            try:
                rec.stop()
                log.info("ASR 已停止 (%s)", reason or "stop")
            except Exception as e:
                log.info("ASR stop: %s", e)

        # stop() 可能 join 识别线程，必须在非回调线程调用
        threading.Thread(target=_stop, daemon=True).start()

    def feed_pcm(self, pcm: bytes) -> None:
        if self._closed or not self.uplink_enabled or not self.recognition or not pcm:
            return
        try:
            self.recognition.send_audio_frame(pcm)
        except Exception as e:
            if self._asr_stopping or self.got_final:
                return
            log.warning("送入 ASR 失败: %s", e)

    async def _on_sentence_from_asr(self, text: str) -> None:
        self.uplink_enabled = False
        self.stop_asr(reason="sentence_end")
        # 立刻通知设备停上行，否则会边收 TTS 边 send_bin，易 TCP abort
        try:
            await self.conn.send(_json("listen_stop", reason="asr"))
            log.info("已发送 listen_stop，要求设备停止上行")
        except Exception as e:
            log.warning("发送 listen_stop 失败: %s", e)
        # 等设备处理 listen_stop 并停 send_bin，再开始下行
        await asyncio.sleep(LISTEN_STOP_SETTLE_MS / 1000.0)
        await self.on_sentence(text)

    async def on_sentence(self, text: str) -> None:
        text = (text or "").strip()
        if not text:
            return

        with self._lock:
            if self._busy:
                log.info("上一轮还在处理，跳过: %s", text)
                return
            self._busy = True
            self.got_final = True

        self.uplink_enabled = False

        try:
            await self.conn.send(_json("transcript", text=text))

            # Hermes/TTS 准备期间持续发 processing，设备只认「静默看门狗」而非总时长上限
            stop_hb = asyncio.Event()

            async def _processing_heartbeat() -> None:
                while not stop_hb.is_set() and not self._closed:
                    try:
                        await self.conn.send(_json("processing"))
                        log.info("processing heartbeat")
                    except Exception as e:
                        log.warning("processing heartbeat 失败: %s", e)
                        return
                    try:
                        await asyncio.wait_for(stop_hb.wait(), timeout=PROCESSING_HEARTBEAT_S)
                        return
                    except asyncio.TimeoutError:
                        continue

            hb_task = asyncio.create_task(_processing_heartbeat())
            try:
                reply = await asyncio.to_thread(ask_hermes, text)
            finally:
                stop_hb.set()
                try:
                    await hb_task
                except Exception:
                    pass

            log.info("Hermes 回复: %s", reply)
            if not self._closed:
                await self.conn.send(_json("reply", text=reply))

            if self._closed:
                log.warning("连接已断开，放弃 TTS")
                return

            self.playing = True
            first = True
            sent_bytes = 0

            async def _send_one_frame(frame: bytes) -> None:
                nonlocal first, sent_bytes
                if self._closed:
                    raise ConnectionError("device disconnected during play")
                if first:
                    await self.conn.send(_json("play_start"))
                    log.info("play_start（首包 PCM 到达后再发）")
                    first = False
                # 先等写缓冲降下来，再 send，避免长音频中途卡在 TCP drain
                await wait_conn_writable(self.conn)
                await asyncio.wait_for(self.conn.send(frame), timeout=PCM_SEND_TIMEOUT_S)
                sent_bytes += len(frame)

            total = 0
            try:
                if USE_STREAM_TTS:
                    log.info("开始流式 TTS...")

                    async def _send_pcm(chunk: bytes) -> None:
                        await forward_pcm_chunked(_send_one_frame, chunk)

                    try:
                        total = await stream_tts_pcm(reply, _send_pcm)
                    except Exception as stream_err:
                        log.warning("流式 TTS 失败，回退批量合成: %s", stream_err)
                        first = True
                        sent_bytes = 0
                        pcm = await asyncio.to_thread(synthesize_tts_pcm, reply)
                        log.info("批量 TTS 完成 %s bytes，开始下发", len(pcm))
                        total = await send_pcm_chunks(_send_one_frame, pcm)
                else:
                    log.info("开始批量 TTS...")
                    # TTS 合成也可能较慢：继续 heartbeat 到 play_start
                    stop_hb2 = asyncio.Event()

                    async def _tts_hb() -> None:
                        while not stop_hb2.is_set() and not self._closed:
                            try:
                                await self.conn.send(_json("processing"))
                            except Exception:
                                return
                            try:
                                await asyncio.wait_for(stop_hb2.wait(), timeout=PROCESSING_HEARTBEAT_S)
                                return
                            except asyncio.TimeoutError:
                                continue

                    tts_hb = asyncio.create_task(_tts_hb())
                    try:
                        pcm = await asyncio.to_thread(synthesize_tts_pcm, reply)
                    finally:
                        stop_hb2.set()
                        try:
                            await tts_hb
                        except Exception:
                            pass
                    log.info("批量 TTS 完成 %s bytes，开始限速下发 (chunk=%s speed=%s)",
                             len(pcm), PCM_CHUNK_BYTES, PCM_SEND_SPEED)
                    total = await send_pcm_chunks(_send_one_frame, pcm)
            finally:
                if not self._closed:
                    try:
                        if first:
                            await self.conn.send(_json("play_start"))
                        await self.conn.send(_json("play_stop"))
                        log.info("play_stop 已发送，共下发 PCM %s bytes", total)
                    except Exception as stop_err:
                        log.warning("发送 play_stop 失败: %s", stop_err)
                await asyncio.sleep(PLAY_STOP_SETTLE_MS / 1000.0)
                self.playing = False
                log.info("下行 TTS 结束（保持 WebSocket，等待下一轮）")
        except Exception as e:
            self.playing = False
            log.exception("处理句子失败: %s", e)
            try:
                if not self._closed:
                    await self.conn.send(_json("error", message=str(e)))
                    await self.conn.send(_json("play_stop"))
            except Exception:
                pass
        finally:
            with self._lock:
                self._busy = False
            # 不再每轮 close：设备复用连接，避免 RST / 二次建任务失败

    async def on_uplink_stop(self, reason: str) -> None:
        """uplink_stop 后保持连接，等 TTS 下行完成。"""
        self.uplink_enabled = False
        reason = reason if reason in self.VALID_STOP_REASONS else "manual"
        log.info("uplink_stop reason=%s（保持连接，等待下行）", reason)
        # 无论是否已 sentence_end，都停掉 ASR，杜绝空等超时
        self.stop_asr(reason=f"uplink_stop:{reason}")
        try:
            await self.conn.send(_json("uplink_ack", status="stopped", reason=reason))
        except Exception:
            pass

        if reason == "error":
            log.warning("设备上报 error，结束会话")
            await self._close_connection()
            return

        # 若还没出最终句，稍等一下（一般 sentence_end 已先到）
        if not self.got_final:
            await asyncio.sleep(0.5)

        if not self.got_final and self.last_partial:
            log.info("uplink_stop 后用最后 partial 收尾: %s", self.last_partial)
            await self.on_sentence(self.last_partial)
        else:
            for _ in range(300):
                if not self._busy and not self.playing:
                    break
                await asyncio.sleep(0.1)
            else:
                log.warning("等待下行超时（仍保持连接）")

        # 正常轮次保持连接，等待下一次 uplink_start

    async def _close_connection(self) -> None:
        if self._closed:
            return
        log.info("关闭 WebSocket")
        try:
            await self.conn.close()
        except Exception:
            pass
        self.close()

    def close(self) -> None:
        self._closed = True
        self.uplink_enabled = False
        self.playing = False
        self.stop_asr(reason="session_close")


# ---------------------------------------------------------------------------
# WebSocket 服务
# ---------------------------------------------------------------------------

async def handle_device(conn: ServerConnection) -> None:
    peer = getattr(conn, "remote_address", None)
    log.info("设备已连接: %s", peer)
    loop = asyncio.get_running_loop()
    session = DeviceSession(conn, loop)

    try:
        await conn.send(
            _json(
                "ready",
                rate=ASR_SAMPLE_RATE,
                format="s16le",
                channels=1,
                hint="uplink_start → PCM → uplink_stop → play_start/PCM/play_stop → 保持连接等待下一轮",
            )
        )

        async for message in conn:
            if isinstance(message, bytes):
                if not session.uplink_enabled:
                    continue
                # 勿在事件循环线程阻塞调用云端 ASR
                pcm = message
                await asyncio.to_thread(session.feed_pcm, pcm)
                continue

            try:
                msg = json.loads(message)
            except json.JSONDecodeError:
                log.warning("忽略非 JSON 文本: %s", str(message)[:120])
                continue

            op = msg.get("op")
            if op == "uplink_start":
                if session.playing or session._busy:
                    log.info("播放/处理中，忽略 uplink_start")
                    await conn.send(
                        _json("error", message="busy playing/processing, ignore uplink_start")
                    )
                    continue

                rate = int(msg.get("rate") or 0)
                fmt = msg.get("format") or ""
                channels = int(msg.get("channels") or 0)
                if rate != ASR_SAMPLE_RATE or fmt != "s16le" or channels != 1:
                    await conn.send(
                        _json(
                            "error",
                            message=(
                                f"仅支持 rate={ASR_SAMPLE_RATE}, format=s16le, channels=1，"
                                f"收到 rate={rate}, format={fmt}, channels={channels}"
                            ),
                        )
                    )
                    continue

                session.last_partial = ""
                session.got_final = False
                session.uplink_enabled = True
                session.start_asr()  # 每次上行单独开 ASR
                log.info("uplink_start")
                await conn.send(_json("uplink_ack", status="started"))

            elif op == "uplink_stop":
                reason = str(msg.get("reason") or "manual")
                if session._finalize_task and not session._finalize_task.done():
                    log.info("已在收尾中，忽略重复 uplink_stop")
                    continue
                session._finalize_task = asyncio.create_task(session.on_uplink_stop(reason))

            elif op == "ping":
                await conn.send(_json("pong", t=time.time()))

            else:
                log.info("未知 op: %s", op)
    except Exception as e:
        log.info("连接结束: %s", e)
    finally:
        session.close()
        log.info("设备已断开: %s", peer)


async def main() -> None:
    if not DASHSCOPE_API_KEY:
        raise SystemExit("请先设置环境变量 DASHSCOPE_API_KEY")

    log.info(
        "中转服务启动 ws://%s:%s/ws  协议: uplink↔play 循环复用同一连接",
        DEVICE_WS_HOST,
        DEVICE_WS_PORT,
    )
    async with serve(
        handle_device,
        DEVICE_WS_HOST,
        DEVICE_WS_PORT,
        # 设备端（尤其 ESP-Hosted）空闲时经常来不及回 ping，会被误判超时踢掉；
        # 然后二次 connect 出现 delayed connect abort。关闭服务端 keepalive ping。
        ping_interval=None,
        ping_timeout=None,
        max_size=2_000_000,
    ):
        await asyncio.Future()


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        print("\n已退出")
