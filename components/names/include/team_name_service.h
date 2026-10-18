// Adopted-name application owner. Callers hold one network heavy-operation
// guard; no names I/O or settings publication is allowed under football locks.
#pragma once
#include <memory>

#include "app_config_model.h"
#include "effective_names.h"
#include "esp_err.h"
#include "json_stream.h"
#include "team_name_files.h"
#include "visible_names.h"

namespace football::names {
class Lookup {
 public:
  esp_err_t open();
  const store::Reader* reader() const {
    return present_ ? &read_.reader() : nullptr;
  }
  bool close() {
    present_ = false;
    return read_.close();
  }

 private:
  files::Read read_;
  bool present_ = false;
};
esp_err_t readStatus();
// Request-only export lease. Prepare before any HTTP output so memory/I/O
// failures can receive a normal error response. Write consumes the lease and
// checks close before success; early failures release both resources via RAII.
// Caller owns heavy admission through preparation and writing.
class AdoptedExport {
 public:
  AdoptedExport() = default;
  AdoptedExport(const AdoptedExport&) = delete;
  AdoptedExport& operator=(const AdoptedExport&) = delete;
  esp_err_t prepare();
  esp_err_t write(::json::Writer&);

 private:
  class Groups {
   public:
    esp_err_t prepare(const store::Reader&);
    uint16_t first(std::size_t section, uint16_t club) const;
    uint16_t next(uint16_t record) const;
    uint16_t local(std::size_t section, uint16_t record) const;

   private:
    static constexpr std::size_t kSections = cfg::kProviderCount + 1;
    std::unique_ptr<uint16_t[]> links_;
    std::array<uint16_t, kSections> bases_{};
    uint16_t clubs_ = 0;
    void prepend(std::size_t section, uint16_t club, uint16_t record);
  } groups_;
  Lookup lookup_;
  bool prepared_ = false;
};
// Owns heavy admission only on cache misses, always outside repository locks.
esp_err_t resolveVisible(Targets&, uint8_t language, VisibleCache&);
bool editable();
esp_err_t begin(files::Candidate&, bool replace_collection = false);
esp_err_t applyConfiguration(
    settings::AppConfig& desired, uint32_t expected_revision,
    files::Candidate* changed_names = nullptr, files::Receipt* result = nullptr,
    settings::Violation* violation = nullptr,
    settings::screen_policy::Error* policy_error = nullptr);
// Reset already erased NVS first; no name mutation changes references itself.
esp_err_t erase();
void invalidate();
}  // namespace football::names
