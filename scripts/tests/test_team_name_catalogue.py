"""Round-trip the actual firmware preset and reject lossy/colliding sources."""

import copy
import hashlib
import importlib.util
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
SPEC = importlib.util.spec_from_file_location(
    "embed_team_names", ROOT / "scripts/embed_team_names.py")
ASSET = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(ASSET)
SOURCE = ROOT / "include/team_names.json"

HARNESS = r'''
#include "team_name_catalogue.h"
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>
namespace cat = football::names::catalogue;
struct Sink { std::size_t calls{}, fail_at{}; bool output{}; };
bool write(void* raw, const char* data, std::size_t length) {
  auto& sink = *static_cast<Sink*>(raw);
  if (!length || length > 128) std::abort();
  if (++sink.calls == sink.fail_at) return false;
  return !sink.output || std::fwrite(data, 1, length, stdout) == length;
}
int main(int argc, char** argv) {
  if (argc > 1 && std::string(argv[1]) == "metadata") {
    std::printf("%s %zu %zu\n", cat::sourceSha256(), cat::payloadBytes(), cat::count());
    return 0;
  }
  if (argc > 1 && std::string(argv[1]) == "queries") {
    std::string line;
    while (std::getline(std::cin, line)) {
      std::size_t match = cat::kNotFound;
      if (line.starts_with("key:")) match = cat::findKey(line.substr(4));
      else if (line.starts_with("name:")) match = cat::uniqueExact(line.substr(5));
      else {
        const auto split = line.find(':');
        match = cat::findId(line.substr(0, split), std::stoul(line.substr(split + 1)));
      }
      if (match == cat::kNotFound) std::puts("missing");
      else if (match == cat::kAmbiguous) std::puts("ambiguous");
      else std::cout << cat::at(match).key << '\n';
    }
    return 0;
  }
  if (argc > 1 && std::string(argv[1]) == "cancel") {
    Sink complete;
    if (!cat::streamJson(write, &complete)) return 1;
    for (std::size_t failure = 1; failure <= complete.calls; ++failure) {
      Sink stopped{0, failure, false};
      if (cat::streamJson(write, &stopped) || stopped.calls != failure) return 2;
    }
    if (cat::streamJson(nullptr, nullptr)) return 3;
    const auto invalid = cat::at(cat::count());
    if (!invalid.key.empty() || invalid.aliasCount() || !invalid.alias(0).empty()) return 4;
    return 0;
  }
  Sink output{0, 0, true};
  return cat::streamJson(write, &output) ? 0 : 1;
}
'''


