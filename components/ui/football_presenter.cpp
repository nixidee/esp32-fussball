#include <algorithm>
#include <cstdio>
#include <cstring>
#include <new>

#include "presenter.h"
#include "settings_store.h"
#include "time_service.h"
#include "ui_controller.h"

namespace ui {
void Presenter::value(char* out, std::size_t size, int n, bool known) {
  if (size == 0) return;
  char text[8] = "--";
  if (known) snprintf(text, sizeof(text), "%d", n);
  const std::size_t count = std::min(size - 1, strlen(text));
  memcpy(out, text, count);
  out[count] = 0;
}

void Presenter::value(char* out, std::size_t size, uint8_t n) {
  value(out, size, n, n != football::kNoValue);
}

void Presenter::value(char* out, std::size_t size, int16_t n) {
  value(out, size, n, n != football::kNoNumber);
}

void Presenter::ownHighlight(const football::MatchStore& store,
                             const football::StoredMatch& match, int64_t now) {
  // Shown events only: removal markers are left out by the view.
  const auto list = football::view::events(store, match);
  if (list.count == 0) return;
  const unsigned event_step = now / cfg::kHighlightRotateMs % list.count;
  const auto& e = store.eventAt(list.index[list.count - 1 - event_step]);
  char minute[12];
  value(minute, sizeof(minute), e.minute);
  const char* label =
      (e.flags & football::kEventShootout)
          ? translated("Elfmeterschießen", "Shootout")
      : e.type == football::EventType::kOwnGoal
          ? translated("Eigentor", "Own goal")
      : e.type == football::EventType::kGoal ? translated("Tor", "Goal")
      : e.type == football::EventType::kPenaltyGoal
          ? translated("Elfmeter", "Penalty")
      : e.type == football::EventType::kPenaltyMissed
          ? translated("Verschossen", "Missed penalty")
      : e.type == football::EventType::kYellow ? translated("Gelb", "Yellow")
      : e.type == football::EventType::kYellowRed
          ? translated("Gelb-Rot", "Second yellow")
      : e.type == football::EventType::kRed ? translated("Rot", "Red")
      : e.type == football::EventType::kSubstitution
          ? translated("Wechsel", "Substitution")
      : e.type == football::EventType::kInjury
          ? translated("Verletzung", "Injury")
          : "VAR";
  char player[41];
  if (e.type == football::EventType::kSubstitution && !e.player_in.empty()) {
    char in[19], out[19];
    prefix<18>(in, e.player_in.data);
    prefix<18>(out, e.player.data);
    snprintf(player, sizeof(player), "%s > %s", out, in);
  } else
    prefix<40>(player, e.player.data);
  snprintf(view.own.highlight, sizeof(view.own.highlight),
           player[0] ? "%s' %s  %s" : "%s' %s", minute, label, player);
}

void Presenter::bindOwnMatch(const football::MatchStore& store,
                             const football::StoredMatch& match,
                             const football::view::Shown& shown,
                             const BindContext& context) {
  using football::Phase;
  view.layout = Layout::kOwnMatch;
  auto& own = view.own;
  roundCaption(store, match, false);
  prefix<40>(own.home,
             football::view::teamName(store, match, true, short_names));
  prefix<40>(own.away,
             football::view::teamName(store, match, false, short_names));
  football::view::ownSides(store, match, shown, own.own_home, own.own_away);
  const bool before = !started(match);
  const int64_t kickoff = match.kickoff;  // 0 = unknown
  if (match.principal.valid() && !before)
    pair(own.centre, "%d : %d", match.principal);
  else if (kickoff != 0)
    localTime(own.centre, sizeof(own.centre), kickoff);
  else
    snprintf(own.centre, sizeof(own.centre), "-- : --");
  auto& rim = view.rim;
  rim.kind = RimKind::kMatch;
  rim.extra = true;
  if (before) {
    if (kickoff != 0) localDate(own.pill, sizeof(own.pill), kickoff);
    own.pill_tone = Tone::kDim;
    own.pill_text = Tone::kText;
    own.highlight_tone = Tone::kDim;
    if (kickoff != 0 && context.valid && kickoff > context.utc) {
      const int64_t left = kickoff - context.utc;
      const long days = static_cast<long>(left / 86400),
                 hours = static_cast<long>(left / 3600),
                 minutes = static_cast<long>((left + 59) / 60);
      if (days >= 1)
        snprintf(own.highlight, sizeof(own.highlight),
                 model.language ? "Kickoff in %ld day%s" : "Anstoß in %ld %s",
                 days,
                 model.language ? (days == 1 ? "" : "s")
                                : (days == 1 ? "Tag" : "Tagen"));
      else if (hours >= 1)
        snprintf(own.highlight, sizeof(own.highlight), "%s %ld %s",
                 translated("Anstoß in", "Kickoff in"), hours,
                 translated("Std.", "h"));
      else
        snprintf(own.highlight, sizeof(own.highlight), "%s %ld %s",
                 translated("Anstoß in", "Kickoff in"), minutes,
                 translated("Min.", "min"));
    }
    return;
  }
  ownHighlight(store, match, context.now);
  const bool minute = match.minute != football::kNoValue;
  if (match.shootout.valid())
    snprintf(own.pill, sizeof(own.pill), "%s %d : %d",
             translated("i.E.", "pens"), match.shootout.home,
             match.shootout.away);
  else if (match.phase == Phase::kFinished)
    snprintf(own.pill, sizeof(own.pill), "%s",
             translated("Beendet", "Full time"));
  else if (match.phase == Phase::kHalfTime)
    snprintf(own.pill, sizeof(own.pill), "%s",
             translated("Halbzeit", "Half time"));
  else if (match.phase == Phase::kPenalties)
    snprintf(own.pill, sizeof(own.pill), "%s",
             translated("Elfmeterschießen", "Shootout"));
  else if (minute)
    snprintf(own.pill, sizeof(own.pill), "%s%d'",
             (match.estimated & football::kEstimatedMinute) ? "~" : "",
             match.minute);
  if (match.phase == Phase::kFinished) {
    own.pill_tone = own.pill_text = Tone::kDim;
    rim.minute = 90;
    rim.finished = true;
  } else if (match.phase == Phase::kHalfTime) {
    rim.minute = 45;
  } else if (match.phase == Phase::kPenalties) {
    rim.minute = 120;
  } else if (minute) {
    rim.minute = match.minute;
  }
  rim.gap = rim.minute >= 0;
}

void Presenter::matchRow(const football::MatchStore& store,
                         const football::StoredMatch& match,
                         const football::view::Shown& shown,
                         const BindContext& context, bool with_day,
                         MatchRow& out) {
  prefix<40>(out.home,
             football::view::teamName(store, match, true, short_names));
  prefix<40>(out.away,
             football::view::teamName(store, match, false, short_names));
  football::view::ownSides(store, match, shown, out.own_home, out.own_away);
  out.own = match.isOwn();
  // score_changed is in monotonic seconds.
  out.goal = match.score_changed > 0 &&
             context.now / 1000 - match.score_changed < cfg::kGoalFlashS;
  if (match.principal.valid() && started(match)) {
    pair(out.centre, "%d:%d", match.principal);
    out.centre_tone = Tone::kText;
  } else if (match.kickoff != 0 && !started(match)) {
    char time[8];
    localTime(time, sizeof(time), match.kickoff);
    std::tm day{}, today{};
    timekeeping::toLocal(static_cast<time_t>(match.kickoff), day);
    timekeeping::toLocal(static_cast<time_t>(context.utc), today);
    if (with_day && context.valid &&
        (day.tm_yday != today.tm_yday || day.tm_year != today.tm_year)) {
      char date[16];
      localDate(date, sizeof(date), match.kickoff);
      // Weekday only: "Sa 15:30".
      char* space = strchr(date, ' ');
      if (space) *space = 0;
      snprintf(out.centre, sizeof(out.centre), "%.3s %.5s", date, time);
    } else {
      snprintf(out.centre, sizeof(out.centre), "%s", time);
    }
    out.centre_tone = Tone::kDim;
  } else {
    snprintf(out.centre, sizeof(out.centre), "--:--");
    out.centre_tone = Tone::kDim;
  }
}

void Presenter::bindConference(const football::MatchStore& store,
                               const football::view::Shown& shown,
                               const football::view::List& members,
                               const BindContext& context) {
  const auto now = context.now;
  football::ConferenceRequest request;
  request.mode = model.conference_mode;
  request.pin_own = model.pin_own_match;
  request.manual = nav.paged;
  request.per_page = model.visible_matches;
  request.page = nav.paged
                     ? static_cast<unsigned>(nav.conference_page)
                     : static_cast<unsigned>(now / cfg::kConferencePageMs);
  const auto page = football::view::conferencePage(store, members, request);
  if (page.row_count == 0) {
    char next[48]{};
    int64_t kickoff = INT64_MAX;
    for (std::size_t i = 0; context.valid && i < store.matchCount(); ++i) {
      const auto& match = store.match(i);
      if (shown.shows(match) && match.kickoff != 0 &&
          match.kickoff > context.utc)
        kickoff = std::min<int64_t>(kickoff, match.kickoff);
    }
    if (kickoff != INT64_MAX) {
      char date[16], time[8];
      localDate(date, sizeof(date), kickoff);
      localTime(time, sizeof(time), kickoff);
      // Weekday only: "Sa 15:30".
      char* space = strchr(date, ' ');
      if (space) *space = 0;
      snprintf(next, sizeof(next), "%s %s %s", translated("Nächstes:", "Next:"),
               date, time);
    }
    cardText(translated("Konferenz", "Conference"),
             translated("Keine Spiele im Zeitfenster", "No matches in window"),
             next, Tone::kNeutral);
    return;
  }
  view.layout = Layout::kConference;
  view.page = page.page;
  view.pages = page.pages;
  view.slots = page.pages > 1 ? model.visible_matches : page.row_count;
  if (page.pages > 1)
    snprintf(view.caption, sizeof(view.caption), "%s \xC2\xB7 %u/%u",
             translated("Konferenz", "Conference"), page.page + 1u,
             static_cast<unsigned>(page.pages));
  else
    snprintf(view.caption, sizeof(view.caption), "%s",
             translated("Konferenz", "Conference"));
  const unsigned rows = std::min<unsigned>(page.row_count, view.matches.size());
  for (unsigned row = 0; row < rows; ++row)
    matchRow(store, store.match(page.rows[row]), shown, context, false,
             view.matches[row]);
  view.row_count = rows;
  // Rim: progress of the own match of the Konferenz.
  view.rim.kind = RimKind::kMatch;
  view.rim.gap = true;
  for (unsigned i = 0; i < members.count; ++i) {
    const auto& match = store.match(members.index[i]);
    if (!match.isOwn()) continue;
    if (match.phase == football::Phase::kFinished) {
      view.rim.minute = 90;
      view.rim.finished = true;
    } else if (match.phase == football::Phase::kHalfTime) {
      view.rim.minute = 45;
    } else if (started(match) && match.minute != football::kNoValue) {
      view.rim.minute = std::min<int16_t>(match.minute, 90);
    }
    break;
  }
}

void Presenter::bindTable(const football::MatchStore& store,
                          const TableSelection& selection) {
  const uint8_t id = selection.competition;
  const char* competition = nullptr;
  settings::Competition entry;
  if (id != 0 && entryOf(id, entry) && !entry.competition_name.empty()) {
    competition = entry.competition_name.data.data();
    snprintf(view.caption, sizeof(view.caption), "%.*s",
             static_cast<int>(entry.competition_name.view().size()),
             competition);
  }
  if (!competition)
    snprintf(view.caption, sizeof(view.caption), "%s",
             translated("Tabelle", "Table"));
  const auto table = football::view::table(store, id);
  const int table_rows = table.count;
  const int count = selection.count;
  if (count == 0) {
    char line[sizeof(view.caption) + 24];
    if (competition)
      snprintf(line, sizeof(line), "%s %s", view.caption,
               translated("hat keine Tabelle", "has no table"));
    else
      snprintf(line, sizeof(line), "%s",
               translated("Keine Tabelle", "No table"));
    cardText(translated("Tabelle", "Table"), line, nullptr, Tone::kNeutral);
    return;
  }
  view.layout = Layout::kTable;
  snprintf(view.columns[0], sizeof(view.columns[0]), "%s",
           translated("Pkt", "Pts"));
  snprintf(view.columns[1], sizeof(view.columns[1]), "%s",
           translated("Diff", "GD"));
  snprintf(view.columns[2], sizeof(view.columns[2]), "%s",
           translated("Sp", "P"));
  const int first = selection.first;
  view.row_count = count;
  for (int i = 0; i < count; ++i) {
    const auto& standing = store.row(table.row[first + i]);
    auto& row = view.table[i];
    prefix<40>(row.name, football::view::rowName(store, standing, short_names));
    value(row.position, sizeof(row.position),
          standing.position != football::kNoValue ? standing.position
                                                  : first + i + 1,
          true);
    value(row.played, sizeof(row.played), standing.played);
    value(row.points, sizeof(row.points), standing.points);
    if (standing.difference != football::kNoNumber)
      snprintf(row.difference, sizeof(row.difference),
               standing.difference > 0 ? "+%d" : "%d", standing.difference);
    else
      snprintf(row.difference, sizeof(row.difference), "--");
    row.own = (standing.flags & football::kRowOwn) != 0;
  }
  view.rim.kind = RimKind::kScroll;
  view.rim.first = static_cast<uint8_t>(first);
  view.rim.count = static_cast<uint8_t>(count);
  view.rim.total = static_cast<uint8_t>(table_rows);
}

void Presenter::bindTables(const football::MatchStore& store,
                           const football::view::Shown&, int64_t now,
                           int64_t utc) {
  const auto selection = tableSelection(nav, model, *navigation_data, now);
  presentation.on_screen = selection.competition;
  if (selection.fault)
    faultCard(selection.competition, now, utc);
  else
    bindTable(store, selection);
}

void Presenter::bindMatchday(const football::MatchStore& store,
                             const football::view::Shown& shown,
                             const BindContext& context) {
  auto facts = *navigation_data;
  facts.local_offset = context.valid && context.utc > 0
                           ? timekeeping::localOffset(context.utc)
                           : 0;
  const auto selection = matchdaySelection(nav, model, facts, context.now);
  if (!selection.available) {
    cardText(translated("Spieltag", "Matchday"),
             translated("Keine Spiele", "No matches"), nullptr, Tone::kNeutral);
    return;
  }
  presentation.on_screen = selection.competition;
  if (selection.fault) {
    faultCard(selection.competition, context.now, context.utc);
    return;
  }
  const auto list = football::view::matchday(
      store, shown, selection.competition, selection.span);
  const int per_page = std::max<int>(1, model.visible_matches);
  roundCaption(store, store.match(list.index[0]), true);
  view.layout = Layout::kConference;
  view.page = static_cast<uint8_t>(selection.page);
  view.pages = static_cast<uint8_t>(std::min(selection.pages, 255));
  const int from = selection.page * per_page;
  const int rows = std::min<int>(
      {per_page, list.count - from, static_cast<int>(view.matches.size())});
  view.slots = selection.pages > 1 ? static_cast<uint8_t>(per_page)
                                   : static_cast<uint8_t>(rows);
  for (int row = 0; row < rows; ++row)
    matchRow(store, store.match(list.index[from + row]), shown, context, true,
             view.matches[row]);
  view.row_count = static_cast<uint8_t>(rows);
  view.rim.kind = RimKind::kScroll;
  view.rim.first = static_cast<uint8_t>(from);
  view.rim.count = static_cast<uint8_t>(rows);
  view.rim.total = static_cast<uint8_t>(std::min<int>(list.count, 255));
}

uint32_t Presenter::bind(const BindContext& context) {
  const auto& data = *published;
  // Rebuilt in place: `view = {}` would add a ~1.9 KB temporary to the
  // main task stack.
  new (&view) ViewModel();
  presentation.no_badge = false;
  presentation.card_network = false;
  presentation.card_fault = 0;
  presentation.on_screen = 0;
  int faults = 0;
  const uint8_t first_fault = firstFault(faults);
  const auto utc = context.utc;
  const bool valid = context.valid;
  // Each competition counts only on the screens it is switched on for
  // (OQ-192). Konferenz members; the demo shows every match in kickoff
  // order.
  const auto conference = data.shown.only(cfg::CompetitionScreen::kConference);
  football::view::List members;
  if (model.demo) {
    members = football::view::conferenceAll(data.store, conference);
  } else if (valid) {
    members = football::view::conferenceMembers(
        data.store, conference, utc,
        {model.window_before, model.window_after, model.whole_matchday});
  }
  // Every active competition opens the Konferenz (OQ-146).
  const bool window = members.count > 0;
  // Own match: the first in its window, else the nearest kickoff.
  bool own_window = false;
  const auto own_shown = data.shown.only(cfg::CompetitionScreen::kOwnMatch);
  const int own_match = football::view::ownForScreen(
      data.store, own_shown, valid ? utc : 0, model.window_before,
      model.window_after, model.demo, own_window);
  const bool chosen =
      input_count && nav.manual &&
      model.screens[static_cast<unsigned>(nav.selected)].enabled;
  auto navigation = resolve(model, {data.configured, window, own_window,
                                    nav.manual, input_count, nav.selected});
  nav.default_screen = navigation.default_screen;
  nav.selected = navigation.selected;
  const Reason reason = chosen             ? Reason::kManual
                        : !data.configured ? Reason::kSetup
                        : own_window       ? Reason::kOwnMatch
                        : window           ? Reason::kMatchday
                                           : Reason::kIdle;
  presentation.screen_status =
      static_cast<uint32_t>(nav.selected) | static_cast<uint32_t>(reason) << 8;
  view.screen = nav.selected;
  view.style = model.screens[static_cast<unsigned>(nav.selected)];
  const auto now = context.now;
  if (presentation.starting) {
    const auto wifi = context.net->state;
    bool active = false;
    settings::Competition entry;
    for (std::size_t i = 0; !active && settings::competitionAt(i, entry); ++i)
      active = entry.active;
    // A failed step shows its state card instead (docs/UI.md "State cards").
    presentation.starting =
        active && !model.demo && !context.ap && data.fetched_ms == 0 &&
        faults == 0 && !networkFault(context) &&
        wifi != network::State::kBackoff &&
        wifi != network::State::kBackoffAuth &&
        wifi != network::State::kWaitSsid && wifi != network::State::kApOnly;
    if (presentation.starting) {
      bindStart(!context.connected ? 0 : !valid ? 1 : 2);
      return 0;
    }
  }
  const bool data_screen = nav.selected != cfg::Screen::kCrest;
  // Spieltag and Tabelle show a failing competition as its notice card in
  // their cycle (OQ-278).
  bool cycle_cards = false;
  if (nav.selected == cfg::Screen::kMatchday ||
      nav.selected == cfg::Screen::kTable) {
    const auto cycle = data.shown.only(nav.selected == cfg::Screen::kMatchday
                                           ? cfg::CompetitionScreen::kMatchday
                                           : cfg::CompetitionScreen::kTable);
    for (std::size_t i = 0; !cycle_cards && i < cycle.count(); ++i)
      cycle_cards = faulted(cycle.at(i).competition);
  }
  if (!data.configured && context.ap) {
    // First setup: how to reach the setup page.
    const auto& net = *context.net;
    char ssid[48];
    snprintf(ssid, sizeof(ssid), "\xC2\xBB %s", net.ap_ssid);
    cardText(translated("Einrichtung", "Setup"),
             translated("Mit WLAN verbinden:", "Join WiFi:"),
             translated("dann im Browser öffnen:", "then open in a browser:"),
             Tone::kAccent);
    prefix<sizeof(view.card.lines[1]) - 1>(view.card.lines[1], ssid);
    view.card.accent[1] = true;
    view.card.line_count = 2;
    prefix<sizeof(view.card.hints[1]) - 1>(view.card.hints[1], net.ap_ip);
    view.card.hint_count = 2;
    presentation.no_badge = true;
    presentation.card_network = true;
    if (networkFault(context)) {
      snprintf(view.card.title, sizeof(view.card.title),
               translated("%s gestört", "%s failed"),
               networkPhase(net.failure_step));
      snprintf(view.card.hints[0], sizeof(view.card.hints[0]), "%s \xC2\xB7 %s",
               translated("IP im Browser öffnen", "Open IP in browser"),
               network::initStepCode(net.failure_step));
    }
  } else if (data.fetched_ms == 0 && !model.demo && networkFault(context)) {
    networkFaultCard(context);
  } else if (data_screen && data.fetched_ms == 0 && !model.demo &&
             !(context.connected && valid && cycle_cards)) {
    // Data never loaded: say why instead of an empty screen.
    presentation.card_network = !context.connected || !valid;
    if (!context.connected) {
      // Why, from the connection state (docs/UI.md "State cards").
      const auto why = network::offlineReason(context.net->state);
      cardText(translated("Kein WLAN", "No WiFi"),
               why == network::Reason::kNotFound
                   ? translated("Netz nicht gefunden", "Network not found")
               : why == network::Reason::kPassword
                   ? translated("Passwort falsch?", "Wrong password?")
               : why == network::Reason::kSetup
                   ? translated("Setup-Netz aktiv", "Setup network on")
                   : translated("Verbinde …", "Connecting …"),
               why == network::Reason::kSetup
                   ? nullptr
                   : translated("Neuer Versuch läuft", "Retrying"),
               Tone::kWarn);
      if (context.ap && why != network::Reason::kSetup)
        prefix<sizeof(view.card.hints[0]) - 1>(
            view.card.hints[view.card.hint_count++],
            translated("Setup-Netz aktiv", "Setup network on"));
    } else if (!valid)
      cardText(translated("Warte auf Uhrzeit", "Waiting for time"),
               translated("Zeitserver wird gefragt", "Asking the time server"),
               nullptr, Tone::kWarn);
    else if (faults == 0)
      cardText(translated("Keine Daten", "No data"),
               translated("Lade Daten", "Loading data"), nullptr,
               Tone::kNeutral);
    else
      // After a failed attempt: which competition, why, the next attempt.
      faultCard(first_fault, now, utc, translated("Keine Daten", "No data"));
  } else if (nav.selected == cfg::Screen::kLiveSingle) {
    if (own_match >= 0)
      bindOwnMatch(data.store, data.store.match(own_match), own_shown, context);
    else
      cardText(translated("Eigenes Spiel", "Your match"),
               translated("Kein Spiel", "No match"), nullptr, Tone::kNeutral);
  } else if (nav.selected == cfg::Screen::kLiveMulti) {
    bindConference(data.store, conference, members, context);
  } else if (nav.selected == cfg::Screen::kMatchday) {
    bindMatchday(data.store, data.shown.only(cfg::CompetitionScreen::kMatchday),
                 context);
  } else if (nav.selected == cfg::Screen::kTable) {
    bindTables(data.store, data.shown.only(cfg::CompetitionScreen::kTable), now,
               valid ? utc : 0);
  }
  return bindStatus(data, context);
}
}  // namespace ui
