// Typed provider boundary. One adapter owns URL, response paths and mapping;
// the common json::Reader alone owns JSON grammar. All callbacks are
// synchronous borrows. No JSON value, DOM, stored provider body or name archive
// escapes.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "json_stream.h"
#include "match_store.h"

namespace settings {
struct ProviderSettings;
}

namespace football::provider {
struct SourceInput {
  cfg::Provider provider = cfg::kProviderDefault;
  std::string_view competition, season;
};
enum class Operation : uint8_t {
  kFixtures,
  kOwnFixtures,
  kTable,
  kCurrentSeason,
  kCurrentRound,
  kLastChange,
  kCalendarDays,
  kSeasonCalendar,
  kSummary,
  kCompetitions,
  kTeams,
  kOriginalNames
};
// Logical data availability, independent of whether a provider needs a
// separate request for that data. OpenLigaDB goals arrive with fixtures.
struct Capabilities {
  bool fixtures = false, table = false, events = false, coverage = false;
};
Capabilities capabilities(cfg::Provider) noexcept;

struct RequestSpec {
  SourceInput source;
  Operation operation = Operation::kFixtures;
  uint32_t from = 0, to = 0, match = 0, team = 0;
  uint16_t round = 0;
  std::string_view country;
};
struct SelectionPlan {
  Id season;
  bool discover = false;
};
// Provider-owned season requirements/default for explicit setup/editor queries.
// No URL, provider enum rule or invented season belongs in the worker.
bool prepareSelection(const SourceInput&, Operation, int64_t utc,
                      SelectionPlan&) noexcept;

struct HttpRequest {
  char url[320]{};
  std::size_t body_limit = cfg::kProviderBodyBytes;
};
bool buildRequest(const RequestSpec& request, HttpRequest& out) noexcept;

struct Authentication {
  const char* header = nullptr;
  std::string_view value;
};
Authentication authentication(cfg::Provider,
                              const settings::ProviderSettings&) noexcept;

struct TeamInput {
  uint32_t id = 0;
  Text<cfg::kTeamNameBytes> name;
  Text<cfg::kTeamShortBytes> short_name;
};
enum MatchField : uint16_t {
  kKickoff = 1 << 0,
  kPhase = 1 << 1,
  kMinute = 1 << 2,
  kAdded = 1 << 3,
  kPeriod = 1 << 4,
  kPrincipal = 1 << 5,
  kShootout = 1 << 6,
  kRound = 1 << 7
};
struct Fixture {
  StoredMatch facts;
  TeamInput home, away;
  Text<31> round_label;
  uint16_t fields = 0;
  // Scope evidence is independent of display admission. Missing/TBD timing or
  // source metadata cannot establish completeness, including for skipped rows.
  bool source_matches = false, time_definite = false;
  bool scope_matches = true;
  bool carries_events = false, events_truncated = false;
};
struct Event {
  StoredEvent facts;
  uint32_t reported_team = 0;
};
struct TableGroup {
  Text<31> key, label;
  uint8_t ordinal = 0;
};
struct TableRow {
  TeamInput team;
  StoredRow facts;
  uint8_t group = 0;
};
struct CalendarRound {
  Text<31> label;
  uint32_t from = 0, to = 0;
  uint8_t number = 0;
};
struct Metadata {
  Id season;
  uint32_t expected_rows = 0;
  uint32_t from = 0, to = 0, previous_day = 0, next_day = 0;
  uint32_t calendar_count = 0;
  uint16_t current_page = 0, total_pages = 0;
  uint16_t round = 0;
  bool expected_rows_present = false, source_matches = false;
  bool calendar_verified = false, metadata_complete = false;
  bool duplicate = false, invalid = false;
};
enum class Status : uint8_t {
  kOk,
  kBadResponse,
  kMissing,
  kTooLarge,
  kNoMemory,
  kRejected
};
struct ParseResult {
  Status status = Status::kBadResponse;
  Metadata metadata;
  uint32_t rows = 0;
  bool primary_seen = false, document_complete = false, early_stopped = false;
};
enum class OriginalField : uint8_t { kName, kShortName };
// Full provider labels are streamed only for an explicit editor query. These
// callbacks receive decoded UTF-8 chunks, never a JSON token or retained tree.
struct OriginalsSink {
  void* context = nullptr;
  bool (*begin)(void*) = nullptr;
  bool (*identity)(uint32_t, void*) = nullptr;
  bool (*text_begin)(OriginalField, void*) = nullptr;
  bool (*text_chunk)(std::string_view, void*) = nullptr;
  bool (*text_end)(OriginalField, void*) = nullptr;
  bool (*end)(void*) = nullptr;
};
struct Sink {
  void* context = nullptr;
  // Events belong to this complete typed fixture; their span is invalidated
  // by the next callback. Highlights and all shootout attempts are distinct
  // consumers of the same event facts, never a twelve-attempt truncation.
  bool (*fixture)(const Fixture&, std::span<const Event>, void*) = nullptr;
  bool (*group)(const TableGroup&, void*) = nullptr;
  bool (*row)(const TableRow&, void*) = nullptr;
  bool (*event)(const Event&, void*) = nullptr;
  bool (*selection)(const Selection&, void*) = nullptr;
  bool (*day)(uint32_t, void*) = nullptr;
  bool (*text)(std::string_view, void*) = nullptr;
  bool (*calendar_round)(const CalendarRound&, void*) = nullptr;
  bool (*enough)(void*) = nullptr;
  // Optional explicit-query lifetime; transport checks it between bounded
  // waits.
  bool (*cancelled)(void*) = nullptr;
  const OriginalsSink* originals = nullptr;
};
// Valid individual fixture fields may reach the sink incrementally. Only a
// complete document with matching nonduplicated metadata establishes an
// authoritative scope, successful empty table, pruning or last-success time.
ParseResult readSelectionResponse(json::Reader& reader,
                                  const RequestSpec& request, const Sink& sink);
ParseResult readResponse(json::Reader& reader, const RequestSpec& request,
                         const Sink& sink);
}  // namespace football::provider
