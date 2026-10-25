import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]


class GoogleOAuthBuildTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix="sbo-oauth-build-")
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        self.header = self.root / "generated/GoogleOAuthCredentials.hpp"
        self.environment = dict(os.environ)
        for key in ("GOOGLE_OAUTH_CLIENT_ID", "GOOGLE_OAUTH_CLIENT_SECRET", "GOOGLE_OAUTH_CLIENT_FILE", "GOOGLE_OAUTH_REQUIRED"):
            self.environment.pop(key, None)

    def configure(self, **values):
        result = subprocess.run(
            ["cmake", f"-DGOOGLE_OAUTH_HEADER={self.header}", "-P", str(ROOT / "cmake/GoogleOAuth.cmake")],
            env=dict(self.environment, **values), text=True, capture_output=True,
        )
        return result

    def test_release_requires_app_credentials(self):
        result = self.configure(GOOGLE_OAUTH_REQUIRED="ON")
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(self.header.exists())

    def test_development_build_can_be_unconfigured(self):
        result = self.configure()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue(self.header.exists())

    def test_partial_app_credentials_are_rejected(self):
        for key in ("GOOGLE_OAUTH_CLIENT_ID", "GOOGLE_OAUTH_CLIENT_SECRET"):
            with self.subTest(key=key):
                result = self.configure(**{key: "one-part-only"})
                self.assertNotEqual(result.returncode, 0)
                self.assertNotIn("one-part-only", result.stdout + result.stderr)

    def test_environment_credentials_round_trip_as_cpp_strings(self):
        client_id = "test-client.apps.googleusercontent.com"
        secret = 'test-"quoted"\\secret;with@characters'
        result = self.configure(GOOGLE_OAUTH_REQUIRED="ON", GOOGLE_OAUTH_CLIENT_ID=client_id, GOOGLE_OAUTH_CLIENT_SECRET=secret)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertNotIn(secret, result.stdout + result.stderr)
        program = self.root / "check.cpp"
        program.write_text(
            '#include "GoogleOAuthCredentials.hpp"\n#include <string>\nint main() {\n'
            f'    return std::string(drive::build::ClientId) == {json.dumps(client_id)} &&\n'
            f'        std::string(drive::build::ClientSecret) == {json.dumps(secret)} ? 0 : 1;\n}}\n'
        )
        subprocess.run(["c++", "-std=c++23", f"-I{self.header.parent}", str(program), "-o", str(self.root / "check")], check=True, capture_output=True)
        subprocess.run([str(self.root / "check")], check=True)

    def test_client_json_embeds_only_app_credentials(self):
        for section in ("installed", "oauth", None):
            with self.subTest(section=section):
                client = {"client_id": "file-client", "client_secret": "file-secret"}
                document = {section: client} if section else client.copy()
                document["connections"] = {"global": {"refresh_token": "never-embed-this-personal-token"}}
                path = self.root / "client.json"
                path.write_text(json.dumps(document))
                result = self.configure(GOOGLE_OAUTH_CLIENT_FILE=str(path), GOOGLE_OAUTH_REQUIRED="ON")
                self.assertEqual(result.returncode, 0, result.stderr)
                generated = self.header.read_text()
                self.assertIn("file-client", generated)
                self.assertIn("file-secret", generated)
                self.assertNotIn("never-embed-this-personal-token", generated)

    def test_invalid_client_json_and_mixed_sources_are_rejected(self):
        path = self.root / "client.json"
        for contents in ("{invalid", '{"installed":null}', '{"client_id":123,"client_secret":"secret"}'):
            path.write_text(contents)
            result = self.configure(GOOGLE_OAUTH_CLIENT_FILE=str(path))
            self.assertNotEqual(result.returncode, 0)
        path.write_text('{"client_id":"id","client_secret":"secret"}')
        result = self.configure(GOOGLE_OAUTH_CLIENT_FILE=str(path), GOOGLE_OAUTH_CLIENT_ID="other-id", GOOGLE_OAUTH_CLIENT_SECRET="other-secret")
        self.assertNotEqual(result.returncode, 0)


if __name__ == "__main__":
    unittest.main()
