#!/usr/bin/env python3
"""Real recorded-season oracle through the actual parser/planner/scheduler."""
from pathlib import Path
import datetime
import json
import subprocess
import tempfile
import shutil

ROOT = Path(__file__).resolve().parents[2]
season = ROOT / "aidocs/Reviews/evidence_2026-10-17/provider-public/openliga-season.json"
rows = json.loads(season.read_text())
now = datetime.datetime(2026, 10, 14, 12, tzinfo=datetime.timezone.utc)
def kickoff(row):
    return datetime.datetime.fromisoformat(row["matchDateTimeUTC"].replace("Z", "+00:00"))
clubs = sorted({row[side]["teamId"] for row in rows for side in ("team1", "team2")})[:8]
rounds = {}
for row in rows:
    rounds.setdefault(row["group"]["groupOrderID"], []).append(row)
past = [key for key, games in rounds.items() if max(kickoff(row).date() for row in games) + datetime.timedelta(days=2) <= now.date()]
future = sorted(set(rounds) - set(past), key=lambda key: min(kickoff(row) for row in rounds[key]))
retained_rounds = ([max(past, key=lambda key: min(kickoff(row) for row in rounds[key]))] if past else []) + future[:2]
expected = {row["matchID"] for row in rows if row["group"]["groupOrderID"] in retained_rounds}
for club in clubs:
    games = sorted((row for row in rows if club in (row["team1"]["teamId"], row["team2"]["teamId"])), key=kickoff)
    before = [row for row in games if kickoff(row) < now]
    after = [row for row in games if kickoff(row) >= now]
    expected.update(row["matchID"] for row in before[-1:] + after[:3])
with tempfile.TemporaryDirectory(prefix="source-planning-") as temporary:
    directory = Path(temporary)
    oracle = directory / "oracle.txt"
    oracle.write_text("\n".join(" ".join(map(str, [len(values), *values])) for values in (clubs, sorted(expected))))
    espn_folder = ROOT / "aidocs/Reviews/evidence_2026-10-17/holistic-architecture/implementation-2026-10-18/provider-retention"
    espn_now = datetime.datetime(2026,10,18,12,tzinfo=datetime.timezone.utc)
    day_names = ("20261009","20261010","20261011","20261016","20261017","20261018","20261023","20261024","20261025")
    espn_expected = {int(event["id"]) for day in day_names for event in json.loads((espn_folder/f"espn-day-{day}.json").read_text())["events"]}
    espn_days = set(espn_expected)
    espn_own = []
    for team in (125,598):
        all_rows = {int(event["id"]):event for mode in ("past","future") for event in json.loads((espn_folder/f"espn-own-{team}-{mode}-2026.json").read_text())["events"]}
        dated = sorted(all_rows.values(),key=lambda event:event["date"])
        when = lambda event: datetime.datetime.fromisoformat(event["date"].replace("Z","+00:00"))
        before = [event for event in dated if when(event)<espn_now]
        after = [event for event in dated if when(event)>=espn_now]
        selected = {int(event["id"]) for event in before[-1:]+after[:3]}
        assert len(selected)==4
        espn_own.append(selected)
        espn_expected.update(selected)
    overlap = directory / "espn-overlap"
    shutil.copytree(espn_folder,overlap)
    for team in (125,598):
        past_file = overlap/f"espn-own-{team}-past-2026.json"
        future_file = overlap/f"espn-own-{team}-future-2026.json"
        past_doc = json.loads(past_file.read_text());future_doc=json.loads(future_file.read_text())
        past_doc["events"] += future_doc["events"][:3]
        future_doc["events"] = future_doc["events"][3:]
        past_file.write_text(json.dumps(past_doc));future_file.write_text(json.dumps(future_doc))
    espn_oracle = directory / "espn-oracle.txt"
    espn_oracle.write_text("\n".join(" ".join(map(str,[len(values),*sorted(values)])) for values in (espn_expected,*espn_own,espn_days)))
    binary = directory / "source-planning"
    command = ["clang++", "-std=c++20", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
    for component in ("football", "timekeeping", "json", "settings", "network"):
        command += ["-I" + str(ROOT / "components" / component / "include")]
    command += ["-I" + str(ROOT / "include")]
    for source in (
        "scripts/tests/fixtures/source_planning_host.cpp",
        "components/football/provider_plan.cpp", "components/football/source_index.cpp",
        "components/football/request_scheduler.cpp", "components/football/fixture_retention.cpp",
        "components/football/provider_adapter.cpp", "components/football/provider_readers.cpp",
        "components/football/provider_selection.cpp", "components/football/match_store.cpp",
        "components/football/store_mappers.cpp", "components/football/screen_selection.cpp",
        "components/football/football_model.cpp", "components/football/matchday.cpp",
        "components/football/store_views.cpp", "components/json/json_stream.cpp",
        "components/settings/settings_model.cpp", "components/settings/screen_policy.cpp",
        "components/network/wifi_policy.cpp", "components/timekeeping/civil_time.cpp",
    ):
        command.append(str(ROOT / source))
    subprocess.run([*command, "-o", str(binary)], check=True)
    subprocess.run([str(binary), str(season), str(oracle), str(espn_folder), str(espn_oracle), str(overlap)], check=True)
