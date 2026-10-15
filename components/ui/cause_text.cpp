#include "cause_text.h"

namespace ui {
const char* causeText(football::Cause cause, bool english) {
  using football::Cause;
  const auto pick = [english](const char* de, const char* en) {
    return english ? en : de;
  };
  switch (cause) {
    case Cause::kNone: return "";
    case Cause::kProviderOff: return pick("Anbieter aus", "Provider off");
    case Cause::kBudget: return pick("Tagesbudget voll", "Daily budget used");
    case Cause::kBusy: return pick("Gerät beschäftigt", "Device busy");
    case Cause::kNetwork: return pick("WLAN unterbrochen", "WiFi interrupted");
    case Cause::kTime: return pick("Uhrzeit fehlt", "No valid time");
    case Cause::kSettings:
      return pick("Einstellung geändert", "Settings changed");
    case Cause::kRateLimited:
      return pick("Anbieter drosselt", "Provider throttles");
    case Cause::kAuth: return pick("Zugang abgelehnt", "Access denied");
    case Cause::kNotFound: return pick("Nicht gefunden", "Not found");
    case Cause::kHttp: return pick("Anbieterfehler", "Provider error");
    case Cause::kConnect: return pick("Keine Verbindung", "No connection");
    case Cause::kBadResponse:
      return pick("Antwort fehlerhaft", "Bad response");
    case Cause::kTooLarge: return pick("Antwort zu groß", "Response too large");
    case Cause::kMemory: return pick("Speicher voll", "Out of memory");
    case Cause::kOther: return pick("Abruf gescheitert", "Request failed");
    case Cause::kUnmaintained:
      return pick("Anbieter pflegt nicht", "Provider not updating");
    case Cause::kNoSeason: return pick("Keine Saison", "No season");
    case Cause::kCounter: return pick("Zähler defekt", "Counter failed");
  }
  return pick("Abruf gescheitert", "Request failed");
}
}  // namespace ui
