#include <cstdio>
#include <cstring>

#include "provider_adapter.h"

namespace football::provider {
namespace {
using json::Kind;
using json::Token;

// Selection rows are consumed one at a time. Original labels use a decoded
// string sink; neither the complete response nor an arbitrarily long label is
// retained. A failed suffix never receives document_complete.
class SelectionReader {
 public:
  SelectionReader(json::Reader& input, const RequestSpec& spec,
                  const Sink& output)
      : reader(input), request(spec), sink(output) {}
  ParseResult run();

 private:
  json::Reader& reader;
  const RequestSpec& request;
  const Sink& sink;
  ParseResult result;
  char buffer[cfg::kNameBytes + 1]{};
  bool stopped = false, rejected = false;

  bool next(Token& token, json::Output decoded = {}) {
    return reader.next(token, buffer, sizeof(buffer), decoded);
  }
  bool bad() {
    result.metadata.invalid = true;
    return false;
  }
  template <std::size_t N, class F>
  bool object(const Token& token, const char* const (&names)[N], F field) {
    if (token.kind != Kind::kObject) return bad();
    json::Fields fields(names, N);
    Token key, value;
    while (next(key)) {
      if (key.kind == Kind::kEndObject) return true;
      if (key.kind != Kind::kKey) return bad();
      const int index = fields.accept(key);
      if (index == -2) {
        result.metadata.duplicate = true;
        return false;
      }
      if (index < 0) {
        if (!next(value) || !reader.skip(value)) return false;
      } else if (!field(index))
        return false;
      if (stopped) return true;
    }
    return false;
  }
  template <class F>
  bool array(const Token& token, F row) {
    if (token.kind != Kind::kArray) return bad();
    Token value;
    while (next(value)) {
      if (value.kind == Kind::kEndArray) return true;
      if (!row(value) || stopped) return stopped;
    }
    return false;
  }
  bool text(const Token& token, bool truncate = false) {
    return token.kind == Kind::kString && !token.contains_nul &&
           (truncate || !token.truncated);
  }
  bool identity(const Token& token, Id& id, bool numeric) {
    if ((token.kind != Kind::kString && token.kind != Kind::kNumber) ||
        token.truncated || token.contains_nul || token.text.empty())
      return bad();
    if (numeric) {
      uint32_t number = 0;
      if (!(number = values::positiveUint32(token.text))) return bad();
      char normalized[11];
      snprintf(normalized, sizeof(normalized), "%lu",
               static_cast<unsigned long>(number));
      return id.assign(normalized);
    }
    return id.assign(token.text);
  }
  bool year(const Token& token, Id& season) {
    uint32_t number = 0;
    if ((token.kind != Kind::kString && token.kind != Kind::kNumber) ||
        token.truncated || token.contains_nul || token.text.size() != 4 ||
        !(number = values::positiveUint32(token.text)) || number < 1000 ||
        number > 9999)
      return bad();
    return season.assign(token.text);
  }
  struct StringPush {
    const OriginalsSink* sink;
    OriginalField field;
    bool begun = false;
    static bool write(std::string_view bytes, void* context) {
      auto& self = *static_cast<StringPush*>(context);
      if (!self.begun) {
        if (!self.sink->text_begin(self.field, self.sink->context))
          return false;
        self.begun = true;
      }
      return self.sink->text_chunk(bytes, self.sink->context);
    }
  };
  bool label(Selection& selection, bool short_name) {
    Token value;
    StringPush push{sink.originals, short_name ? OriginalField::kShortName
                                               : OriginalField::kName};
    const json::Output decoded = sink.originals
                                     ? json::Output{&push, StringPush::write}
                                     : json::Output{};
    if (!next(value, decoded)) return false;
    if (short_name && value.kind == Kind::kNull) return true;
    if (!text(value, true)) return bad();
    if (!short_name) selection.name.assign(value.text);
    if (sink.originals) {
      if (!push.begun &&
          !sink.originals->text_begin(push.field, sink.originals->context))
        return false;
      if (!sink.originals->text_end(push.field, sink.originals->context))
        return false;
    }
    return true;
  }
  bool team(const Token&, Selection&, cfg::Provider);
  bool row(const Token&);
  bool rows(const Token& token) {
    result.primary_seen = true;
    return array(token, [&](const Token& value) { return row(value); });
  }
  bool apiSeasons(const Token&, Id&);
  bool espnTeams(const Token&);
  bool root(const Token&);
  bool emptyErrors(const Token& token) {
    if (token.kind != Kind::kObject && token.kind != Kind::kArray) return bad();
    Token value;
    if (!next(value)) return false;
    return value.kind == (token.kind == Kind::kObject ? Kind::kEndObject
                                                      : Kind::kEndArray) ||
           bad();
  }
};

bool SelectionReader::team(const Token& token, Selection& selection,
                           cfg::Provider provider) {
  const bool olg = provider == cfg::Provider::kOpenLigaDb;
  const bool espn = provider == cfg::Provider::kEspn;
  const char* const names[] = {olg ? "teamId" : "id",
                               olg    ? "teamName"
                               : espn ? "displayName"
                                      : "name",
                               olg    ? "shortName"
                               : espn ? "abbreviation"
                               : provider == cfg::Provider::kApiFootball
                                   ? "code"
                                   : "shortName"};
  uint8_t seen = 0;
  if (!object(token, names, [&](int field) {
        seen |= uint8_t(1 << field);
        if (field != 0) return label(selection, field == 2);
        Token value;
        if (!next(value) || !identity(value, selection.id, true)) return false;
        if (sink.originals) {
          uint32_t number = 0;
          if (!(number = values::positiveUint32(selection.id.data)) ||
              !sink.originals->identity(number, sink.originals->context))
            return false;
        }
        return true;
      }))
    return false;
  return (seen & 3) == 3 || bad();
}

bool SelectionReader::apiSeasons(const Token& token, Id& season) {
  bool found = false;
  return array(token, [&](const Token& value) {
    Id candidate;
    bool current = false;
    const char* const names[] = {"year", "current"};
    if (!object(value, names, [&](int field) {
          Token item;
          if (!next(item)) return false;
          if (field == 0) return year(item, candidate);
          if (item.kind != Kind::kTrue && item.kind != Kind::kFalse)
            return bad();
          current = item.kind == Kind::kTrue;
          return true;
        }))
      return false;
    if (current) {
      if (found || candidate.empty()) return bad();
      season = candidate;
      found = true;
    }
    return true;
  });
}

bool SelectionReader::row(const Token& token) {
  Selection selection;
  const auto provider = request.source.provider;
  const bool teams = request.operation != Operation::kCompetitions;
  if (sink.originals && !sink.originals->begin(sink.originals->context))
    return false;
  bool football = true;
  if (teams) {
    if (provider == cfg::Provider::kApiFootball ||
        provider == cfg::Provider::kEspn) {
      bool seen = false;
      const char* const keys[] = {"team"};
      if (!object(token, keys,
                  [&](int) {
                    Token value;
                    seen = true;
                    return next(value) && team(value, selection, provider);
                  }) ||
          !seen)
        return bad();
    } else if (!team(token, selection, provider))
      return false;
  } else if (provider == cfg::Provider::kApiFootball) {
    const char* const keys[] = {"league", "seasons"};
    bool seen = false;
    if (!object(token, keys,
                [&](int field) {
                  Token value;
                  if (!next(value)) return false;
                  if (field == 1) return apiSeasons(value, selection.season);
                  seen = true;
                  const char* const names[] = {"id", "name"};
                  uint8_t fields = 0;
                  return object(value, names,
                                [&](int item) {
                                  Token text_value;
                                  fields |= uint8_t(1 << item);
                                  if (!next(text_value)) return false;
                                  if (item == 0)
                                    return identity(text_value, selection.id,
                                                    true);
                                  if (!text(text_value, true)) return bad();
                                  selection.name.assign(text_value.text);
                                  return true;
                                }) &&
                         (fields == 3 || bad());
                }) ||
        !seen)
      return bad();
  } else {
    const bool olg = provider == cfg::Provider::kOpenLigaDb;
    const char* const keys[] = {olg ? "leagueShortcut"
                                : provider == cfg::Provider::kEspn ? "slug"
                                                                   : "id",
                                olg ? "leagueName" : "name", "leagueSeason",
                                "sport", "currentSeason"};
    uint8_t fields = 0;
    football = !olg;
    if (!object(token, keys,
                [&](int field) {
                  Token value;
                  if (!next(value)) return false;
                  fields |= uint8_t(1 << field);
                  if (field == 0)
                    return identity(value, selection.id,
                                    provider == cfg::Provider::kFootballData);
                  if (field == 1) {
                    if (!text(value, true)) return bad();
                    selection.name.assign(value.text);
                    return true;
                  }
                  if (field == 2) return year(value, selection.season);
                  if (field == 3) {
                    const char* const sport[] = {"sportId"};
                    return object(value, sport, [&](int) {
                      Token id;
                      if (!next(id)) return false;
                      Id parsed;
                      if (!identity(id, parsed, true)) return false;
                      football = std::string_view(parsed.data) == "1";
                      return true;
                    });
                  }
                  if (value.kind == Kind::kNull) return true;
                  const char* const season[] = {"startDate"};
                  return object(value, season, [&](int) {
                    Token date;
                    if (!next(date) || !text(date) || date.text.size() < 4)
                      return bad();
                    date.text = date.text.substr(0, 4);
                    return year(date, selection.season);
                  });
                }) ||
        (fields & 3) != 3)
      return bad();
  }
  ++result.rows;
  if (sink.originals && !sink.originals->end(sink.originals->context))
    return false;
  if (football && sink.selection && !sink.selection(selection, sink.context)) {
    rejected = true;
    return false;
  }
  if (sink.enough && sink.enough(sink.context)) stopped = true;
  return true;
}

bool SelectionReader::espnTeams(const Token& token) {
  const char* const sports_keys[] = {"leagues"};
  return array(token, [&](const Token& sport) {
    return object(sport, sports_keys, [&](int) {
      Token leagues;
      if (!next(leagues)) return false;
      return array(leagues, [&](const Token& league) {
        const char* const league_keys[] = {"teams", "slug"};
        return object(league, league_keys, [&](int field) {
          Token value;
          if (!next(value)) return false;
          if (field == 0) return rows(value);
          return (text(value) && value.text == request.source.competition) ||
                 bad();
        });
      });
    });
  });
}

bool SelectionReader::root(const Token& token) {
  const auto provider = request.source.provider;
  const bool teams = request.operation != Operation::kCompetitions;
  if (provider == cfg::Provider::kOpenLigaDb) return rows(token);
  const char* const names[] = {
      provider == cfg::Provider::kApiFootball ? "response"
      : provider == cfg::Provider::kEspn      ? teams ? "sports" : "leagues"
      : teams                                 ? "teams"
                                              : "competitions",
      "errors", "results", "paging", "count"};
  return object(token, names, [&](int field) {
    Token value;
    if (!next(value)) return false;
    if (field == 0)
      return provider == cfg::Provider::kEspn && teams ? espnTeams(value)
                                                       : rows(value);
    if (field == 1 && provider == cfg::Provider::kApiFootball)
      return emptyErrors(value);
    if (field == 2 || field == 4) {
      uint32_t count = 0;
      if (value.kind != Kind::kNumber || value.truncated || value.contains_nul)
        return bad();
      if (value.text != "0" && !(count = values::positiveUint32(value.text)))
        return bad();
      result.metadata.expected_rows_present = true;
      result.metadata.expected_rows = count;
      return true;
    }
    if (field == 3 && provider == cfg::Provider::kApiFootball) {
      const char* const paging[] = {"current", "total"};
      return object(value, paging, [&](int item) {
        Token number;
        uint32_t count = 0;
        if (!next(number) || number.kind != Kind::kNumber || number.truncated ||
            number.contains_nul ||
            !(count = values::positiveUint32(number.text)) ||
            count > UINT16_MAX)
          return bad();
        (item == 0 ? result.metadata.current_page
                   : result.metadata.total_pages) =
            static_cast<uint16_t>(count);
        return true;
      });
    }
    return reader.skip(value);
  });
}

ParseResult SelectionReader::run() {
  const auto* originals = sink.originals;
  if (originals &&
      (!originals->begin || !originals->identity || !originals->text_begin ||
       !originals->text_chunk || !originals->text_end || !originals->end))
    return result;
  Token token;
  const bool valid = next(token) && root(token);
  if (stopped && valid) {
    result.status = Status::kOk;
    result.early_stopped = true;
    return result;
  }
  result.document_complete = valid && reader.finish();
  if (!result.document_complete || result.metadata.invalid ||
      result.metadata.duplicate || !result.primary_seen ||
      (result.metadata.expected_rows_present &&
       result.metadata.expected_rows != result.rows) ||
      result.metadata.total_pages > 1) {
    result.status = rejected              ? Status::kRejected
                    : result.primary_seen ? Status::kBadResponse
                                          : Status::kMissing;
    return result;
  }
  result.metadata.metadata_complete = true;
  result.status = Status::kOk;
  return result;
}
}  // namespace

ParseResult readSelectionResponse(json::Reader& reader,
                                  const RequestSpec& request,
                                  const Sink& sink) {
  return SelectionReader(reader, request, sink).run();
}
}  // namespace football::provider
