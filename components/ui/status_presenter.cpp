#include <algorithm>
#include <cstdio>
#include <cstring>
#include <new>

#include "cause_text.h"
#include "esp_app_desc.h"
#include "presenter.h"
#include "settings_store.h"
#include "time_service.h"

namespace ui {
void Presenter::localDate(char* out, std::size_t size, int64_t utc) {
  static constexpr const char* kDe[] = {"So", "Mo", "Di", "Mi",
                                        "Do", "Fr", "Sa"};
  static constexpr const char* kEn[] = {"Sun", "Mon", "Tue", "Wed",
                                        "Thu", "Fri", "Sat"};
  std::tm local{};
  timekeeping::toLocal(static_cast<time_t>(utc), local);
  snprintf(out, size, "%s %02d.%02d.",
           (model.language ? kEn : kDe)[local.tm_wday % 7], local.tm_mday,
           local.tm_mon + 1);
}

void Presenter::localTime(char* out, std::size_t size, int64_t utc) {
  std::tm local{};
  timekeeping::toLocal(static_cast<time_t>(utc), local);
  snprintf(out, size, "%02d:%02d", local.tm_hour, local.tm_min);
}

bool Presenter::entryOf(uint8_t id, settings::Competition& entry) {
  for (std::size_t i = 0; settings::competitionAt(i, entry); ++i)
    if (entry.id == id) return true;
  return false;
}

void Presenter::roundCaption(const football::MatchStore& store,
                             const football::StoredMatch& match, bool date) {
  settings::Competition entry;
  const bool known = entryOf(match.competition, entry);
  const char* competition = known ? entry.competition_name.data.data() : "";
  const std::size_t competition_length =
      known ? entry.competition_name.view().size() : 0;
  char round[40]{};
  if (match.label != football::kNoIndex)
    prefix<39>(round, store.labelAt(match.label).data);
  else if (match.round != 0)
    snprintf(round, sizeof(round),
             model.language ? "Matchday %u" : "%u. Spieltag",
             static_cast<unsigned>(match.round));
  else if (date && match.kickoff != 0)
    localDate(round, sizeof(round), match.kickoff);
  snprintf(view.caption, sizeof(view.caption), "%.*s%s%s",
           static_cast<int>(competition_length), competition,
           competition_length && round[0] ? " \xC2\xB7 " : "", round);
}

void Presenter::cardText(const char* title, const char* line, const char* hint,
                         Tone ring) {
  prefix<sizeof(view.card.title) - 1>(view.card.title, title);
  view.card.line_count = 0;
  view.card.hint_count = 0;
  if (line && line[0])
    prefix<sizeof(view.card.lines[0]) - 1>(
        view.card.lines[view.card.line_count++], line);
  if (hint && hint[0])
    prefix<sizeof(view.card.hints[0]) - 1>(
        view.card.hints[view.card.hint_count++], hint);
  view.layout = Layout::kCard;
  view.rim.kind = RimKind::kRing;
  view.rim.tone = ring;
}

void Presenter::competitionName(char (&out)[48], uint8_t id) {
  settings::Competition entry;
  football::Text<sizeof(out) - 1> name;
  if (entryOf(id, entry))
    name.assign(entry.competition_name.empty()
                    ? entry.primary.competition.view()
                    : entry.competition_name.view());
  memcpy(out, name.data, sizeof(out));
}

const football::CompetitionState* Presenter::stateOf(uint8_t id) {
  return published ? football::findState(published->competitions, id) : nullptr;
}

bool Presenter::faulted(uint8_t id) {
  const auto* state = stateOf(id);
  return state && state->active && state->cause != football::Cause::kNone;
}

uint8_t Presenter::firstFault(int& count) {
  count = 0;
  uint8_t first = 0;
  if (!published) return 0;
  for (const auto& state : published->competitions)
    if (state.active && state.cause != football::Cause::kNone && count++ == 0)
      first = state.id;
  return first;
}

Tone Presenter::causeTone(football::Cause cause) {
  return football::dataNotice(cause) ? Tone::kWarn : Tone::kError;
}

void Presenter::faultCard(uint8_t id, int64_t now, int64_t utc,
                          const char* title) {
  const auto* state = stateOf(id);
  const auto cause = state ? state->cause : football::Cause::kOther;
  char name[48], retry[40]{}, code[football::kErrorCodeBytes];
  competitionName(name, id);
  // A data notice has no failed request to retry.
  if (!football::dataNotice(cause) && published && published->retry_ms > now &&
      utc > 0) {
    char time[8];
    localTime(time, sizeof(time),
              utc + (published->retry_ms - now + 999) / 1000);
    snprintf(retry, sizeof(retry), "%s %s",
             translated("Neuer Versuch", "Retry at"), time);
  }
  const char* text = causeText(cause, model.language != 0);
  cardText(title ? title : name, title ? name : text, retry, causeTone(cause));
  presentation.card_fault = id;
  if (title)
    prefix<sizeof(view.card.lines[0]) - 1>(
        view.card.lines[view.card.line_count++], text);
  football::errorCode(code, state ? state->provider : cfg::kProviderDefault,
                      cause, state ? state->http : 0);
  prefix<sizeof(view.card.hints[0]) - 1>(
      view.card.hints[view.card.hint_count++], code);
}

void Presenter::bindStart(uint8_t step) {
  view.style = model.screens[static_cast<unsigned>(cfg::Screen::kCrest)];
  // Read one entry at a time without copying the competition list onto the
  // caller's stack.
  bool active = false, named = false;
  cfg::Provider provider{};
  char club[48] = "Fussball";
  settings::Competition entry;
  for (std::size_t i = 0; settings::competitionAt(i, entry); ++i) {
    if (entry.active && !active) {
      active = true;
      provider = entry.primary.provider;
    }
    if (!entry.team_name.empty() && !named) {
      named = true;
      snprintf(club, sizeof(club), "%.*s",
               static_cast<int>(entry.team_name.view().size()),
               entry.team_name.data.data());
    }
  }
  const char* line =
      step == 0   ? translated("Verbinde WLAN …", "Connecting WiFi …")
      : step == 1 ? translated("Hole Uhrzeit …", "Getting time …")
                  : translated("Lade Daten …", "Loading data …");
  cardText(club, line,
           step == 2 && active ? football::providerName(provider)
                               : esp_app_get_description()->version,
           Tone::kAccent);
  view.rim.kind = RimKind::kSegments;
  view.rim.done = step;
  view.rim.current = step;
  presentation.no_badge = true;
}

bool Presenter::networkFault(const BindContext& context) const {
  return context.net && context.net->init_error != ESP_OK;
}

bool Presenter::networkUseIp(const BindContext& context) const {
  if (!networkFault(context)) return false;
  const auto step = context.net->failure_step;
  const bool names = step == network::InitStep::kPortalDns ||
                     step == network::InitStep::kMdns ||
                     step == network::InitStep::kHostname ||
                     step == network::InitStep::kHttpService;
  return names && ((context.ap && context.net->ap_ip[0]) ||
                   (context.connected && context.net->ip[0]));
}

const char* Presenter::networkPhase(network::InitStep step) const {
  switch (step) {
    case network::InitStep::kAntenna:
      return translated("Antennenschalter", "Antenna switch");
    case network::InitStep::kNetworkStack:
      return translated("Netzwerkbasis", "Network stack");
    case network::InitStep::kStationInterface:
      return translated("WLAN-Schnittstelle", "WiFi interface");
    case network::InitStep::kAccessPointInterface:
      return translated("Setup-Schnittstelle", "Setup interface");
    case network::InitStep::kAccessPointAddress:
      return translated("Setup-IP", "Setup IP");
    case network::InitStep::kWifiDriver:
      return translated("WLAN-Treiber", "WiFi driver");
    case network::InitStep::kWifiConfiguration:
      return translated("WLAN-Konfiguration", "WiFi configuration");
    case network::InitStep::kWifiEvents:
      return translated("WLAN-Ereignisse", "WiFi events");
    case network::InitStep::kRadio:
      return translated("WLAN-Start", "WiFi start");
    case network::InitStep::kPortalDns:
      return translated("Setup-DNS", "Setup DNS");
    case network::InitStep::kMdns:
      return translated("Namensdienst", "Local name service");
    case network::InitStep::kHostname:
      return translated("Gerätename", "Device name");
    case network::InitStep::kHttpService:
      return translated("Web-Ankündigung", "Web announcement");
    case network::InitStep::kNone: return translated("Netzwerk", "Network");
  }
  return translated("Netzwerk", "Network");
}

void Presenter::networkFaultCard(const BindContext& context) {
  char phase[sizeof(view.card.lines[0])];
  snprintf(phase, sizeof(phase), translated("%s fehlgeschlagen", "%s failed"),
           networkPhase(context.net->failure_step));
  cardText(translated("Netzwerkstart gestört", "Network startup failed"), phase,
           networkUseIp(context)
               ? translated("IP im Browser öffnen", "Open IP in browser")
               : translated("Neuer Versuch läuft", "Retrying"),
           Tone::kWarn);
  prefix<sizeof(view.card.hints[0]) - 1>(
      view.card.hints[view.card.hint_count++],
      network::initStepCode(context.net->failure_step));
  presentation.card_network = true;
}

Subject Presenter::statusText(const football::Published& data,
                              const BindContext& context) {
  const int64_t utc = context.utc, now = context.now;
  char time[8], code[football::kErrorCodeBytes];
  const auto asOf = [&](int64_t fetched_ms) {
    localTime(time, sizeof(time), utc - (now - fetched_ms) / 1000);
  };
  // Last successful request of 'fetched_ms' (0 = none) as "Stand HH:MM".
  const auto stand = [&](int64_t fetched_ms) {
    if (fetched_ms != 0) {
      asOf(fetched_ms);
      snprintf(view.status, sizeof(view.status), "%s %s",
               translated("Stand", "As of"), time);
    } else
      snprintf(view.status, sizeof(view.status), "%s",
               translated("Daten veraltet", "Data stale"));
    view.status_tone = Tone::kWarn;
  };
  if (networkFault(context)) {
    snprintf(view.status, sizeof(view.status), "%s \xC2\xB7 %s",
             network::initStepCode(context.net->failure_step),
             networkUseIp(context) ? translated("IP nutzen", "Use IP")
                                   : translated("neuer Versuch", "retrying"));
    view.status_tone = Tone::kWarn;
    return {true, 0};
  }
  if (!context.connected) {
    snprintf(view.status, sizeof(view.status), "%s",
             translated("Kein WLAN", "No WiFi"));
    view.status_tone = Tone::kWarn;
    return {true, 0};
  }
  if (!context.valid) {
    snprintf(view.status, sizeof(view.status), "%s",
             translated("Warte auf Uhrzeit", "Waiting for time"));
    view.status_tone = Tone::kWarn;
    return {true, 0};
  }
  const bool cycle_screen = nav.selected == cfg::Screen::kMatchday ||
                            nav.selected == cfg::Screen::kTable;
  const auto* state = cycle_screen ? stateOf(presentation.on_screen) : nullptr;
  if (state) {
    if (faulted(state->id)) {
      // The HTTP status only on the card and in the Web UI.
      football::errorCode(code, state->provider, state->cause, 0);
      if (state->fetched_ms != 0) {
        asOf(state->fetched_ms);
        snprintf(view.status, sizeof(view.status), "%s \xC2\xB7 %s %s", code,
                 translated("Stand", "As of"), time);
      } else
        snprintf(view.status, sizeof(view.status), "%s", code);
      view.status_tone = causeTone(state->cause);
      return {false, state->id};
    }
    if (state->stale) stand(state->fetched_ms);
    return {};
  }
  int count = 0;
  const uint8_t first = firstFault(count);
  if (count == 1) {
    const auto* fault = stateOf(first);
    char name[48];
    competitionName(name, first);
    football::errorCode(code, fault->provider, fault->cause, 0);
    // A long name is cut at a character boundary; fit() ends it with "…".
    char band[sizeof(code) + 4 + sizeof(name)];
    snprintf(band, sizeof(band), "%s \xC2\xB7 %s", code, name);
    prefix<sizeof(view.status) - 1>(view.status, band);
    view.status_tone = causeTone(fault->cause);
    return {false, first};
  }
  if (count > 1) {
    snprintf(view.status, sizeof(view.status),
             translated("%d Wettbewerbe gestört", "%d competitions failing"),
             count);
    view.status_tone = Tone::kError;
  } else if (data.stale)
    // A competition that failed keeps earlier data: show the oldest.
    stand(data.fetched_ms);
  return {};
}

uint32_t Presenter::statusState(const football::Published& data,
                                const BindContext& context) {
  uint8_t bytes[3 + 1 + sizeof(uint32_t) + 3 * cfg::kCompetitionMax];
  std::size_t n = 0;
  bytes[n++] = context.connected;
  bytes[n++] = context.valid;
  bytes[n++] = data.stale;
  bytes[n++] =
      context.net ? static_cast<uint8_t>(context.net->failure_step) : 0;
  const uint32_t error =
      context.net ? static_cast<uint32_t>(context.net->init_error) : 0;
  memcpy(bytes + n, &error, sizeof(error));
  n += sizeof(error);
  for (const auto& state : data.competitions) {
    const bool active = state.active && state.id != 0;
    bytes[n++] = active ? state.id : 0;
    bytes[n++] = active ? static_cast<uint8_t>(state.cause) : 0;
    bytes[n++] = active && state.stale;
  }
  return football::fingerprint({reinterpret_cast<const char*>(bytes), n});
}

uint32_t Presenter::bindStatus(const football::Published& data,
                               const BindContext& context) {
  if (model.demo) {
    snprintf(view.status, sizeof(view.status), "Demo");
    view.status_tone = Tone::kDim;
    return 0;
  }
  const uint32_t state = statusState(data, context);
  const Subject subject = statusText(data, context);
  const bool repeated =
      view.layout == Layout::kCard &&
      (subject.network ? presentation.card_network
                       : subject.competition != 0 &&
                             subject.competition == presentation.card_fault);
  if (repeated) view.status[0] = 0;
  return state;
}

const char* Presenter::imageFailure(images::Failure failure) const {
  switch (failure) {
    case images::Failure::kMissing:
      return translated("Bilddatei fehlt", "Image file missing");
    case images::Failure::kUnavailable:
      return translated("Bild nicht lesbar", "Image unreadable");
    case images::Failure::kInvalid:
      return translated("Bilddatei nicht nutzbar", "Image file unusable");
    case images::Failure::kTimeout:
      return translated("Bildprüfung zu langsam", "Image check timed out");
    case images::Failure::kSource:
      return translated("Bild konnte nicht geladen werden",
                        "Image could not be loaded");
    case images::Failure::kDecode:
      return translated("Bild kann nicht dekodiert werden",
                        "Image cannot be decoded");
    case images::Failure::kMemory:
      return translated("Speicher reicht nicht", "Not enough memory");
    case images::Failure::kNone: return "";
  }
  return "";
}

bool Presenter::imageReplace(images::Failure failure) const {
  return failure == images::Failure::kMissing ||
         failure == images::Failure::kInvalid;
}

void Presenter::imageFaultCard(const images::Status& selected_image) {
  cardText(translated("Bild nicht verfügbar", "Image unavailable"),
           imageFailure(selected_image.reason),
           imageReplace(selected_image.reason)
               ? translated("Im Web Bild ersetzen", "Replace image in Web")
               : translated("Im Web Fehler prüfen", "Check error in Web"),
           Tone::kWarn);
  prefix<sizeof(view.card.lines[0]) - 1>(
      view.card.lines[view.card.line_count++],
      images::failureCode(selected_image.reason));
  prefix<sizeof(view.card.hints[0]) - 1>(
      view.card.hints[view.card.hint_count++],
      translated("Neuer Versuch läuft", "Retrying"));
}

void Presenter::finishStatus(uint32_t state, int64_t now,
                             const images::Status& selected_image) {
  const bool image_fault =
      selected_image.active && selected_image.reason != images::Failure::kNone;
  bool image_card = false;
  if (image_fault) {
    // A new attempt or retry deadline alone must not renew the five-second
    // notice. The image service retains the complete diagnosis for the Web UI.
    uint8_t bytes[sizeof(state) + 4];
    memcpy(bytes, &state, sizeof(state));
    bytes[sizeof(state)] = static_cast<uint8_t>(selected_image.reason);
    bytes[sizeof(state) + 1] = selected_image.desired_id;
    bytes[sizeof(state) + 2] = static_cast<uint8_t>(selected_image.actual);
    bytes[sizeof(state) + 3] = static_cast<uint8_t>(selected_image.role);
    state = football::fingerprint(
        {reinterpret_cast<const char*>(bytes), sizeof(bytes)});
    image_card = view.layout == Layout::kImage &&
                 nav.selected == cfg::Screen::kCrest &&
                 selected_image.actual == images::FileSource::kNone;
    if (image_card) {
      imageFaultCard(selected_image);
    } else if (!model.demo && view.status[0]) {
      // Keep the existing code/count when warnings compete for the one pill.
      // A single warning retains its normal action; a collision points to Web.
      char previous[sizeof(view.status)];
      snprintf(previous, sizeof(previous), "%s", view.status);
      if (char* separator = strstr(previous, " \xC2\xB7 ")) *separator = 0;
      snprintf(view.status, sizeof(view.status),
               "%.*s \xC2\xB7 %s \xC2\xB7 Web",
               static_cast<int>(sizeof(view.status) - 20), previous,
               images::failureCode(selected_image.reason));
      if (view.status_tone != Tone::kError) view.status_tone = Tone::kWarn;
    } else {
      snprintf(view.status, sizeof(view.status), "%s \xC2\xB7 %s",
               images::failureCode(selected_image.reason),
               imageReplace(selected_image.reason)
                   ? translated("Bild ersetzen", "Replace image")
                   : translated("Web prüfen", "Check Web UI"));
      view.status_tone = Tone::kWarn;
    }
  }
  if (!presentation.pill_known || state != presentation.pill_state) {
    presentation.pill_known = true;
    presentation.pill_state = state;
    presentation.pill_until = now + cfg::kStatusPillS * 1000;
  }
  if (model.demo &&
      (!image_fault || image_card || now >= presentation.pill_until)) {
    snprintf(view.status, sizeof(view.status), "Demo");
    view.status_tone = Tone::kDim;
    view.status_pill = false;
  } else if (now >= presentation.pill_until) {
    view.status[0] = 0;
  } else {
    view.status_pill = view.status[0] != 0;
  }
}
}  // namespace ui
