// Actual shared protocol replay: prior escaped-key rejection, malformed skipped
// data, bounded oversized strings, duplicate schema fields and output failures.
#include <cassert>
#include <iostream>
#include <string>
#include <vector>

#include "command_json.h"
#include "json_stream.h"

struct Source {
  std::string_view text;
  std::size_t at = 0;
  json::ErrorInfo diagnostic{};
  unsigned failures = 0;
  static int read(void* context) {
    auto& self = *static_cast<Source*>(context);
    return self.at == self.text.size()
               ? -1
               : static_cast<uint8_t>(self.text[self.at++]);
  }
  json::Input input() {
    return {this, read,
            [](json::Error error, std::size_t offset, void* context) {
              auto& source = *static_cast<Source*>(context);
              source.diagnostic = {error, offset};
              ++source.failures;
            }};
  }
};
bool valid(std::string_view text) {
  Source source{text};
  json::Reader reader(source.input());
  json::Token token;
  do {
    if (!reader.next(token)) return false;
  } while (token.kind != json::Kind::kEnd);
  return true;
}
struct Sink {
  std::string text;
  std::size_t limit = SIZE_MAX;
  static bool write(std::string_view bytes, void* context) {
    auto& self = *static_cast<Sink*>(context);
    if (bytes.size() > self.limit - self.text.size()) return false;
    self.text.append(bytes);
    return true;
  }
  json::Output output() { return {this, write}; }
};
void exactIntegerCases() {
  const auto convert = [](std::string_view input, int64_t& output) {
    Source source{input};
    json::Reader reader(source.input());
    json::Token token;
    char text[256];
    if (!reader.next(token, text, sizeof(text)) ||
        token.kind != json::Kind::kNumber || token.truncated)
      return false;
    const auto number = token.text;
    // Keep the borrowed number buffer while checking the document suffix.
    json::Token end;
    if (!reader.next(end) || end.kind != json::Kind::kEnd) return false;
    return json::exactInteger(number, output);
  };
  const std::pair<std::string_view, int64_t> good[] = {
      {"0", 0},
      {"-0e999999999999", 0},
      {"0.000e-999999999999", 0},
      {"1.0", 1},
      {"1e2", 100},
      {"100e-2", 1},
      {"1.250e2", 125},
      {"1.0000000000000000000000000", 1},
      {"4294967295.0", UINT32_MAX},
      {"9223372036854775807", INT64_MAX},
      {"-9223372036854775808.0", INT64_MIN},
      {"-10e-1", -1},
      {"9e18", INT64_C(9000000000000000000)}};
  for (const auto& [input, expected] : good) {
    int64_t value = 7;
    assert(convert(input, value) && value == expected);
  }
  for (auto input : {"1.0000000000000000000001", "4294967295.0000000001",
                     "-0.00001", "1e-999999999999", "1e999999999999",
                     "9223372036854775808", "-9223372036854775809", "1.5e-1",
                     "1e19", "\"1\"", "1e", "01", "+1", "1 false"}) {
    int64_t value = 7;
    assert(!convert(input, value) && value == 7);
  }
}
int main() {
  // Raw UTF-8 byte positions, including multibyte keys, consumed bad bytes,
  // lookahead, EOF and a stable first failure across repeated calls.
  for (auto text : {std::string(R"({"x":truX})"), std::string(R"({"ä":1,})"),
                    std::string(R"([0] trailing)"), std::string(R"({"x":)"),
                    std::string("\"\x80\""), std::string(R"("\uD800x")")}) {
    const auto expected = text.find('X') != text.npos    ? text.find('X')
                          : text.find(",}") != text.npos ? text.find(",}") + 1
                          : text.find("trailing") != text.npos
                              ? text.find("trailing")
                          : text.back() == ':'             ? text.size()
                          : text.find("D800") != text.npos ? text.find('x')
                                                           : std::size_t{1};
    Source source{text};
    json::Reader reader(source.input());
    json::Token token;
    while (reader.next(token) && token.kind != json::Kind::kEnd) {
    }
    assert(reader.error() == json::Error::kSyntax);
    assert(reader.errorOffset() == expected &&
           source.diagnostic.offset == expected);
    assert(source.diagnostic.error == json::Error::kSyntax &&
           source.failures == 1);
    assert(!reader.next(token) && source.failures == 1 &&
           reader.errorOffset() == expected);
  }

  exactIntegerCases();
  const std::vector<std::string> good = {
      "{}",
      "[]",
      " {\"a\": [0,-0,12,-12,1.25,1e3,0.1E-20,true,false,null]} \n",
      "\"\\uD83D\\uDE00\"",
      "\"\\u0000\"",
      "\"äü😀\"",
      R"({"\u0074eams":[],"a\/b":"\b\f\n\r\t"})"};
  for (const auto& text : good) assert(valid(text));
  const std::vector<std::string> bad = {"",
                                        " ",
                                        "[",
                                        "{",
                                        "[1,]",
                                        "{\"a\":1,}",
                                        "[01]",
                                        "[+1]",
                                        "[.1]",
                                        "[1.]",
                                        "[1e]",
                                        "[1e+]",
                                        "[--1]",
                                        "[NaN]",
                                        "[Infinity]",
                                        "[truefalse]",
                                        "{a:1}",
                                        "{'a':1}",
                                        "{\"a\" 1}",
                                        "{\"a\":1 \"b\":2}",
                                        "true false",
                                        "\"\\uD800\"",
                                        "\"\\uDC00\"",
                                        "\"\\uD800\\u0061\"",
                                        "\"\\x00\"",
                                        std::string("\"\xc0\x80\"", 4),
                                        std::string("\"\xed\xa0\x80\"", 5),
                                        std::string("\"\xf4\x90\x80\x80\"", 6),
                                        "\"a\nb\""};
  for (const auto& text : bad) assert(!valid(text));
  // Exact declared keys compare after decoding, including escaped spellings.
  Source source{R"({"\u0074eams":[],"teams":[]})"};
  json::Reader reader(source.input());
  json::Token token;
  char buffer[16];
  const char* names[] = {"teams"};
  json::Fields fields(names, 1);
  assert(reader.next(token, buffer, sizeof buffer) &&
         token.kind == json::Kind::kObject);
  assert(reader.next(token, buffer, sizeof buffer) &&
         fields.accept(token) == 0);
  assert(reader.next(token) && reader.skip(token));
  assert(reader.next(token, buffer, sizeof buffer) &&
         fields.accept(token) == -2);
  // No body-size allocation, and a long irrelevant key/value is not rejected.
  std::string large = "{\"" + std::string(10000, 'k') + "\":\"" +
                      std::string(300000, 'v') + "\",\"teams\":[]}";
  Source big{large};
  json::Reader bounded(big.input());
  assert(bounded.next(token));
  assert(bounded.next(token, buffer, sizeof buffer) && token.truncated);
  assert(bounded.next(token, buffer, sizeof buffer) && token.truncated);
  assert(bounded.next(token, buffer, sizeof buffer) && token.text == "teams");
  assert(bounded.next(token) && bounded.skip(token));
  assert(bounded.next(token) && token.kind == json::Kind::kEndObject &&
         bounded.finish());
  // Complete decoded strings can be consumed without another body/string
  // buffer. Escapes and raw multibyte input share the same validation path.
  const std::string long_string =
      "\"" + std::string(16384, 'a') + "\\uD83D\\uDE00ä\\u0000\"";
  Source full_source{long_string};
  json::Reader full_reader(full_source.input());
  Sink decoded;
  assert(full_reader.next(token, buffer, sizeof(buffer), decoded.output()) &&
         token.truncated &&
         token.text == std::string_view(buffer, sizeof(buffer)));
  assert(decoded.text ==
         std::string(16384, 'a') + "😀ä" + std::string(1, '\0'));
  assert(token.contains_nul && full_reader.finish());
  Source no_nul_source{"\"ok\""};
  json::Reader no_nul_reader(no_nul_source.input());
  assert(no_nul_reader.next(token, buffer, sizeof(buffer)) &&
         !token.contains_nul);
  Sink cancelled;
  cancelled.limit = 17;
  Source cancel_source{long_string};
  json::Reader cancel_reader(cancel_source.input());
  assert(!cancel_reader.next(token, nullptr, 0, cancelled.output()) &&
         cancel_reader.error() == json::Error::kOutput &&
         cancelled.text.size() == 17 && cancel_source.at < long_string.size());
  assert(!cancel_reader.finish());
  for (const auto& suffix :
       {std::string("\\uD800\""), std::string("\xc0\x80\"", 3),
        std::string("\\x00\"")}) {
    const std::string malformed = "\"" + std::string(16384, 'a') + suffix;
    Source invalid_source{malformed};
    json::Reader invalid_reader(invalid_source.input());
    Sink partial;
    assert(!invalid_reader.next(token, nullptr, 0, partial.output()) &&
           invalid_reader.error() == json::Error::kSyntax &&
           !invalid_reader.finish());
  }
  Source trailing_source{"\"complete\" false"};
  json::Reader trailing_reader(trailing_source.input());
  Sink trailing;
  assert(trailing_reader.next(token, nullptr, 0, trailing.output()) &&
         trailing.text == "complete" && !trailing_reader.finish());
  // Skipped content must still satisfy the same complete grammar.
  Source skipped{R"({"unknown":[{"bad":01}]})"};
  json::Reader skip(skipped.input());
  assert(skip.next(token));
  assert(!skip.skip(token));
  std::string deep(cfg::kJsonDepth, '[');
  deep.append(cfg::kJsonDepth, ']');
  assert(valid(deep));
  assert(!valid("[" + deep + "]"));
  // Writer strings preserve explicit NUL, control escapes and UTF-8. Integer
  // limits are exact and independent of floating-point formatting or locale.
  Sink sink;
  json::Writer writer(sink.output());
  assert(writer.object() && writer.key("s") &&
         writer.string(std::string_view("a\0b", 3)));
  assert(writer.key("u") && writer.string("😀"));
  assert(writer.key("n") && writer.array() && writer.integer(UINT64_MAX));
  assert(writer.signedInteger(INT64_MIN) && writer.number("1.25e-2") &&
         writer.null() && writer.boolean(false));
  assert(writer.end() && writer.end() && writer.finish());
  assert(sink.text ==
         "{\"s\":\"a\\u0000b\",\"u\":\"😀\",\"n\":[18446744073709551615,-"
         "9223372036854775808,1.25e-2,null,false]}");
  assert(valid(sink.text));
  Sink rejected;
  rejected.limit = 4;
  json::Writer output(rejected.output());
  assert(output.object());
  assert(!output.key("too long"));
  assert(output.error() == json::Error::kOutput && !output.finish());
  assert(output.error() == json::Error::kOutput);
  Sink invalid;
  json::Writer invalid_writer(invalid.output());
  assert(!invalid_writer.string(std::string_view("\xc0\x80", 2)));
  assert(invalid_writer.error() == json::Error::kUsage);
  // File-backed strings split through every UTF-8 byte boundary, including
  // escaped control characters. A failed source/output cannot finish JSON.
  struct Chunks {
    std::string_view text;
    bool fail = false;
    static bool push(json::Output output, void* raw) {
      const auto& self = *static_cast<Chunks*>(raw);
      for (std::size_t at = 0; at < self.text.size(); ++at)
        if (!output.write(self.text.substr(at, 1), output.context))
          return false;
      return !self.fail;
    }
  } chunks{std::string_view("ä😀\0\n\"", 9)};
  Sink streamed;
  json::Writer streamed_writer(streamed.output());
  assert(streamed_writer.streamedString(Chunks::push, &chunks));
  assert(streamed_writer.finish() && valid(streamed.text));
  assert(streamed.text == "\"ä😀\\u0000\\u000a\\\"\"");
  for (auto text :
       {std::string_view("\xc3", 1), std::string_view("\xf0\x9f\x98", 3),
        std::string_view("\xc0\x80", 2)}) {
    Chunks broken{text};
    Sink sink;
    json::Writer writer(sink.output());
    assert(!writer.streamedString(Chunks::push, &broken));
    assert(writer.error() == json::Error::kUsage && !writer.finish());
  }
  Chunks failed_source{"complete text", true};
  Sink failed_string;
  json::Writer failed_stream(failed_string.output());
  assert(!failed_stream.streamedString(Chunks::push, &failed_source));
  assert(failed_stream.error() == json::Error::kOutput &&
         !failed_stream.finish());
  Sink limited;
  limited.limit = 2;
  json::Writer limited_stream(limited.output());
  assert(!limited_stream.streamedString(Chunks::push, &chunks));
  assert(limited_stream.error() == json::Error::kOutput &&
         !limited_stream.finish());
  Sink pushed;
  json::Writer push(pushed.output());
  assert(push.array() && push.beginString());
  for (char byte : std::string("ä😀")) assert(push.stringChunk({&byte, 1}));
  assert(push.endString() && push.integer(3) && push.end() && push.finish());
  assert(pushed.text == "[\"ä😀\",3]");
  Sink unfinished;
  json::Writer open(unfinished.output());
  assert(open.beginString() && open.stringChunk("partial"));
  assert(!open.finish() && open.error() == json::Error::kUsage);
  Sink interleaved;
  json::Writer incompatible(interleaved.output());
  assert(incompatible.object() && incompatible.key("name") &&
         incompatible.beginString());
  assert(!incompatible.key("wrong") &&
         incompatible.error() == json::Error::kUsage);
  // Real display-command schema: caller revision is required and exact;
  // invalid bodies publish neither a command nor its revision.
  for (auto body : {R"({"action":"next","config_revision":17})",
                    R"({"config_revision":17,"\u0061ction":"\u006eext"})",
                    R"({"action":"next","config_\u0072evision":17})"}) {
    Source source{body};
    web::DisplayCommand command;
    settings::Violation why;
    assert(web::readDisplayCommand(source.input(), command, why) ==
           web::CommandDecode::kOk);
    assert(command.action == ui::Action::kNext &&
           command.config_revision == 17);
  }
  for (const auto revision : {uint32_t{0}, UINT32_MAX}) {
    for (const auto action : {"next", "ip", "auto_next"}) {
      const std::string body =
          "{\"action\":\"" + std::string(action) +
          "\",\"config_revision\":" + std::to_string(revision) + "}";
      Source source{body};
      web::DisplayCommand command;
      settings::Violation why;
      assert(web::readDisplayCommand(source.input(), command, why) ==
             web::CommandDecode::kOk);
      assert(command.config_revision == revision);
      assert(command.action ==
             (std::string_view(action) == "next" ? ui::Action::kNext
              : std::string_view(action) == "ip" ? ui::Action::kShowIp
                                                 : ui::Action::kAutomaticNext));
    }
  }
  for (
      auto body :
      {R"({"action":"next","action":"ip","config_revision":17})",
       R"({"action":"next","ignored":1,"config_revision":17})",
       R"({"action":1,"config_revision":17})", R"({})",
       R"({"action":"next\u0000bad","config_revision":17})",
       R"({"action":"next","config_revision":17} false)",
       R"({'action':'next','config_revision':17})", R"({"action":"next"})",
       R"({"config_revision":17})",
       R"({"action":"next","config_revision":null})",
       R"({"action":"next","config_revision":"17"})",
       R"({"action":"next","config_revision":true})",
       R"({"action":"next","config_revision":-1})",
       R"({"action":"next","config_revision":4294967296})",
       R"({"action":"next","config_revision":1.5})",
       R"({"action":"next","config_revision":17.0})",
       R"({"action":"next","config_revision":17e0})",
       R"({"action":"next","config_revision":999999999999999999999999999999})",
       R"({"action":"next","config_revision":17,"config_\u0072evision":17})"}) {
    Source source{body};
    web::DisplayCommand command{ui::Action::kPrevious, 23};
    settings::Violation why;
    assert(web::readDisplayCommand(source.input(), command, why) !=
           web::CommandDecode::kOk);
    assert(command.action == ui::Action::kPrevious &&
           command.config_revision == 23);
  }
  std::cout << "shared JSON grammar replay PASS; reader="
            << sizeof(json::Reader) << ", writer=" << sizeof(json::Writer)
            << " host bytes; no protocol allocations\n";
}
