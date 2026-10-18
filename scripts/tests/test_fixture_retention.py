#!/usr/bin/env python3
"""Real response retention, with independent expected identities and row order."""
from pathlib import Path
import datetime
import json
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
season = root / "aidocs/Reviews/evidence_2026-10-17/provider-public/openliga-season.json"
own = root / "aidocs/Reviews/evidence_2026-10-17/holistic-architecture/implementation-2026-10-18/provider-retention/espn-own-schedule.json"
cup = own.parent / "cup-type1-2025.json"
calendar = own.parent / "calendar-seasonpath2025.json"
cup_day = own.parent / "cup-day20251202.json"
rows = json.loads(season.read_text())
clock = datetime.datetime(2026, 10, 14, 12, tzinfo=datetime.timezone.utc)
date = lambda row: datetime.datetime.fromisoformat(row["matchDateTimeUTC"].replace("Z", "+00:00"))
rounds = {}
for row in rows:
    rounds.setdefault(row["group"]["groupOrderID"], []).append(row)
past = [key for key, group in rounds.items() if max(date(row).date() for row in group) + datetime.timedelta(days=2) <= clock.date()]
future = sorted(set(rounds) - set(past), key=lambda key: min(date(row) for row in rounds[key]))
selected = ([max(past, key=lambda key: min(date(row) for row in rounds[key]))] if past else []) + future[:2]
club = sorted((row for row in rows if 40 in (row["team1"]["teamId"], row["team2"]["teamId"])), key=date)
past_own = [row for row in club if date(row) < clock]
next_own = [row for row in club if date(row) >= clock]
expected = {row["matchID"] for row in rows if row["group"]["groupOrderID"] in selected}
expected.update(row["matchID"] for row in past_own[-1:] + next_own[:3])
# Independent multi-club oracle from recorded provider fixtures, not selector
# output. Eight club preferences share one source; shared matches occur once.
clubs = sorted({row[side]["teamId"] for row in rows for side in ("team1", "team2")})[:8]
club_union = set()
for team in clubs:
    fixtures = sorted((row for row in rows if team in (row["team1"]["teamId"], row["team2"]["teamId"])), key=date)
    past = [row for row in fixtures if date(row) < clock]
    future = [row for row in fixtures if date(row) >= clock]
    club_union.update(row["matchID"] for row in past[-1:] + future[:3])
multi_expected = {row["matchID"] for row in rows if row["group"]["groupOrderID"] in selected} | club_union
with tempfile.TemporaryDirectory(prefix="fixture-retention-", dir="/tmp") as directory:
    folder = Path(directory)
    multi = folder / "multi.txt"
    multi.write_text("\n".join(" ".join(map(str, [len(values), *values])) for values in (clubs, sorted(club_union), sorted(multi_expected))))
    reversed_season = folder / "reversed.json"
    reversed_season.write_text(json.dumps(rows[::-1]))
    command = ["clang++", "-std=c++20", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
    for path in ("include", "components/football/include", "components/json/include", "components/settings/include", "components/timekeeping/include", "components/network/include"):
        command += ["-I", str(root / path)]
    for path in ("components/football/fixture_retention.cpp", "components/football/provider_adapter.cpp", "components/football/provider_plan.cpp", "components/football/request_scheduler.cpp", "components/football/budget_ledger.cpp", "components/football/screen_selection.cpp", "components/football/store_views.cpp", "components/settings/settings_model.cpp", "components/settings/settings_record.cpp", "components/settings/screen_policy.cpp", "components/network/wifi_policy.cpp", "components/football/provider_readers.cpp", "components/football/store_mappers.cpp", "components/football/match_store.cpp", "components/football/football_model.cpp", "components/football/matchday.cpp", "components/timekeeping/civil_time.cpp", "components/json/json_stream.cpp", "scripts/tests/fixtures/fixture_retention_host.cpp"):
        command.append(str(root / path))
    command += ["-o", str(folder / "check")]
    subprocess.run(command, check=True)
    subprocess.run([str(folder / "check"), str(season), str(reversed_season), str(own), str(cup), str(calendar), str(cup_day), str(multi), *map(str, sorted(expected))], check=True)
