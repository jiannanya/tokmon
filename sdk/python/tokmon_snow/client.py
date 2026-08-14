from __future__ import annotations

import asyncio
import contextlib
import json
import os
from pathlib import Path
from typing import Any, AsyncIterator, Callable


class SnowError(RuntimeError):
    def __init__(self, code: str, message: str, data: Any = None):
        super().__init__(message)
        self.code = code
        self.data = data


class SnowSession:
    def __init__(self, client: "SnowClient", session_id: str):
        self.client = client
        self.id = session_id

    async def turn(self, message: str, *, model: str = "", max_steps: int = 32,
                   model_parameters: dict[str, Any] | None = None,
                   attachments: list[dict[str, Any]] | None = None) -> dict[str, Any]:
        return await self.client.request("turn.start", {
            "session_id": self.id, "message": message, "model": model,
            "max_steps": max_steps, "model_parameters": model_parameters or {},
            "attachments": attachments or []
        })

    async def cancel(self) -> bool:
        value = await self.client.request("turn.cancel", {"session_id": self.id})
        return bool(value["accepted"])

    async def steer(self, message: str) -> int:
        value = await self.client.request("turn.steer", {
            "session_id": self.id, "message": message
        })
        return int(value["seq"])

    async def events(self, after: int = 0) -> list[dict[str, Any]]:
        return await self.client.request("session.events", {
            "session_id": self.id, "after": after
        })

    async def transcript(self) -> list[dict[str, Any]]:
        return await self.client.request("session.transcript", {"session_id": self.id})

    async def replay(self, level: str = "R2") -> dict[str, Any]:
        return await self.client.request("session.replay", {
            "session_id": self.id, "level": level
        })

    async def fork(self, metadata: dict[str, Any] | None = None,
                   seed_seq: int = 0) -> "SnowSession":
        value = await self.client.request("session.fork", {
            "session_id": self.id, "metadata": metadata or {}, "seed_seq": seed_seq
        })
        return SnowSession(self.client, value["session_id"])

    async def close(self) -> None:
        await self.client.request("session.close", {"session_id": self.id})