class TeamNameCatalogue(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temporary = tempfile.TemporaryDirectory(prefix="football-name-preset-")
        cls.directory = Path(cls.temporary.name)
        cls.document = json.loads(SOURCE.read_text())
        cls.executable = cls.compile_source(SOURCE, cls.directory / "original")

    @classmethod
    def tearDownClass(cls):
        cls.temporary.cleanup()

    @staticmethod
    def compile_source(source, directory):
        ASSET.prepare_header(source, directory)
        harness = directory / "catalogue_test.cpp"
        harness.write_text(HARNESS)
        executable = directory / "catalogue_test"
        subprocess.run([
            shutil.which("c++") or "c++", "-std=c++20", "-Os", "-Wall",
            "-Wextra", "-Werror", "-fstack-usage",
            "-I" + str(ROOT / "include"),
            "-I" + str(ROOT / "components/json/include"),
            "-I" + str(ROOT / "components/names/include"),
            "-I" + str(directory / "team_name_asset"),
            str(ROOT / "components/names/team_name_catalogue.cpp"),
            str(ROOT / "components/json/json_stream.cpp"),
            "-I" + str(ROOT / "components/json/include"),
            str(harness), "-o", str(executable),
        ], check=True, capture_output=True, text=True)
        return executable

    def test_full_source_round_trip_and_fingerprint(self):
        actual = subprocess.check_output([self.executable])
        self.assertEqual(json.loads(actual), self.document)
        sha, payload, count = subprocess.check_output(
            [self.executable, "metadata"], text=True).split()
        self.assertEqual(sha, hashlib.sha256(SOURCE.read_bytes()).hexdigest())
        self.assertEqual(int(payload), 12767)
        self.assertEqual(int(count), 147)
        self.assertEqual(len(actual), 26645)

    def test_every_source_id_key_and_exact_name_matches(self):
        queries, expected, names = [], [], {}
        for club in self.document["clubs"]:
            queries.append("key:" + club["key"])
            expected.append(club["key"])
            for provider, identities in club["ids"].items():
                for identity in identities:
                    queries.append(provider + ":" + str(identity))
                    expected.append(club["key"])
            for text in [club["de"], club["en"], *club["aliases"]]:
                names.setdefault(text, set()).add(club["key"])
        for text, keys in names.items():
            queries.append("name:" + text)
            expected.append(next(iter(keys)) if len(keys) == 1 else "ambiguous")
        queries += ["espn:0", "unknown:131", "key:unknown-club", "name:bayern", "name:"]
        expected += ["missing"] * 5
        result = subprocess.run([self.executable, "queries"],
                                input="\n".join(queries) + "\n", capture_output=True,
                                text=True, check=True)
        self.assertEqual(result.stdout.splitlines(), expected)

    def test_callback_failure_cancels_at_every_output_chunk(self):
        subprocess.run([self.executable, "cancel"], check=True)

    def test_generator_is_deterministic_and_does_not_rewrite_unchanged_output(self):
        directory = self.directory / "reproducible"
        path, changed, size = ASSET.prepare_header(SOURCE, directory)
        self.assertTrue(changed)
        self.assertEqual(size, 12767)
        before = path.stat().st_mtime_ns
        _, changed, repeated_size = ASSET.prepare_header(SOURCE, directory)
        self.assertFalse(changed)
        self.assertEqual(path.stat().st_mtime_ns, before)
        self.assertEqual(size, repeated_size)
        other, _, _ = ASSET.prepare_header(SOURCE, self.directory / "independent")
        self.assertEqual(path.read_bytes(), other.read_bytes())

    def test_escape_full_values_and_ambiguous_exact_name(self):
        document = copy.deepcopy(self.document)
        document["clubs"][0]["de"] = 'Quote " slash \\ newline\n tab\t Ü 😀'
        document["clubs"][0]["aliases"].append("shared exact name")
        document["clubs"][1]["aliases"].append("shared exact name")
        # Preserve absent-versus-empty provider fields as well.
        del document["clubs"][0]["ids"]["espn"]
        document["clubs"][1]["ids"]["openligadb"] = []
        path = self.directory / "escaped.json"
        path.write_text(json.dumps(document, ensure_ascii=False))
        executable = self.compile_source(path, self.directory / "escaped")
        self.assertEqual(json.loads(subprocess.check_output([executable])), document)
        result = subprocess.run([executable, "queries"], input="name:shared exact name\n",
                                capture_output=True, text=True, check=True)
        self.assertEqual(result.stdout, "ambiguous\n")

    def test_reject_key_id_overflow_and_unrepresented_field(self):
        mutations = [
            ("key collision", lambda d: d["clubs"][1].update(key=d["clubs"][0]["key"])),
            ("ID collision", lambda d: d["clubs"][1]["ids"]["espn"].append(
                d["clubs"][0]["ids"]["espn"][0])),
            ("ID overflow", lambda d: d["clubs"][0]["ids"]["espn"].append(1 << 32)),
            ("pool overflow", lambda d: d["clubs"][0].update(de="x" * 65536)),
            ("count overflow", lambda d: d["clubs"][0].update(aliases=["x"] * 256)),
            ("unknown field", lambda d: d["clubs"][0].update(unrepresented="lost")),
            ("unknown provider", lambda d: d["clubs"][0]["ids"].update(unknown=[1])),
            ("embedded NUL", lambda d: d["clubs"][0].update(de="a\0b")),
        ]
        for description, mutate in mutations:
            with self.subTest(description):
                document = copy.deepcopy(self.document)
                mutate(document)
                path = self.directory / "rejected.json"
                path.write_text(json.dumps(document))
                with self.assertRaises(ValueError):
                    ASSET.render_header(path)


if __name__ == "__main__":
    unittest.main()
