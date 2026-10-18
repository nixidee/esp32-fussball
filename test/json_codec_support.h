// Test-only adapters: ArduinoJson is an independent assertion oracle;
// production codecs consume and produce the shared streaming grammar directly.
#pragma once
#include <ArduinoJson.h>

#include <string>

#include "json_stream.h"

namespace codec_test {
template <class Encode>
bool write(ArduinoJson::JsonDocument& doc, Encode encode) {
  std::string text;
  json::Writer out({&text, [](std::string_view bytes, void* context) {
                      static_cast<std::string*>(context)->append(bytes);
                      return true;
                    }});
  return out.object() && encode(out) && out.end() && out.finish() &&
         !ArduinoJson::deserializeJson(doc, text);
}
template <class Decode>
bool read(ArduinoJson::JsonObjectConst doc, Decode decode) {
  struct Source {
    std::string text;
    std::size_t at = 0;
  } source;
  ArduinoJson::serializeJson(doc, source.text);
  return decode(json::Input{&source, [](void* context) {
                              auto& self = *static_cast<Source*>(context);
                              return self.at < self.text.size()
                                         ? static_cast<unsigned char>(
                                               self.text[self.at++])
                                         : -1;
                            }});
}
}  // namespace codec_test