class SnowClient:
    FRAME_LIMIT = 8 * 1024 * 1024

    def __init__(self, process: asyncio.subprocess.Process, *, timeout: float):
        self.process = process
        self.timeout = timeout
        self.capabilities: tuple[str, ...] = ()
        self._next_id = 1
        self._pending: dict[int, asyncio.Future[Any]] = {}
        self._notifications: asyncio.Queue[dict[str, Any]] = asyncio.Queue(maxsize=256)
        self._reader = asyncio.create_task(self._read_stdout())
        self._stderr = asyncio.create_task(self._drain_stderr())

    @classmethod
    async def connect(cls, *, executable: str | os.PathLike[str] | None = None,
                      workspace: str | os.PathLike[str] | None = None,
                      data_root: str | os.PathLike[str] | None = None,
                      config_dir_name: str = ".snow", raw_trace: bool = False,
                      request_timeout: float = 300.0) -> "SnowClient":
        if (not config_dir_name or config_dir_name in (".", "..") or
                any(c not in "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-"
                    for c in config_dir_name)):
            raise ValueError("config_dir_name must be one safe directory name")
        executable = os.fspath(executable or ("snow.exe" if os.name == "nt" else "snow"))
        workspace = os.fspath(Path(workspace or os.getcwd()).resolve())
        arguments = [executable, "serve", "--workspace", workspace,
                     "--config-dir-name", config_dir_name]
        if data_root is not None:
            arguments += ["--data-root", os.fspath(Path(data_root).resolve())]
        if raw_trace:
            arguments.append("--raw-trace")
        process = await asyncio.create_subprocess_exec(
            *arguments, stdin=asyncio.subprocess.PIPE,
            stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.PIPE)
        client = cls(process, timeout=request_timeout)
        initialized = await client.request("initialize", {
            "protocol_min": 1, "protocol_max": 1,
            "client": {"name": "tokmon-snow", "version": "1.0.0"}
        })
        if initialized["selected_protocol"] != 1:
            await client.close()
            raise SnowError("protocol.incompatible", "Snow protocol negotiation failed")
        client.capabilities = tuple(initialized["capabilities"])
        return client

    async def _read_stdout(self) -> None:
        assert self.process.stdout is not None
        while line := await self.process.stdout.readline():
            if len(line) > self.FRAME_LIMIT:
                continue
            try:
                message = json.loads(line)
            except (UnicodeDecodeError, json.JSONDecodeError):
                continue
            request_id = message.get("id")
            if request_id is not None:
                future = self._pending.pop(request_id, None)
                if future is None or future.done():
                    continue
                if "error" in message:
                    value = message["error"]
                    future.set_exception(SnowError(
                        value.get("code", "snow.protocol"),
                        value.get("message", "Snow request failed"), value.get("data")))
                else:
                    future.set_result(message.get("result"))
            else:
                with contextlib.suppress(asyncio.QueueFull):
                    self._notifications.put_nowait(message)
        error = SnowError("snow.disconnected", "Snow process exited")
        for future in self._pending.values():
            if not future.done():
                future.set_exception(error)
        self._pending.clear()

    async def _drain_stderr(self) -> None:
        assert self.process.stderr is not None
        while await self.process.stderr.readline():
            pass

    async def request(self, method: str, params: dict[str, Any] | None = None,
                      *, timeout: float | None = None) -> Any:
        if len(self._pending) >= 64:
            raise SnowError("snow.backpressure", "too many in-flight requests")
        if self.process.returncode is not None:
            raise SnowError("snow.disconnected", "Snow process is not running")
        request_id = self._next_id
        self._next_id += 1
        loop = asyncio.get_running_loop()
        future = loop.create_future()
        self._pending[request_id] = future
        frame = (json.dumps({"jsonrpc": "2.0", "id": request_id,
                             "method": method, "params": params or {}},
                            ensure_ascii=False, separators=(",", ":")) + "\n").encode()
        if len(frame) > self.FRAME_LIMIT:
            self._pending.pop(request_id, None)
            raise SnowError("snow.frame_limit", "request exceeds 8 MiB")
        assert self.process.stdin is not None
        self.process.stdin.write(frame)
        await self.process.stdin.drain()
        try:
            return await asyncio.wait_for(future, timeout or self.timeout)
        finally:
            self._pending.pop(request_id, None)

    async def create_session(self, metadata: dict[str, Any] | None = None) -> SnowSession:
        value = await self.request("session.create", {"metadata": metadata or {}})
        return SnowSession(self, value["session_id"])

    async def resume_session(self, session_id: str, after: int = 0) -> SnowSession:
        await self.request("session.resume", {"session_id": session_id, "after": after})
        return SnowSession(self, session_id)

    async def list_sessions(self, limit: int = 100) -> list[dict[str, Any]]:
        return await self.request("session.list", {"limit": limit})

    async def respond_approval(self, approval_id: str, approved: bool) -> bool:
        value = await self.request("approval.respond", {
            "approval_id": approval_id, "approved": approved
        })
        return bool(value["resolved"])

    async def read_artifact(self, reference: dict[str, Any], *, offset: int = 0,
                            limit: int = 256 * 1024) -> dict[str, Any]:
        return await self.request("artifact.read", {
            **reference, "offset": offset, "limit": limit
        })

    async def notifications(self) -> AsyncIterator[dict[str, Any]]:
        while True:
            yield await self._notifications.get()

    async def close(self) -> None:
        if self.process.returncode is None:
            if self.process.stdin is not None:
                self.process.stdin.close()
                with contextlib.suppress(Exception):
                    await self.process.stdin.wait_closed()
            try:
                await asyncio.wait_for(self.process.wait(), 2.0)
            except asyncio.TimeoutError:
                self.process.terminate()
                await self.process.wait()
        for task in (self._reader, self._stderr):
            if not task.done():
                task.cancel()
            with contextlib.suppress(asyncio.CancelledError):
                await task

    async def __aenter__(self) -> "SnowClient":
        return self

    async def __aexit__(self, *_: object) -> None:
        await self.close()
