import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]


class VersionTests(unittest.TestCase):
    def read_version(self, source):
        with tempfile.TemporaryDirectory(prefix="sbo-version-") as directory:
            root = Path(directory)
            (root / "cmake").mkdir()
            (root / "src").mkdir()
            shutil.copyfile(ROOT / "cmake/Version.cmake", root / "cmake/Version.cmake")
            (root / "src/main.cpp").write_text(source)
            output = root / "version.txt"

            result = subprocess.run(["cmake", f"-DVERSION_OUTPUT={output}", "-P", str(root / "cmake/Version.cmake")], text=True, capture_output=True)
            return result, output.read_text() if output.exists() else ""

    def test_version_and_tag_use_main_cpp(self):
        for version in ("0.1.0", "1.2.3", "1234.5678.90123"):
            with self.subTest(version=version):
                result, output = self.read_version(f'static constexpr const char* APP_VERSION = "{version}";\n')
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(output, f"version={version}\ntag=v{version}\n")

    def test_invalid_versions_fail(self):
        for version in ("", "1.2", "v1.2.3", "01.2.3", "1.2.3-beta", "12345.12345.12345"):
            with self.subTest(version=version):
                result, output = self.read_version(f'static constexpr const char* APP_VERSION = "{version}";\n')
                self.assertNotEqual(result.returncode, 0)
                self.assertEqual(output, "")

    def test_missing_or_duplicate_declarations_fail(self):
        for source in ("", 'static constexpr const char* APP_VERSION = "0.1.0";\n' * 2):
            result, _ = self.read_version(source)
            self.assertNotEqual(result.returncode, 0)


class ReleaseTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix="sbo-release-")
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        self.git("init", "--initial-branch=main")
        self.git("commit", "--allow-empty", "-m", "Initial test commit")
        self.commit = self.git("rev-parse", "HEAD").stdout.strip()
        self.log = self.root / "gh.jsonl"
        self.bin = self.root / "bin"
        self.bin.mkdir()

        fake_gh = self.bin / "gh"
        fake_gh.write_text('''#!/usr/bin/env python3
import json
import os
from pathlib import Path
import sys

arguments = sys.argv[1:]
with Path(os.environ["FAKE_GH_LOG"]).open("a") as log:
    log.write(json.dumps(arguments) + "\\n")

command = arguments[1]
if command == os.environ.get("FAKE_GH_FAIL"):
    sys.exit(1)

if command == "view":
    state = os.environ.get("FAKE_GH_STATE", "absent")
    if state == "absent":
        sys.exit(1)
    print(("true" if state == "draft" else "false") + "\\t" + os.environ["FAKE_GH_TARGET"])
''')
        fake_gh.chmod(0o700)

        self.artifacts = self.root / "dist"
        self.artifacts.mkdir()
        data = b"Test overlay bytes"
        (self.artifacts / "SavegameBackupOverlay.ovl").write_bytes(data)
        (self.artifacts / "SavegameBackupOverlay.ovl.sha256").write_text(hashlib.sha256(data).hexdigest() + "  SavegameBackupOverlay.ovl\n")

    def git(self, *arguments):
        command = ["git", "-c", "user.name=Release Test", "-c", "user.email=release@example.test", "-c", "commit.gpgsign=false", "-c", "tag.gpgsign=false", *arguments]
        return subprocess.run(command, cwd=self.root, text=True, capture_output=True, check=True)

    def publish(self, state="absent", target=None, fail="", version="1.2.3"):
        environment = dict(os.environ, PATH=f"{self.bin}:{os.environ['PATH']}", GITHUB_SHA=self.commit, GITHUB_REPOSITORY="example/test", GH_TOKEN="test-token", FAKE_GH_LOG=str(self.log), FAKE_GH_STATE=state, FAKE_GH_TARGET=target or self.commit, FAKE_GH_FAIL=fail)
        result = subprocess.run(["bash", str(ROOT / ".github/scripts/publish_release.sh"), version, str(self.artifacts)], cwd=self.root, env=environment, text=True, capture_output=True)
        calls = [json.loads(line) for line in self.log.read_text().splitlines()] if self.log.exists() else []
        return result, calls

    def test_new_release_uses_exact_commit_and_both_assets(self):
        result, calls = self.publish()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual([call[1] for call in calls], ["view", "create"])
        create = calls[-1]
        self.assertEqual(create[2], "v1.2.3")
        self.assertEqual(create[create.index("--target") + 1], self.commit)
        self.assertIn(str(self.artifacts / "SavegameBackupOverlay.ovl"), create)
        self.assertIn(str(self.artifacts / "SavegameBackupOverlay.ovl.sha256"), create)

    def test_published_release_is_skipped(self):
        result, calls = self.publish(state="published", target="old-commit")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual([call[1] for call in calls], ["view"])

    def test_partial_draft_is_finished(self):
        result, calls = self.publish(state="draft")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual([call[1] for call in calls], ["view", "upload", "edit"])
        self.assertIn("--draft=false", calls[-1])

    def test_upload_failure_does_not_publish_draft(self):
        result, calls = self.publish(state="draft", fail="upload")
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual([call[1] for call in calls], ["view", "upload"])

    def test_draft_from_other_commit_is_rejected(self):
        result, calls = self.publish(state="draft", target="other-commit")
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual([call[1] for call in calls], ["view"])

    def test_existing_tag_on_same_commit_can_be_released(self):
        self.git("tag", "v1.2.3")
        result, calls = self.publish()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(calls[-1][1], "create")

    def test_tag_is_never_moved_to_another_commit(self):
        self.git("tag", "v1.2.3")
        self.git("commit", "--allow-empty", "-m", "Next test commit")
        self.commit = self.git("rev-parse", "HEAD").stdout.strip()
        result, calls = self.publish()
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual([call[1] for call in calls], ["view"])

    def test_bad_checksum_prevents_release(self):
        (self.artifacts / "SavegameBackupOverlay.ovl").write_bytes(b"Changed bytes")
        result, calls = self.publish()
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(calls, [])

    def test_missing_overlay_prevents_release(self):
        (self.artifacts / "SavegameBackupOverlay.ovl").unlink()
        result, calls = self.publish()
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(calls, [])

    def test_invalid_release_version_is_rejected(self):
        result, calls = self.publish(version="01.2.3")
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(calls, [])


if __name__ == "__main__":
    unittest.main()
