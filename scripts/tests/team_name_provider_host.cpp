// Actual shared Reader -> provider facts -> mapper -> RAM fallbacks. Recorded
// OLDB/ESPN responses are historical; keyed-provider cases are synthetic.
// ArduinoJson is only an independent input oracle, never the production path.
// The retired prepared-label/visit APIs cannot be retained: adopted names now
// resolve outside this store through the separate names owner.
#include <ArduinoJson.h>

#include <cassert>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "../../test/test_match_store/recorded.h"
#include "match_store.h"
#include "provider_adapter.h"
#include "provider_plan.h"
#include "store_mappers.h"

using namespace football;
namespace football::provider {
// This sporting replay does not exercise the interactive selection parser.
ParseResult readSelectionResponse(json::Reader&, const RequestSpec&,
                                  const Sink&) {
  return {};
}
}  // namespace football::provider
namespace {
struct Labels {
  std::string name, short_name;
};
std::string text(ArduinoJson::JsonVariantConst value) {
  const auto string = value.as<ArduinoJson::JsonString>();
  return string ? std::string(string.c_str(), string.size()) : std::string{};
}
// Independent expected-prefix calculation for valid fixture strings. The
// explicit invalid-input cases below exercise the production Unicode reader.
std::string prefix(std::string value, std::size_t bound) {
  if (value.find('\0') != std::string::npos) return {};
  auto n = std::min(bound, value.size());
  while (n < value.size() && n &&
         (static_cast<unsigned char>(value[n]) & 0xc0) == 0x80)
    --n;
  value.resize(n);
  return value;
}
Labels expected(ArduinoJson::JsonVariantConst team, cfg::Provider provider) {
  const auto full =
      text(team[provider == cfg::Provider::kEspn         ? "displayName"
                : provider == cfg::Provider::kOpenLigaDb ? "teamName"
                                                         : "name"]);
  const auto short_name =
      text(team[provider == cfg::Provider::kEspn ? "shortDisplayName"
                                                 : "shortName"]);
  return {prefix(full.empty() ? short_name : full, 26),
          prefix(short_name.empty() ? full : short_name, 3)};
}
void collect(ArduinoJson::JsonVariantConst node, cfg::Provider provider,
             std::map<uint32_t, Labels>& out) {
  if (node.is<ArduinoJson::JsonArrayConst>()) {
    for (auto value : node.as<ArduinoJson::JsonArrayConst>())
      collect(value, provider, out);
    return;
  }
  if (!node.is<ArduinoJson::JsonObjectConst>()) return;
  if (provider == cfg::Provider::kFootballData &&
      node["type"].is<const char*>() && text(node["type"]) != "TOTAL" &&
      node["table"].is<ArduinoJson::JsonArrayConst>())
    return;
  const auto add = [&](ArduinoJson::JsonVariantConst team, const char* id_key) {
    const auto id = team[id_key].as<uint32_t>();
    const auto labels = expected(team, provider);
    if (id && (!labels.name.empty() || !labels.short_name.empty()))
      out[id] = labels;
  };
  if (provider == cfg::Provider::kOpenLigaDb &&
      node["teamInfoId"].is<uint32_t>())
    add(node, "teamInfoId");
  for (auto item : node.as<ArduinoJson::JsonObjectConst>()) {
    const std::string_view key = item.key().c_str();
    if (key == "team" || key == "team1" || key == "team2" ||
        key == "homeTeam" || key == "awayTeam" || key == "home" ||
        key == "away")
      add(item.value(),
          provider == cfg::Provider::kOpenLigaDb ? "teamId" : "id");
    collect(item.value(), provider, out);
  }
}
struct Body {
  std::string_view bytes;
  std::size_t at = 0;
  static int read(void* raw) {
    auto& self = *static_cast<Body*>(raw);
    return self.at < self.bytes.size()
               ? static_cast<unsigned char>(self.bytes[self.at++])
               : -1;
  }
};
struct Harness {
  MatchStore store;
  MapContext map;
  provider::RequestSpec request;
  std::vector<provider::Fixture> fixtures;
  std::vector<std::vector<provider::Event>> events;
  unsigned callback_count = 0, fail_at = 0;
  Harness(cfg::Provider provider, bool table = false) {
    request.source = {provider, "bl1", "2026"};
    request.operation =
        table ? provider::Operation::kTable : provider::Operation::kFixtures;
    map.competition = 3;
    map.provider = provider;
    map.clock = {parseUtc("2026-10-17T15:00:00Z"), 100};
  }
  provider::ParseResult parse(std::string_view bytes) {
    Body body{bytes};
    json::Reader reader({&body, Body::read});
    provider::Sink sink;
    sink.context = this;
    sink.fixture = [](const provider::Fixture& value,
                      std::span<const provider::Event> events, void* raw) {
      auto& self = *static_cast<Harness*>(raw);
      if (++self.callback_count == self.fail_at) return false;
      self.fixtures.push_back(value);
      self.events.emplace_back(events.begin(), events.end());
      self.map.club = value.home.id;
      return storeMatch(value, events, self.store, self.map);
    };
    sink.group = [](const provider::TableGroup& group, void* raw) {
      auto& self = *static_cast<Harness*>(raw);
      return self.store.appendGroup(group.key.data, group.label.data,
                                    group.ordinal);
    };
    sink.row = [](const provider::TableRow& row, void* raw) {
      auto& self = *static_cast<Harness*>(raw);
      return storeTableRow(row, self.store, self.map);
    };
    const bool table = request.operation == provider::Operation::kTable;
    if (table) store.beginTable(3);
    auto result = provider::readResponse(reader, request, sink);
    if (table)
      store.commitTable(result.status == provider::Status::kOk &&
                        result.document_complete);
    if (result.status == provider::Status::kOk) assert(body.at == bytes.size());
    return result;
  }
  const StoredTeam& team(uint32_t id) const {
    for (std::size_t i = 0; i < cfg::kStoreTeams; ++i)
      if (store.teamAt(static_cast<TeamIndex>(i)).id == id)
        return store.teamAt(static_cast<TeamIndex>(i));
    assert(false);
    return store.teamAt(0);
  }
};
void replay(cfg::Provider provider, const char* body, bool table = false,
            bool valid = true) {
  ArduinoJson::JsonDocument oracle;
  assert(!deserializeJson(oracle, body));
  std::map<uint32_t, Labels> labels;
  collect(oracle.as<ArduinoJson::JsonVariantConst>(), provider, labels);
  Harness actual(provider, table);
  const auto result = actual.parse(body);
  assert(result.document_complete);
  assert(result.status ==
         (valid ? provider::Status::kOk : provider::Status::kBadResponse));
  std::set<uint32_t> mapped;
  for (std::size_t i = 0; i < cfg::kStoreTeams; ++i) {
    const auto& team = actual.store.teamAt(static_cast<TeamIndex>(i));
    if (!team.id) continue;
    mapped.insert(team.id);
    const auto expected = labels.at(team.id);
    if (std::string_view(team.name.data) != expected.name ||
        std::string_view(team.short_name.data) != expected.short_name)
      std::cerr << "provider=" << static_cast<unsigned>(provider)
                << " team=" << team.id << " name actual=[" << team.name.data
                << "] expected=[" << expected.name << "] short actual=["
                << team.short_name.data << "] expected=[" << expected.short_name
                << "]\n";
    assert(std::string_view(team.name.data) == expected.name);
    assert(std::string_view(team.short_name.data) == expected.short_name);
  }
  assert(mapped.size() == labels.size());
  for (const auto& [id, ignored] : labels) {
    (void)ignored;
    assert(mapped.contains(id));
  }
  // Names cannot alter the decoded sporting facts or lose observed events.
  for (std::size_t i = 0; i < actual.fixtures.size(); ++i) {
    const auto& input = actual.fixtures[i];
    const auto* match = actual.store.find(3, provider, input.facts.id);
    assert(match);
    if (input.fields & provider::kKickoff)
      assert(match->kickoff == input.facts.kickoff);
    if (input.fields & provider::kPrincipal)
      assert(match->principal == input.facts.principal);
    if (actual.store.hasDetail(*match)) {
      const auto& detail = actual.store.detailAt(match->detail);
      for (unsigned at = 0; at < detail.event_count; ++at) {
        const auto& event = actual.store.eventAt(detail.events[at]);
        assert(at < actual.events[i].size());
        const auto& original = actual.events[i][at].facts;
        assert(event.identity == original.identity &&
               event.type == original.type && event.player == original.player);
      }
    }
  }
}
void source_and_update_boundaries() {
  Harness h(cfg::Provider::kOpenLigaDb);
  settings::CompetitionRoute entry;
  entry.id = 3;
  entry.incarnation = 1;
  entry.primary.provider = h.map.provider;
  entry.primary.competition.assign("bl1");
  entry.primary.season.assign("2026");
  plan::Planner planner;
  schedule::Job job;
  job.competition = 3;
  job.provider = h.map.provider;
  job.type = cfg::DataType::kFixtures;
  plan::Request request;
  assert(planner.request(job, entry, h.store, h.map.clock, request));
  const auto body = [](const char* league, unsigned season, unsigned id,
                       const char* name) {
    return std::string("[{\"matchID\":") + std::to_string(id) +
           ",\"leagueShortcut\":\"" + league +
           "\",\"leagueSeason\":" + std::to_string(season) +
           ",\"team1\":{\"teamId\":7,\"teamName\":\"" + name +
           "\",\"shortName\":\"RAW\"},\"team2\":{\"teamId\":40,\"teamName\":"
           "\"Second provider name\"}}]";
  };
  assert(h.parse(body("bl1", 2026, 700, "Full provider original München"))
             .status == provider::Status::kOk);
  assert(h.fixtures.back().source_matches &&
         planner.acceptsMatch(h.fixtures.back()));
  assert(std::string_view(h.team(7).name.data) == "Full provider original Mü");
  assert(h.parse(body("bl1", 2026, 700, "Later provider full name")).status ==
         provider::Status::kOk);
  assert(std::string_view(h.team(7).name.data) == "Later provider full name");
  for (auto [league, season] :
       {std::pair{"dfb", 2026u}, std::pair{"bl1", 2025u}}) {
    Harness other(cfg::Provider::kOpenLigaDb);
    assert(other.parse(body(league, season, 701, "Wrong source")).status ==
           provider::Status::kOk);
    assert(!other.fixtures.back().source_matches &&
           !planner.acceptsMatch(other.fixtures.back()));
  }
  Harness cancelled(cfg::Provider::kOpenLigaDb);
  cancelled.fail_at = 1;
  assert(cancelled.parse(body("bl1", 2026, 702, "Not published")).status ==
         provider::Status::kRejected);
  assert(cancelled.callback_count == 1 && !cancelled.store.matchCount());
  Harness invalid(cfg::Provider::kOpenLigaDb);
  assert(invalid.parse(body("bl1", 2026, 0, "Invalid match")).status ==
         provider::Status::kBadResponse);
  assert(invalid.fixtures.empty() && !invalid.store.matchCount());
}
void text_and_identity_boundaries() {
  replay(
      cfg::Provider::kFootballData,
      R"({"matches":[{"id":2,"homeTeam":{"id":5,"shortName":"Full-field fallback"},"awayTeam":{"id":81,"name":"Original without provider short"}}]})");
  replay(
      cfg::Provider::kFootballData,
      R"({"matches":[{"id":3,"homeTeam":{"id":5,"name":"1234567890123456789012345ü","shortName":"äöü"},"awayTeam":{"id":81,"name":"Bodö/Glimt","shortName":"😀AB"}}]})");
  Harness nul(cfg::Provider::kApiFootball);
  assert(
      nul.parse(
             R"({"response":[{"fixture":{"id":1},"teams":{"home":{"id":7,"name":"A\u0000B","shortName":"X\u0000Y"},"away":{"id":8,"name":"Other"}}}]})")
          .status == provider::Status::kOk);
  assert(nul.team(7).name.empty() && nul.team(7).short_name.empty());
  Harness bad_id(cfg::Provider::kApiFootball);
  assert(
      bad_id
          .parse(
              R"({"response":[{"fixture":{"id":"1\u0000wrong"},"teams":{"home":{"id":7,"name":"Must not match truncated ID"}}}]})")
          .status == provider::Status::kBadResponse);
  assert(bad_id.fixtures.empty());
  Harness suffix(cfg::Provider::kFootballData);
  const auto long_nul =
      std::string(
          "{\"matches\":[{\"id\":4,\"homeTeam\":{\"id\":5,\"name\":\"") +
      std::string(200, 'A') + "\\u0000suffix\"}}]}";
  assert(suffix.parse(long_nul).status == provider::Status::kOk &&
         suffix.team(5).name.empty());
  Harness malformed(cfg::Provider::kFootballData);
  const auto invalid_utf8 =
      std::string(
          "{\"matches\":[{\"id\":5,\"homeTeam\":{\"id\":5,\"name\":\"") +
      std::string(200, 'A') + char(0xc0) + char(0xaf) + "\"}}]}";
  assert(malformed.parse(invalid_utf8).status ==
         provider::Status::kBadResponse);
}
}  // namespace
int main() {
  replay(cfg::Provider::kOpenLigaDb, recorded::kOpenLigaMatchday5);
  replay(cfg::Provider::kOpenLigaDb, recorded::kOpenLigaTable, true);
  replay(cfg::Provider::kEspn, recorded::kEspnDay);
  replay(cfg::Provider::kEspn, recorded::kEspnTable, true);
  replay(
      cfg::Provider::kApiFootball,
      R"({"response":[{"fixture":{"id":1},"teams":{"home":{"id":33,"name":"Manchester United"},"away":{"id":40,"name":"Liverpool"}}}]})");
  replay(
      cfg::Provider::kApiFootball,
      R"({"response":[{"league":{"standings":[[{"team":{"id":33,"name":"Manchester United"}},{"team":{"id":40,"name":"Liverpool"}}],[{"team":{"id":50,"name":"A third group club"}}]]}}]})",
      true);
  replay(
      cfg::Provider::kFootballData,
      R"({"standings":[{"type":"HOME","table":[{"team":{"id":999,"name":"Ignored home subtotal"}}]},{"type":"TOTAL","table":[{"team":{"id":5,"name":"FC Bayern München","shortName":"Bayern"}},{"team":{"id":81,"name":"FC Barcelona","shortName":"Barça"}}]}]})",
      true);
  replay(
      cfg::Provider::kApiFootball,
      R"({"response":[{"league":{"standings":[[{"team":{"id":33,"name":"Before missing identity"}},{"team":{"name":"No ID"}},{"team":{"id":50,"name":"After invalid row"}}]]}}]})",
      true, false);
  source_and_update_boundaries();
  text_and_identity_boundaries();
  std::cout << "provider names PASS: actual Reader/facts/mapper/store, four "
               "providers, recorded OLDB/ESPN, nested TOTAL/group tables, "
               "source filters, UTF8/NUL/ID/fallback boundaries and events\n";
}
