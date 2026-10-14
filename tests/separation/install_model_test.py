"""Tests for scripts/separation-model/install_model.py (issue #112).

Runs on the host's python3 with the standard library only: download goes through file:// URLs,
and export / read_facts are fakes, so no test touches the network, torch, onnx or the real model
(ADR 0005).
"""

import hashlib
import io
import re
import subprocess
import sys
import tempfile
import unittest
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
SCRIPT_DIR = REPO_ROOT / "scripts" / "separation-model"
sys.path.insert(0, str(SCRIPT_DIR))

import install_model as im  # noqa: E402

VALID_METADATA = {
    "architecture": "bs_roformer",
    "sources": '["bass", "drums", "other", "vocals", "guitar", "piano"]',
    "segment_samples": "588800",
    "sample_rate": "44100",
    "stft_n_fft": "2048",
    "stft_hop_length": "512",
    "stft_win_length": "2048",
    "stft_normalized": "false",
    "stft_window": "hann",
}
VALID_SHAPES = {"spec_real": [1, 2, 1025, 1151], "spec_imag": [1, 2, 1025, 1151]}
EXPORTED_BYTES = b"fake onnx graph"


def valid_facts():
    return im.ModelFacts(metadata=dict(VALID_METADATA), opset=18, input_shapes=dict(VALID_SHAPES))


class FakeExporter:
    """Writes EXPORTED_BYTES; read_facts returns `facts` for any file it wrote."""

    def __init__(self, facts=None, fail=False):
        self.facts = facts or valid_facts()
        self.fail = fail
        self.calls = 0

    def export(self, cache_dir, output_path):
        self.calls += 1
        if self.fail:
            output_path.write_bytes(b"half a graph")
            raise RuntimeError("tracing blew up")
        output_path.write_bytes(EXPORTED_BYTES)

    def read_facts(self, path):
        if path.read_bytes() != EXPORTED_BYTES:
            raise ValueError("not an ONNX file")
        return self.facts


class InstallTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        root = Path(self.temp.name)
        self.models_dir = root / "models"
        self.cache_dir = root / "cache"
        self.source = root / "remote.safetensors"
        self.source.write_bytes(b"checkpoint weights")
        self.checkpoint = im.Checkpoint(url=self.source.as_uri(), sha256=hashlib.sha256(b"checkpoint weights").hexdigest())
        self.final_path = self.models_dir / im.MODEL_FILE_NAME

    def tearDown(self):
        self.temp.cleanup()

    def run_install(self, exporter, checkpoint=None, download=im.download_url):
        with redirect_stdout(io.StringIO()):
            return im.install(self.models_dir, self.cache_dir, checkpoint or self.checkpoint, download, exporter.export, exporter.read_facts)

    def assert_failed_at(self, step, exporter, **kwargs):
        with self.assertRaises(im.InstallError) as raised:
            self.run_install(exporter, **kwargs)
        self.assertEqual(raised.exception.step, step)
        self.assertFalse(self.final_path.exists(), "a failed install must leave no model file")
        leftovers = [p.name for p in self.models_dir.iterdir()] if self.models_dir.exists() else []
        self.assertEqual(leftovers, [], "a failed install must leave no temp files in the models folder")
        return raised.exception

    def test_pinned_checkpoint_is_unblends_revision_and_hash(self):
        self.assertIn("/resolve/9ba75778acf1b64fc40a9ea05137a2c6af9cd1d5/bs_roformer_sw.safetensors", im.PINNED_CHECKPOINT.url)
        self.assertEqual(im.PINNED_CHECKPOINT.sha256, "56c131428745d83d63f72293d237fdbb56f40d8e4b6980cbcde70e28a9f7330f")
        self.assertEqual(im.OPSET_VERSION, 18)

    def test_installs_the_exported_model_and_caches_the_checkpoint_under_unblends_name(self):
        exporter = FakeExporter()
        self.assertEqual(self.run_install(exporter), "installed")
        self.assertEqual(self.final_path.read_bytes(), EXPORTED_BYTES)
        self.assertEqual(sorted(p.name for p in self.models_dir.iterdir()), [im.MODEL_FILE_NAME])
        cached = self.cache_dir / f"{self.checkpoint.sha256[:16]}.safetensors"
        self.assertEqual(cached.read_bytes(), b"checkpoint weights")
        self.assertEqual(sorted(p.name for p in self.cache_dir.iterdir()), [cached.name])

    def test_hash_mismatch_refuses_before_export(self):
        exporter = FakeExporter()
        wrong = im.Checkpoint(url=self.checkpoint.url, sha256="0" * 64)
        error = self.assert_failed_at("verify", exporter, checkpoint=wrong)
        self.assertIn("0" * 64, error.message)
        self.assertEqual(exporter.calls, 0)
        self.assertEqual(list(self.cache_dir.iterdir()), [], "a mismatching download must not stay in the cache")

    def test_failed_download_names_the_step(self):
        missing = im.Checkpoint(url=(Path(self.temp.name) / "absent.safetensors").as_uri(), sha256=self.checkpoint.sha256)
        exporter = FakeExporter()
        self.assert_failed_at("download", exporter, checkpoint=missing)
        self.assertEqual(exporter.calls, 0)
        self.assertEqual(list(self.cache_dir.iterdir()), [])

    def test_failed_export_names_the_step(self):
        self.assert_failed_at("export", FakeExporter(fail=True))

    def test_export_with_external_data_is_refused(self):
        class ExternalDataExporter(FakeExporter):
            def export(self, cache_dir, output_path):
                super().export(cache_dir, output_path)
                (output_path.parent / "weights.data").write_bytes(b"x")

        self.assert_failed_at("export", ExternalDataExporter())

    def test_wrong_metadata_is_refused_before_install(self):
        facts = valid_facts()
        facts.metadata["sources"] = '["vocals", "other"]'
        del facts.metadata["stft_hop_length"]
        error = self.assert_failed_at("check", FakeExporter(facts=facts))
        self.assertIn("'sources'", error.message)
        self.assertIn("'stft_hop_length' is missing", error.message)

    def test_rerun_with_a_valid_model_does_nothing(self):
        self.run_install(FakeExporter())
        self.source.unlink()  # a second download would now fail
        exporter = FakeExporter()
        out = io.StringIO()
        with redirect_stdout(out):
            result = im.install(self.models_dir, self.cache_dir, self.checkpoint, im.download_url, exporter.export, exporter.read_facts)
        self.assertEqual(result, "already_installed")
        self.assertEqual(exporter.calls, 0)
        self.assertIn("already installed", out.getvalue())

    def test_rerun_reuses_a_verified_cached_checkpoint(self):
        self.run_install(FakeExporter())
        self.final_path.unlink()
        self.source.unlink()
        self.assertEqual(self.run_install(FakeExporter()), "installed")

    def test_invalid_existing_model_is_replaced(self):
        self.models_dir.mkdir(parents=True)
        self.final_path.write_bytes(b"something else")
        self.assertEqual(self.run_install(FakeExporter()), "installed")
        self.assertEqual(self.final_path.read_bytes(), EXPORTED_BYTES)

    def test_main_reports_the_failed_step_and_exits_nonzero(self):
        err = io.StringIO()
        original = im.PINNED_CHECKPOINT
        im.PINNED_CHECKPOINT = im.Checkpoint(url=(Path(self.temp.name) / "absent").as_uri(), sha256=original.sha256)
        try:
            with redirect_stdout(io.StringIO()), redirect_stderr(err):
                code = im.main(["--models-dir", str(self.models_dir), "--cache-dir", str(self.cache_dir)])
        finally:
            im.PINNED_CHECKPOINT = original
        self.assertEqual(code, 1)
        self.assertIn("[download] FAILED", err.getvalue())


