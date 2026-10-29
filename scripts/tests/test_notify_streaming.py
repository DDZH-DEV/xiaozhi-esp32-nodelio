"""Run the real MP3/M4A producer loops with deterministic HTTP/codec substitutes.

This checks scheduling, buffering, cancellation and EOF, not codec fidelity.
"""
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


def function(source, signature):
    start = source.index(signature)
    brace = source.index("{", start)
    depth, end = 1, brace + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


class NotifyStreamingTests(unittest.TestCase):
    def test_streaming_start_tail_cancellation_and_stalled_container(self):
        compiler = shlex.split(os.environ.get("CXX", ""))
        if not compiler:
            compiler = [shutil.which("c++") or shutil.which("clang++") or ""]
        if not compiler[0]:
            self.skipTest("requires a host C++ compiler (or CXX)")
        source = (ROOT / "main/notify/notify_player.cc").read_text(encoding="utf-8")
        constants = source[source.index("constexpr size_t kPrefetchBytes"):
                           source.index('const char* TAG')]
        methods = "\n".join(function(source, signature) for signature in (
            "bool NotifyPlayer::PlayMp3(", "bool NotifyPlayer::PlayM4a(",
            "bool NotifyPlayer::HasPlaybackStarted()", "bool NotifyPlayer::IsCancelled(",
            "void NotifyPlayer::OnPlaybackProgress(", "void NotifyPlayer::Stop()",
        ))
        support = (Path(__file__).with_name("notify_streaming_host.cc")).read_text(encoding="utf-8")
        driver = support.replace("// INSERT_CONSTANTS", constants).replace("// INSERT_METHODS", methods)
        with tempfile.TemporaryDirectory() as directory:
            build_dir = Path(directory)
            cpp = build_dir / "notify_streaming.cc"
            cpp.write_text(driver, encoding="utf-8")
            executable = build_dir / ("notify_streaming.exe" if os.name == "nt" else "notify_streaming")
            subprocess.run(compiler + ["-std=c++20", str(cpp), "-o", str(executable)],
                           check=True, cwd=build_dir, timeout=120)
            subprocess.run([str(executable)], check=True, cwd=build_dir, timeout=15)


if __name__ == "__main__":
    unittest.main()
