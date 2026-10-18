// Whole production image/files/JPEG owners are linked by the runner.
#include <ArduinoJson.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "display_port.h"
#include "file_io.h"
#include "file_service.h"
#include "image_service.h"
#include "network_service.h"
#include "request_json.h"
#include "settings_store.h"
#include "tjpgd.h"

std::string imageTestRoot;
bool trial = false, mounted = true, lock_ok = true, heavy_ok = true,
     fail_rename = false, fail_unlink = false;
bool fail_open = false, fail_read = false, fail_write = false,
     fail_sync = false, fail_close = false;
int write_error = ENOSPC, other_error = EIO;
std::string fail_stat_suffix;
unsigned read_interrupts = 0, write_interrupts = 0;
std::size_t io_chunk = 0;
bool references = true, save_fails = false, save_unknown = false;
unsigned validations = 0, settings_revision = 0;
std::size_t free_bytes = 1024 * 1024;
int64_t fake_now = 1000000;
int64_t esp_timer_get_time() { return fake_now; }
uint32_t esp_random() { return 17; }
using portMUX_TYPE = int;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(value) ((void)(value))
#define portEXIT_CRITICAL(value) ((void)(value))
template <class... T>
void testLog(const T&...) {}
#define ESP_LOGI(...) testLog(__VA_ARGS__)
#define ESP_LOGW(...) testLog(__VA_ARGS__)
namespace ota {
bool trial() { return ::trial; }
}  // namespace ota
namespace network {
bool tryHeavy() { return heavy_ok; }
void releaseHeavy() {}
}  // namespace network
namespace files {
State state() { return mounted ? State::kMounted : State::kUnavailable; }
esp_err_t usage(Usage& out) {
  out = {1024 * 1024, 1024 * 1024 - free_bytes};
  return ESP_OK;
}
esp_err_t format() {
  std::filesystem::remove_all(imageTestRoot);
  std::filesystem::create_directory(imageTestRoot);
  return ESP_OK;
}
}  // namespace files
namespace display {
ImageObservation observed{};
bool ready() { return true; }
bool lock(uint32_t) { return lock_ok; }
void unlock() {}
ImageObservation imageObservation() { return observed; }
}  // namespace display
namespace settings {
ImageMask referencedImages() { return references ? ImageMask{2} : 0; }
esp_err_t dropMissingImages(ImageMask present,
                            ImageCleanupCompletion completion, void* context,
                            ImageCleanupProgress* progress) {
  *progress = {};
  progress->revision = settings_revision;
  if (save_fails) {
    progress->outcome_unknown = save_unknown;
    return ESP_FAIL;
  }
  if (references && !(present & 2)) {
    references = false;
    ++settings_revision;
    progress->settings_saved = true;
  }
  progress->references_cleared = true;
  progress->revision = settings_revision;
  return completion ? completion(context) : ESP_OK;
}
}  // namespace settings
std::string mappedPath(const char* path) {
  const std::string_view value(path);
  return value.starts_with("/fs/")
             ? imageTestRoot + std::string(value.substr(3))
             : std::string(value);
}
void lv_image_cache_drop(const char*) {}
int test_open(const char* path, int flags, int mode = 0) {
  if (fail_open) {
    fail_open = false;
    errno = other_error;
    return -1;
  }
  return ::open(mappedPath(path).c_str(), flags, mode);
}
ssize_t test_read(int fd, void* data, std::size_t count) {
  if (fail_read) {
    fail_read = false;
    errno = other_error;
    return -1;
  }
  if (read_interrupts) {
    --read_interrupts;
    errno = EINTR;
    return -1;
  }
  return ::read(fd, data, io_chunk ? std::min(count, io_chunk) : count);
}
ssize_t test_write(int fd, const void* data, std::size_t count) {
  if (fail_write) {
    fail_write = false;
    errno = write_error;
    return -1;
  }
  if (write_interrupts) {
    --write_interrupts;
    errno = EINTR;
    return -1;
  }
  return ::write(fd, data, io_chunk ? std::min(count, io_chunk) : count);
}
int test_sync(int fd) {
  if (fail_sync) {
    fail_sync = false;
    errno = other_error;
    return -1;
  }
  return ::fsync(fd);
}
int test_close(int fd) {
  const int result = ::close(fd);
  if (fail_close) {
    fail_close = false;
    errno = other_error;
    return -1;
  }
  return result;
}
int test_rename(const char* from, const char* to) {
  if (fail_rename) {
    errno = other_error;
    return -1;
  }
  return ::rename(mappedPath(from).c_str(), mappedPath(to).c_str());
}
int test_unlink(const char* path) {
  if (fail_unlink && std::string_view(path).ends_with("img1.jpg")) {
    errno = other_error;
    return -1;
  }
  return ::unlink(mappedPath(path).c_str());
}
int test_stat(const char* path, struct stat* out) {
  if (!fail_stat_suffix.empty() &&
      std::string_view(path).ends_with(fail_stat_suffix)) {
    errno = EIO;
    return -1;
  }
  return ::stat(mappedPath(path).c_str(), out);
}
unsigned stdio_fault = 0;
int stdio_error = EIO, stdio_close_error = 0;
bool stdio_failed = false, stdio_deadline = false;
void stdioFailure() {
  errno = stdio_error;
  if (stdio_deadline) fake_now += 2000000;
}
FILE* test_fopen(const char* path, const char* mode) {
  ++validations;  // Observe the actual decoder's file-open boundary.
  if (stdio_fault == 1) {
    stdio_fault = 0;
    stdioFailure();
    return nullptr;
  }
  return ::fopen(mappedPath(path).c_str(), mode);
}
size_t test_fread(void* bytes, size_t size, size_t count, FILE* file) {
  if (stdio_fault == 2) {
    stdio_fault = 0;
    stdio_failed = true;
    stdioFailure();
    return 0;
  }
  return ::fread(bytes, size, count, file);
}
int test_ferror(FILE* file) { return stdio_failed ? 1 : ::ferror(file); }
int test_fseek(FILE* file, long offset, int origin) {
  if (stdio_fault == 3) {
    stdio_fault = 0;
    stdioFailure();
    return -1;
  }
  return ::fseek(file, offset, origin);
}
long test_ftell(FILE* file) {
  if (stdio_fault == 4) {
    stdio_fault = 0;
    stdioFailure();
    return -1;
  }
  return ::ftell(file);
}
int test_fclose(FILE* file) {
  const auto result = ::fclose(file);
  if (stdio_close_error) {
    errno = stdio_close_error;
    stdio_close_error = 0;
    return -1;
  }
  if (stdio_fault == 5) {
    stdio_fault = 0;
    stdioFailure();
    return -1;
  }
  return result;
}
// IMAGE_SERVICE
// JPEG_VALIDATOR
struct httpd_req_t {
  const char* body;
};
struct Request {
  explicit Request(httpd_req_t*) {}
};
// HTTP_FAULTS
ArduinoJson::JsonDocument response;
std::string response_status;
unsigned json_started = 0;
using web::readImageId;
const Fault* admitSession(httpd_req_t*, void*, bool) { return nullptr; }
int error(httpd_req_t*, const Fault&) { return ESP_FAIL; }
int failed(httpd_req_t*, esp_err_t error, const char* = nullptr) {
  response_status = error == ESP_ERR_NOT_FOUND ? "404 Not Found"
                                               : "500 Internal Server Error";
  return ESP_FAIL;
}
void httpd_resp_set_status(httpd_req_t*, const char* value) {
  response_status = value;
}
template <class T, class Decode>
bool readCommand(httpd_req_t* req, T& value, Decode decode, int& reply) {
  struct Input {
    std::string_view text;
    std::size_t at = 0;
    static int read(void* raw) {
      auto& in = *static_cast<Input*>(raw);
      return in.at < in.text.size() ? static_cast<uint8_t>(in.text[in.at++])
                                    : -1;
    }
  } source{req->body};
  settings::Violation why;
  if (decode(json::Input{&source, Input::read}, value, why) ==
      web::SettingsDecode::kOk)
    return true;
  reply = ESP_FAIL;
  return false;
}
template <class Write>
int streamJson(httpd_req_t*, Write write) {
  ++json_started;
  std::string bytes;
  json::Writer out({&bytes, [](std::string_view text, void* raw) {
                      static_cast<std::string*>(raw)->append(text);
                      return true;
                    }});
  assert(out.object() && write(out) && out.end() && out.finish());
  assert(!deserializeJson(response, bytes));
  return ESP_OK;
}
// DELETE_HANDLER

