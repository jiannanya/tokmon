import asyncio
import os
import shutil
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parents[1] / "python"))
from tokmon_snow import SnowClient


async def main() -> None:
    workspace = Path(tempfile.mkdtemp(prefix="snow-python-"))
    client = await SnowClient.connect(
        executable=os.environ["SNOW_EXE"], workspace=workspace,
        config_dir_name=".sdk-test", data_root=workspace / "data",
        request_timeout=10)
    try:
        session = await client.create_session({"fixture": "python"})
        turn = await session.turn("hello")
        assert turn["reason"] == "completed"
        events = await session.events()
        types = {event["type"] for event in events}
        assert {"session/header", "request/header", "assistant/message", "turn/end"} <= types
        replay = await session.replay("R2")
        assert replay["control"]
    finally:
        await client.close()
        shutil.rmtree(workspace, ignore_errors=True)
    print("python_golden: ok")


asyncio.run(main())