class ModelProblemsTest(unittest.TestCase):
    def test_the_expected_model_has_no_problems(self):
        self.assertEqual(im.model_problems(valid_facts()), [])

    def test_each_required_key_and_value_is_checked(self):
        cases = {
            "architecture": "mel_band_roformer",
            "segment_samples": "409600",
            "sample_rate": "48000",
            "stft_n_fft": "4096",
            "stft_hop_length": "1024",
            "stft_win_length": "4096",
            "stft_normalized": "true",
            "sources": '["bass", "drums", "other", "vocals"]',
        }
        for key, wrong in cases.items():
            with self.subTest(key=key):
                facts = valid_facts()
                facts.metadata[key] = wrong
                problems = im.model_problems(facts)
                self.assertEqual(len(problems), 1, problems)
                self.assertIn(f"'{key}'", problems[0])

    def test_opset_and_static_batch_are_checked(self):
        facts = im.ModelFacts(metadata=dict(VALID_METADATA), opset=17, input_shapes={"spec_real": ["batch", 2, 1025, 1151], "spec_imag": [1, 2, 1025, 1151]})
        problems = im.model_problems(facts)
        self.assertEqual(len(problems), 2, problems)


class PinsAndRepoTest(unittest.TestCase):
    def test_unblend_torch_and_onnx_are_pinned(self):
        requirements = (SCRIPT_DIR / "requirements.txt").read_text()
        for package in ("unblend", "torch", "onnx", "onnxscript"):
            with self.subTest(package=package):
                self.assertRegex(requirements, rf"(?m)^{re.escape(package)}(\[[a-z,]+\])?==\d+(\.\d+)*")

    def test_no_model_file_is_committed(self):
        files = subprocess.run(["git", "-C", str(REPO_ROOT), "ls-files"], check=True, capture_output=True, text=True).stdout.split("\n")
        weights = [f for f in files if f.endswith((".onnx", ".safetensors", ".ckpt", ".pth"))]
        self.assertEqual(weights, [])

    def test_model_files_are_ignored(self):
        for path in ("models/bs_roformer_sw.fp32.onnx", "bs_roformer_sw.safetensors"):
            with self.subTest(path=path):
                result = subprocess.run(["git", "-C", str(REPO_ROOT), "check-ignore", "-q", path])
                self.assertEqual(result.returncode, 0, f"{path} is not git-ignored")

    def test_install_script_defaults_are_outside_the_repo(self):
        script = (SCRIPT_DIR / "install.sh").read_text()
        self.assertIn('"$HOME/Library/Application Support/Overdub/models"', script)
        self.assertIn('"$HOME/Library/Caches/Overdub/separation-model"', script)


if __name__ == "__main__":
    unittest.main()