std::vector<uint8_t> readFile(const std::string& path) {
  std::ifstream file(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(file), {}};
}
void writeFile(const std::string& name, const std::vector<uint8_t>& bytes) {
  std::ofstream file(imageTestRoot + "/" + name, std::ios::binary);
  file.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
  assert(file.good());
}
std::vector<uint8_t> jpeg;
void reset() {
  images::abort();
  std::filesystem::remove_all(imageTestRoot);
  std::filesystem::create_directory(imageTestRoot);
  fail_open = fail_read = fail_write = fail_sync = fail_close = fail_rename =
      fail_unlink = false;
  write_error = ENOSPC;
  other_error = EIO;
  stdio_fault = 0;
  stdio_error = EIO;
  stdio_close_error = 0;
  stdio_failed = stdio_deadline = false;
  fail_stat_suffix.clear();
  json_started = 0;
  response_status.clear();
  read_interrupts = write_interrupts = 0;
  io_chunk = 0;
  trial = false;
  mounted = true;
  heavy_ok = lock_ok = true;
  references = true;
  save_fails = save_unknown = false;
  free_bytes = 1024 * 1024;
  display::observed = {};
  validations = 0;
  assert(images::format() == ESP_OK);
  images::clearSelection();
  writeFile("default_crest.jpg", jpeg);
  writeFile("default_background.jpg", jpeg);
}
int upload(uint8_t id, const std::vector<uint8_t>& bytes) {
  auto error = images::begin(id, bytes.size());
  if (error == ESP_OK) error = images::write(bytes.data(), bytes.size());
  if (error == ESP_OK) error = images::finish(fake_now / 1000 + 1000);
  if (error != ESP_OK) images::abort();
  return error;
}
void publication() {
  reset();
  free_bytes = cfg::kFileReplacementWorkspaceBytes;
  assert(upload(1, jpeg) ==
         ESP_OK);  // reserve is consumed once, not added to size
  assert(readFile(imageTestRoot + "/img1.jpg") == jpeg);
  read_interrupts = write_interrupts = 2;
  io_chunk = 17;
  assert(images::copyToRole(1, cfg::kStockCrest) == ESP_OK);
  assert(images::copyToRole(1, cfg::kStockBackground) == ESP_OK);
  assert(readFile(imageTestRoot + "/role_background.jpg") == jpeg);
  io_chunk = 0;
  const auto previous = readFile(imageTestRoot + "/role_crest.jpg");
  auto bad = jpeg;
  bad[0] ^= 0xff;
  writeFile("img1.jpg", bad);
  assert(images::copyToRole(1, cfg::kStockCrest) == ESP_ERR_INVALID_RESPONSE);
  assert(readFile(imageTestRoot + "/role_crest.jpg") == previous);
  writeFile("img1.jpg", jpeg);
  for (unsigned mode = 0; mode < 5; ++mode) {
    fail_read = mode == 0;
    fail_write = mode == 1;
    fail_sync = mode == 2;
    fail_close = mode == 3;
    fail_rename = mode == 4;
    assert(images::copyToRole(1, cfg::kStockCrest) != ESP_OK);
    fail_rename = false;
    assert(readFile(imageTestRoot + "/role_crest.jpg") == previous);
    assert(!std::filesystem::exists(imageTestRoot + "/.image_upload"));
  }
  assert(images::begin(1, jpeg.size()) == ESP_OK);
  assert(images::write(jpeg.data(), jpeg.size() - 1) == ESP_OK);
  assert(images::finish(fake_now / 1000 + 1000) != ESP_OK);
  assert(readFile(imageTestRoot + "/img1.jpg") == jpeg);
  free_bytes = cfg::kFileReplacementWorkspaceBytes - 1;
  assert(images::begin(1, jpeg.size()) == files::kStorageFull);
  free_bytes = 1024 * 1024;
  references = false;
  writeFile("img2.jpg", {});
  images::Inventory inventory;
  assert(images::inventory(inventory) == ESP_OK);
  assert(inventory.library[1].present && inventory.library[1].bytes == 0);
  assert(images::begin(0, jpeg.size()) == ESP_OK && images::uploadedId() == 3);
  images::abort();

  std::cout << "image publication: actual JPEG copy validation, "
               "read/write/sync/close/rename faults, incomplete upload, single "
               "workspace PASS\n";
}
void capacityFailures() {
  reset();
  assert(upload(1, jpeg) == ESP_OK);
  assert(images::copyToRole(1, cfg::kStockCrest) == ESP_OK);
  const auto previous = readFile(imageTestRoot + "/role_crest.jpg");
  for (const int cause : {ENOSPC, ENOMEM, EIO}) {
    const auto expected = cause == ENOSPC   ? files::kStorageFull
                          : cause == ENOMEM ? ESP_ERR_NO_MEM
                                            : ESP_FAIL;
    const auto* code = cause == ENOSPC   ? "storage_full"
                       : cause == ENOMEM ? "memory"
                                         : "internal";
    assert(std::string_view(faultFor(expected).code) == code);
    write_error = other_error = cause;
    for (unsigned mode = 0; mode < 6; ++mode) {
      fail_open = mode == 0;
      fail_read = mode == 1;
      fail_write = mode == 2;
      fail_sync = mode == 3;
      fail_close = mode == 4;
      fail_rename = mode == 5;
      assert(images::copyToRole(1, cfg::kStockCrest) == expected);
      fail_rename = false;
      assert(readFile(imageTestRoot + "/role_crest.jpg") == previous);
      assert(!std::filesystem::exists(imageTestRoot + "/.image_upload"));
    }
  }
  reset();
  references = false;
  for (uint8_t id = 1; id <= cfg::kImageLibraryMax; ++id)
    writeFile("img" + std::to_string(id) + ".jpg", jpeg);
  assert(images::begin(0, jpeg.size()) == files::kStorageFull);
  reset();
  writeFile("img2.jpg", jpeg);
  std::filesystem::resize_file(imageTestRoot + "/img2.jpg",
                               cfg::kImageTotalBytes);
  assert(images::begin(1, jpeg.size()) == files::kStorageFull);
  assert(!std::filesystem::exists(imageTestRoot + "/.image_upload"));
  reset();
  assert(upload(1, jpeg) == ESP_OK);
  httpd_req_t request{"{\"id\":1}"};
  fail_unlink = true;
  other_error = ENOSPC;
  assert(deleteImage(&request) == ESP_OK);
  assert(response_status == "507 Insufficient Storage" &&
         response["code"] == "storage_full");
  assert(response["references_cleared"] == true &&
         response["settings_saved"] == true &&
         response["file_deleted"] == false);
  assert(readFile(imageTestRoot + "/img1.jpg") == jpeg);
  std::cout
      << "capacity: actual quota/full IDs/workspace and POSIX "
         "open/read/write/sync/close/rename distinguish ENOSPC/ENOMEM/EIO, "
         "active files and HTTP partial receipts retained PASS\n";
}
void jpegIoFailures() {
  reset();
  assert(upload(1, jpeg) == ESP_OK);
  const auto previous = readFile(imageTestRoot + "/img1.jpg");
  for (const int cause : {ENOSPC, ENOMEM, EIO}) {
    for (unsigned mode = 1; mode <= 5; ++mode) {
      stdio_fault = mode;
      stdio_error = cause;
      stdio_failed = false;
      const auto expected = cause == ENOSPC   ? files::kStorageFull
                            : cause == ENOMEM ? ESP_ERR_NO_MEM
                                              : ESP_FAIL;
      assert(upload(1, jpeg) == expected);
      assert(stdio_fault == 0 &&
             readFile(imageTestRoot + "/img1.jpg") == previous);
      assert(!std::filesystem::exists(imageTestRoot + "/.image_upload"));
    }
  }
  for (unsigned mode = 1; mode <= 5; ++mode) {
    stdio_fault = mode;
    stdio_error = ENOMEM;
    stdio_failed = false;
    stdio_deadline = true;
    assert(display::validateJpeg((imageTestRoot + "/img1.jpg").c_str(), 240,
                                 240,
                                 fake_now / 1000 + 1000) == ESP_ERR_TIMEOUT);
  }
  stdio_deadline = false;
  stdio_failed = false;
  stdio_fault = 2;
  stdio_error = ENOSPC;
  stdio_close_error = ENOMEM;
  assert(display::validateJpeg((imageTestRoot + "/img1.jpg").c_str(), 240, 240,
                               fake_now / 1000 + 1000) == files::kStorageFull);
  assert(stdio_close_error == 0);
  std::cout
      << "JPEG stdio: actual fopen/read/seek/tell/close ENOSPC/ENOMEM/EIO "
         "retain active file, first failure and deadline priority PASS\n";
}
void inventory() {
  reset();
  assert(upload(1, jpeg) == ESP_OK);
  assert(images::copyToRole(1, cfg::kStockCrest) == ESP_OK);
  writeFile("img2.jpg", {});
  images::Inventory value;
  assert(images::inventory(value) == ESP_OK);
  assert(value.count == 2 && value.bytes == 2 * jpeg.size());
  assert(value.library[0].present && value.library[0].bytes == jpeg.size());
  assert(value.library[1].present && value.library[1].bytes == 0);
  assert(value.replacements[cfg::kStockCrest].present);
  assert(!value.replacements[cfg::kStockBackground].present);
  httpd_req_t request{""};
  assert(getImages(&request) == ESP_OK && json_started == 1);
  assert(response["count"] == 2 && response["used"] == 2 * jpeg.size());
  assert(response["roles"][0]["replaced"] == true);
  char path[files::kPathBufferBytes];
  for (const auto* suffix : {"img1.jpg", "role_crest.jpg"}) {
    fail_stat_suffix = suffix;
    json_started = 0;
    assert(images::inventory(value) == ESP_FAIL);
    assert(getImages(&request) == ESP_FAIL && json_started == 0 &&
           response_status == "500 Internal Server Error");
    const auto id = std::string_view(suffix) == "img1.jpg" ? 1 : 0;
    assert(images::filePath(id, cfg::kStockCrest, path, sizeof(path)) ==
               ESP_FAIL &&
           path[0] == 0);
  }
  fail_stat_suffix.clear();
  assert(images::filePath(0, cfg::kStockBackground, path, sizeof(path)) ==
         ESP_OK);
  assert(std::string_view(path).ends_with("default_background.jpg"));
  assert(images::filePath(3, cfg::kStockCrest, path, sizeof(path)) ==
         ESP_ERR_NOT_FOUND);
  mounted = false;
  json_started = 0;
  assert(images::inventory(value) == ESP_ERR_INVALID_STATE);
  assert(getImages(&request) == ESP_FAIL && json_started == 0);
  std::cout
      << "image inventory: one complete scan, zero-byte presence, stat-I/O "
         "rejects before HTTP output; only ENOENT selects factory PASS\n";
}
void deletion() {
  reset();
  assert(upload(1, jpeg) == ESP_OK);
  images::DeleteResult result;
  mounted = false;
  assert(images::remove(1, &result) == ESP_ERR_INVALID_STATE && references &&
         !result.references_cleared && !result.already_missing);
  mounted = true;
  fail_unlink = true;
  assert(images::remove(1, &result) != ESP_OK);
  assert(result.references_cleared && result.settings_saved &&
         !result.file_deleted && !result.already_missing && !references);
  assert(readFile(imageTestRoot + "/img1.jpg") == jpeg);
  fail_unlink = false;
  assert(images::remove(1, &result) == ESP_OK && result.file_deleted &&
         !result.settings_saved);
  assert(images::remove(1, &result) == ESP_OK && result.already_missing &&
         !result.file_deleted);
  assert(upload(1, jpeg) == ESP_OK);
  references = true;
  save_fails = save_unknown = true;
  assert(images::remove(1, &result) != ESP_OK && result.outcome_unknown &&
         !result.references_cleared && !result.file_deleted);
  assert(readFile(imageTestRoot + "/img1.jpg") == jpeg);
  std::cout << "image deletion: committed-reference/unlink partial receipt, "
               "missing file, uncertain settings stops deletion PASS\n";
}
void deletionWire() {
  reset();
  assert(upload(1, jpeg) == ESP_OK);
  httpd_req_t request{"{\"id\":1}"};
  fail_unlink = true;
  assert(deleteImage(&request) == ESP_OK);
  assert(response_status == "500 Internal Server Error" &&
         response["settings_saved"] == true &&
         response["references_cleared"] == true &&
         response["file_deleted"] == false && response["code"] == "internal");
  fail_unlink = false;
  assert(deleteImage(&request) == ESP_OK);
  assert(response["ok"] == true && response["file_deleted"] == true &&
         response["settings_saved"] == false);
  assert(deleteImage(&request) == ESP_OK &&
         response["already_missing"] == true);
  assert(upload(1, jpeg) == ESP_OK);
  references = true;
  save_fails = save_unknown = true;
  assert(deleteImage(&request) == ESP_OK &&
         response["outcome_unknown"] == true &&
         response["file_deleted"] == false);
  std::cout << "actual delete HTTP handler preserves partial/uncertain service "
               "receipt PASS\n";
}
void validationCache() {
  reset();
  // Existing files after boot start unchecked; no private cache mutation.
  writeFile("img1.jpg", jpeg);
  writeFile("img2.jpg", jpeg);
  validations = 0;
  char source[64];
  auto first = images::select(1, cfg::kStockCrest, source, sizeof(source));
  assert(first.actual == images::FileSource::kLibrary && validations == 1);
  images::clearSelection();
  images::select(2, cfg::kStockCrest, source, sizeof(source));
  images::clearSelection();
  first = images::select(1, cfg::kStockCrest, source, sizeof(source));
  assert(first.actual == images::FileSource::kLibrary && validations == 2);
  auto bad = jpeg;
  bad[0] ^= 0xff;
  writeFile("img1.jpg", bad);
  display::observed = {first.attempt,
                       display::ImageDecodeResult::kDecodeFailed};
  auto fallback = images::select(1, cfg::kStockCrest, source, sizeof(source));
  assert(fallback.actual == images::FileSource::kFactory);
  const auto after_failure = validations;
  writeFile("img1.jpg", jpeg);
  fake_now += (cfg::kImageSourceRetryMs + 1) * 1000;
  display::observed = {};
  auto recovered = images::select(1, cfg::kStockCrest, source, sizeof(source));
  assert(recovered.actual == images::FileSource::kLibrary &&
         recovered.reason == images::Failure::kNone &&
         validations == after_failure + 1);
  reset();
  const auto background =
      images::select(0, cfg::kStockBackground, source, sizeof(source));
  assert(background.actual == images::FileSource::kFactory &&
         background.reason == images::Failure::kNone);
  std::cout << "image selection: unchanged files skip full decode; observed "
               "failure invalidates and retry restores desired file PASS\n";
}
int main(int argc, char** argv) {
  assert(argc == 3);
  imageTestRoot = argv[1];
  jpeg = readFile(argv[2]);
  assert(!jpeg.empty());
  assert(roleNamed("crest") == cfg::kStockCrest &&
         roleNamed("background") == cfg::kStockBackground);
  assert(roleNamed("unknown") == cfg::kStockImageCount);
  for (unsigned role = 0; role < cfg::kStockImageCount; role++)
    assert(roleNamed(kStockNames[role]) == role);
  publication();
  capacityFailures();
  jpegIoFailures();
  inventory();
  deletion();
  deletionWire();
  validationCache();
  images::abort();
}
