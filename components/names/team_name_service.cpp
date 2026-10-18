#include "team_name_service.h"

#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <memory>
#include <new>

#include "file_service.h"
#include "network_service.h"
#include "ota_service.h"
#include "settings_store.h"

namespace football::names {
namespace {
// Heavy admission serializes this checked receipt and all open readers.
files::State active;
files::Status active_status = files::Status::kMissing;
const files::Context& context() {
  static constexpr files::Context value{
      cfg::kFsBasePath, nullptr,
      [](void*, files::Usage& out) { return ::files::usage(out) == ESP_OK; }};
  return value;
}
esp_err_t error(files::Status status) {
  switch (status) {
    case files::Status::kOk:
    case files::Status::kMissing: return ESP_OK;
    case files::Status::kInvalid: return ESP_ERR_INVALID_RESPONSE;
    case files::Status::kConflict: return ESP_ERR_INVALID_STATE;
    case files::Status::kPhysicalSpace: return ::files::kStorageFull;
    case files::Status::kNoMemory: return ESP_ERR_NO_MEM;
    case files::Status::kIo: return ESP_FAIL;
  }
  return ESP_FAIL;
}
esp_err_t synchronize() {
  const auto reference = settings::namesReference();
  if (reference != active.reference ||
      (active_status != files::Status::kOk &&
       active_status != files::Status::kMissing))
    active_status = files::load(context(), reference, active);
  return error(active_status);
}
}  // namespace
esp_err_t Lookup::open() {
  if (!close()) return ESP_FAIL;
  const auto checked = synchronize();
  if (checked != ESP_OK) return checked;
  const auto status = read_.open(context(), active);
  present_ = status == files::Status::kOk;
  return error(status);
}
esp_err_t resolveVisible(Targets& targets, uint8_t language,
                         uint32_t config_revision, VisibleCache& cache) {
  uint32_t revision;
  const auto reference = settings::namesReference(&revision);
  if (revision != config_revision) return ESP_ERR_INVALID_STATE;
  if (cache.reference != reference || cache.language != language) {
    cache.count = 0;
    cache.reference = reference;
    cache.language = language;
  }
  if (!reference.slot) return ESP_OK;
  auto find = [&](const Target& target) -> const VisibleCache::Entry* {
    for (std::size_t i = 0; i < cache.count; ++i)
      if (cache.entries[i].id == target.id &&
          cache.entries[i].provider == target.provider)
        return &cache.entries[i];
    return nullptr;
  };
  auto copy = [](const Target& target, const VisibleCache::Entry& entry) {
    const auto assign = [](char* destination, std::size_t capacity,
                           const char* text) {
      if (!destination || !capacity) return;
      const auto value = std::string_view(text);
      const auto count = values::utf8PrefixBytes(value, capacity - 1);
      values::copyPadded(destination, capacity, value.substr(0, count));
    };
    if (entry.name_present)
      assign(target.output, target.capacity, entry.labels.name.data);
    if (entry.short_present)
      assign(target.short_output, target.short_capacity,
             entry.labels.short_name.data);
  };
  bool missing = false;
  for (const auto& target : targets.span()) {
    if (const auto* entry = find(target))
      copy(target, *entry);
    else
      missing = true;
  }
  if (!missing)
    return settings::generation() == config_revision ? ESP_OK
                                                     : ESP_ERR_INVALID_STATE;
  network::HeavyGuard guard;
  if (!guard) return ESP_ERR_NOT_FINISHED;
  Lookup lookup;
  const auto opened = lookup.open();
  if (opened != ESP_OK) return opened;
  if (settings::namesReference(&revision) != reference ||
      revision != config_revision)
    return ESP_ERR_INVALID_STATE;
  // Keep only the current visible projection, bounding state independently of
  // every visited page or competition. Existing cache hits were already copied.
  cache.count = 0;
  for (const auto& target : targets.span()) {
    VisibleCache::Entry entry;
    entry.id = target.id;
    entry.provider = target.provider;
    if (!adopted(lookup.reader(), target.provider, target.id, language,
                 entry.labels, entry.name_present, entry.short_present)) {
      cache.count = 0;
      return ESP_FAIL;
    }
    cache.entries[cache.count++] = entry;
    copy(target, entry);
  }
  if (!lookup.close()) {
    cache.count = 0;
    return ESP_FAIL;
  }
  return settings::generation() == config_revision ? ESP_OK
                                                   : ESP_ERR_INVALID_STATE;
}
namespace {
bool storedString(::json::Writer& writer, const store::Reader& reader,
                  uint16_t reference) {
  struct Source {
    const store::Reader& reader;
    uint16_t reference;
  } source{reader, reference};
  return writer.streamedString(
      [](::json::Output chunks, void* raw) {
        const auto& source = *static_cast<Source*>(raw);
        return source.reader.streamText(
            source.reference,
            [](const void* bytes, std::size_t size, void* raw_sink) {
              const auto& sink = *static_cast<::json::Output*>(raw_sink);
              return sink.write({static_cast<const char*>(bytes), size},
                                sink.context);
            },
            &chunks);
      },
      &source);
}
// Request-only traversal order, never another collection or text copy.
// Reverse insertion makes each forward chain retain the original numeric
// binding / alias order without a second per-club tail array. Heap bytes are
// 2*(5*clubs + bindings + aliases). The validated wire uses at least11/club,
// 2/binding and4/alias, so this index is smaller than its <=16-KiB source.
}  // namespace
esp_err_t AdoptedExport::Groups::prepare(const store::Reader& reader) {
  links_.reset();
  const auto counts = reader.counts();
  clubs_ = counts.clubs;
  std::size_t records = 0;
  for (std::size_t p = 0; p < cfg::kProviderCount; ++p) {
    bases_[p] = static_cast<uint16_t>(records);
    records += counts.bindings[p];
  }
  bases_[cfg::kProviderCount] = static_cast<uint16_t>(records);
  records += counts.aliases;
  if (!records) return ESP_OK;
  const auto heads = kSections * clubs_;
  const auto slots = heads + records;
  if (!clubs_ || records >= store::kNone ||
      slots > reader.size() / sizeof(uint16_t))
    return ESP_ERR_INVALID_RESPONSE;
  links_.reset(new (std::nothrow) uint16_t[slots]);
  if (!links_) return ESP_ERR_NO_MEM;
  std::fill_n(links_.get(), heads, store::kNone);
  for (std::size_t p = 0; p < cfg::kProviderCount; ++p)
    for (std::size_t at = counts.bindings[p]; at-- > 0;) {
      store::Binding value;
      if (!reader.binding(static_cast<cfg::Provider>(p),
                          static_cast<uint16_t>(at), value) ||
          value.club >= clubs_)
        return ESP_FAIL;
      prepend(p, value.club, static_cast<uint16_t>(bases_[p] + at));
    }
  for (std::size_t at = counts.aliases; at-- > 0;) {
    store::Alias value;
    if (!reader.alias(static_cast<uint16_t>(at), value) || value.club >= clubs_)
      return ESP_FAIL;
    prepend(cfg::kProviderCount, value.club,
            static_cast<uint16_t>(bases_[cfg::kProviderCount] + at));
  }
  return ESP_OK;
}
uint16_t AdoptedExport::Groups::first(std::size_t section,
                                      uint16_t club) const {
  return links_ ? links_[section * clubs_ + club] : store::kNone;
}
uint16_t AdoptedExport::Groups::next(uint16_t record) const {
  return links_[kSections * clubs_ + record];
}
uint16_t AdoptedExport::Groups::local(std::size_t section,
                                      uint16_t record) const {
  return record - bases_[section];
}
void AdoptedExport::Groups::prepend(std::size_t section, uint16_t club,
                                    uint16_t record) {
  auto& head = links_[section * clubs_ + club];
  links_[kSections * clubs_ + record] = head;
  head = record;
}
esp_err_t AdoptedExport::prepare() {
  prepared_ = false;
  const auto opened = lookup_.open();
  if (opened != ESP_OK) return opened;
  if (const auto* reader = lookup_.reader()) {
    const auto status = groups_.prepare(*reader);
    if (status != ESP_OK) return status;
  }
  prepared_ = true;
  return ESP_OK;
}
esp_err_t AdoptedExport::write(::json::Writer& out) {
  if (!prepared_) return ESP_ERR_INVALID_STATE;
  prepared_ = false;
  const auto* reader = lookup_.reader();
  const auto counts = reader ? reader->counts() : store::Counts{};
  if (!out.object() || !out.key("format") ||
      !out.string(cfg::kTeamNamesJsonFormat) || !out.key("version") ||
      !out.integer(cfg::kAdoptedNamesJsonVersion) || !out.key("texts") ||
      !out.object())
    return ESP_FAIL;
  for (uint16_t reference = reader ? reader->firstText() : 0; reference;) {
    char id[5];
    const auto end = std::to_chars(id, id + sizeof(id), reference);
    if (end.ec != std::errc{} || !out.key({id, std::size_t(end.ptr - id)}) ||
        !storedString(out, *reader, reference) || !reader->nextText(reference))
      return ESP_FAIL;
  }
  if (!out.end() || !out.key("clubs") || !out.array()) return ESP_FAIL;
  for (uint16_t i = 0; i < counts.clubs; ++i) {
    char key[store::kKeyBytes + 1];
    store::Club club;
    if (!reader->key(i, key) || !reader->club(i, club) || !out.object() ||
        !out.key("key") || !out.string(key))
      return ESP_FAIL;
    const char* names[] = {"de", "en", "short", "league"};
    const uint16_t references[] = {club.de, club.en, club.short_name,
                                   club.league};
    for (unsigned field = 0; field < 4; ++field)
      if (club.present & (1u << field))
        if (!out.key(names[field]) || !out.integer(references[field]))
          return ESP_FAIL;
    if (!out.key("ids") || !out.object()) return ESP_FAIL;
    for (std::size_t provider = 0; provider < cfg::kProviderCount; ++provider) {
      const auto p = static_cast<cfg::Provider>(provider);
      if (!out.key(providerName(p)) || !out.array()) return ESP_FAIL;
      for (auto binding = groups_.first(provider, i); binding != store::kNone;
           binding = groups_.next(binding)) {
        store::Binding value;
        if (!reader->binding(p, groups_.local(provider, binding), value) ||
            !out.integer(value.id))
          return ESP_FAIL;
      }
      if (!out.end()) return ESP_FAIL;
    }
    if (!out.end() || !out.key("aliases") || !out.array()) return ESP_FAIL;
    for (auto alias = groups_.first(cfg::kProviderCount, i);
         alias != store::kNone; alias = groups_.next(alias)) {
      store::Alias value;
      if (!reader->alias(groups_.local(cfg::kProviderCount, alias), value) ||
          !out.integer(value.text))
        return ESP_FAIL;
    }
    if (!out.end() || !out.end()) return ESP_FAIL;
  }
  if (!out.end() || !out.key("exceptions") || !out.array()) return ESP_FAIL;
  for (uint16_t i = 0; i < counts.exceptions; ++i) {
    store::Exception value;
    if (!reader->exception(i, value) || !out.object() || !out.key("provider") ||
        !out.integer(static_cast<uint8_t>(value.provider)) || !out.key("id") ||
        !out.integer(value.id) || !out.key("name") ||
        !out.integer(value.text) || !out.end())
      return ESP_FAIL;
  }
  return out.end() && out.end() && lookup_.close() ? ESP_OK : ESP_FAIL;
}
esp_err_t readStatus() { return synchronize(); }
bool editable() {
  const auto origin = settings::origin();
  return !ota::trial() && ::files::state() == ::files::State::kMounted &&
         origin != settings::Origin::kOutcomeUnknown &&
         origin != settings::Origin::kUnreadable &&
         origin != settings::Origin::kNoStorage;
}
esp_err_t begin(files::Candidate& candidate, bool replace_collection) {
  if (!editable()) return ESP_ERR_INVALID_STATE;
  const auto checked = synchronize();
  if (checked != ESP_OK && !replace_collection) return checked;
  return error(candidate.begin(context(), active));
}
esp_err_t prepareChange(files::Candidate& candidate,
                        settings::NamesChange& change) {
  change = {};
  if (settings::namesReference() != active.reference ||
      candidate.expectedReference() != active.reference)
    return ESP_ERR_INVALID_STATE;
  const auto reference = candidate.reference();
  if (!reference.slot) return ESP_ERR_INVALID_ARG;
  change = {active.reference, reference, &candidate,
            [](void* raw) {
              const auto receipt =
                  static_cast<files::Candidate*>(raw)->activate(active);
              active_status = receipt.status;
              return error(receipt.status);
            },
            [](void* raw) { static_cast<files::Candidate*>(raw)->preserve(); }};
  return ESP_OK;
}
void invalidate() {
  active = {};
  active_status = files::Status::kMissing;
}
esp_err_t erase() {
  if (::files::state() != ::files::State::kMounted)
    return ESP_ERR_INVALID_STATE;
  esp_err_t result = ESP_OK;
  for (const auto* name : cfg::kTeamNamesSlotFiles) {
    char path[::files::kPathBufferBytes];
    esp_err_t current = ESP_OK;
    if (!::files::buildPath(cfg::kFsBasePath, name, path))
      current = ESP_ERR_INVALID_ARG;
    else if (unlink(path) != 0 && errno != ENOENT)
      current = ::files::error(::files::fromErrno(errno));
    if (result == ESP_OK) result = current;
  }
  invalidate();
  return result;
}
}  // namespace football::names
