#include <algorithm>
#include <cstdio>
#include <cstring>
#include <new>

#include "presenter.h"
#include "ui_controller.h"

namespace ui {
void Presenter::value(char* out, std::size_t size, int n, bool known) {
  if (size == 0) return;
  char text[8] = "--";
  if (known) values::formatText(text, sizeof(text), "%d", n);
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

void Presenter::matchProgress(const football::StoredMatch& match) {
  using football::Period;
  using football::Phase;
  auto& rim = view.rim;
  rim.kind = RimKind::kMatch;
  const bool extra = match.period == Period::kExtraFirstHalf ||
                     match.period == Period::kExtraSecondHalf ||
                     match.phase == Phase::kExtraTime ||
                     match.phase == Phase::kPenalties;
  const bool finished = match.phase == Phase::kFinished;
  const bool known = match.minute != football::kNoValue;
  rim.progress_known =
      known || finished || match.phase == Phase::kHalfTime || extra;
  const unsigned minute = extra || finished                 ? 90
                          : match.phase == Phase::kHalfTime ? 45
                          : known ? std::min<unsigned>(match.minute, 90)
                                  : 0;
  rim.progress_degrees = minute * 4;
  rim.progress_tone = finished ? Tone::kDim : Tone::kAccent;
  rim.extra_known = extra;
  if (extra && known)
    rim.extra_degrees =
        std::min<unsigned>(30, match.minute > 90 ? match.minute - 90 : 0) * 12;
  if (match.phase == Phase::kPenalties) rim.extra_degrees = 360;
  rim.gap = rim.progress_known;
  rim.top = (!started(match) || match.phase == Phase::kPenalties) ? Tone::kWarn
                                                                  : Tone::kDim;
  rim.bottom = match.phase == Phase::kHalfTime ? Tone::kWarn : Tone::kDim;
  if (known && !finished) {
    unsigned boundary = 0;
    switch (match.period) {
      case Period::kFirstHalf: boundary = 45; break;
      case Period::kSecondHalf: boundary = 90; break;
      case Period::kExtraFirstHalf: boundary = 105; break;
      case Period::kExtraSecondHalf: boundary = 120; break;
      default:
        boundary = extra ? (match.minute <= 105 ? 105 : 120)
                         : (match.minute <= 45 ? 45 : 90);
        break;
    }
    if (match.minute >= boundary ||
        (match.added != football::kNoValue && match.added > 0)) {
      if (boundary == 45 || boundary == 105)
        rim.bottom = Tone::kWarn;
      else
        rim.top = Tone::kWarn;
    }
  }
}
void Presenter::shootout(const football::MatchStore& store,
                         const football::StoredMatch& match, int64_t now) {
  const auto events = football::view::shootout(store, match);
  auto& out = view.own.shootout;
  out.active = match.phase == football::Phase::kPenalties ||
               match.shootout.valid() || events.count;
  if (!out.active) return;
  unsigned scored_home = 0, scored_away = 0;
  bool consistent =
      events.count != 0 && !(match.flags & football::kMatchEventsPoolCut);
  for (unsigned i = 0; i < events.count; ++i) {
    const auto& event = store.eventAt(events.index[i]);
    const bool goal = event.type == football::EventType::kPenaltyGoal ||
                      event.type == football::EventType::kGoal;
    if (!goal && event.type != football::EventType::kPenaltyMissed)
      consistent = false;
    if (event.actor == football::Side::kHome)
      scored_home += goal;
    else if (event.actor == football::Side::kAway)
      scored_away += goal;
    else
      consistent = false;
  }
  if (!match.shootout.valid() ||
      scored_home != static_cast<unsigned>(match.shootout.home) ||
      scored_away != static_cast<unsigned>(match.shootout.away))
    consistent = false;
  const unsigned pages = std::max<unsigned>(
      1, (events.count + out.attempts.size() - 1) / out.attempts.size());
  const unsigned first =
      now / cfg::kHighlightRotateMs % pages * out.attempts.size();
  out.count = std::min<unsigned>(out.attempts.size(), events.count - first);
  for (unsigned i = 0; i < out.count; ++i) {
    const auto& event = store.eventAt(events.index[first + i]);
    const bool goal = event.type == football::EventType::kPenaltyGoal ||
                      event.type == football::EventType::kGoal;
    if (!goal && event.type != football::EventType::kPenaltyMissed) {
      out.attempts[i] =
          event.actor == football::Side::kHome   ? Attempt::kHomeUnknown
          : event.actor == football::Side::kAway ? Attempt::kAwayUnknown
                                                 : Attempt::kUnknown;
      continue;
    }
    out.attempts[i] =
        event.actor == football::Side::kHome
            ? (goal ? Attempt::kHomeScored : Attempt::kHomeMissed)
        : event.actor == football::Side::kAway
            ? (goal ? Attempt::kAwayScored : Attempt::kAwayMissed)
            : (goal ? Attempt::kUnknownScored : Attempt::kUnknownMissed);
  }
  if (!events.count)
    values::formatText(
        out.caption, sizeof(out.caption), "%s",
        translated("Versuche nicht geliefert", "Attempts not supplied"));
  else
    values::formatText(out.caption, sizeof(out.caption), "%s %u–%u/%u%s",
                       translated("Versuche", "Attempts"), first + 1,
                       first + out.count, events.count,
                       consistent
                           ? translated(" · gemeldet", " · reported")
                           : translated(" · unvollständig", " · incomplete"));
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
  if (e.minute != football::kNoValue && e.added != football::kNoValue &&
      e.added != 0)
    values::formatText(minute, sizeof(minute), "%u+%u", e.minute, e.added);
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
    values::formatText(player, sizeof(player), "%s > %s", out, in);
  } else
    prefix<40>(player, e.player.data);
  values::formatText(view.own.highlight, sizeof(view.own.highlight),
                     player[0] ? "%s' %s  %s" : "%s' %s", minute, label,
                     player);
}

void Presenter::bindOwnMatch(const football::MatchStore& store,
                             const football::StoredMatch& match,
                             const football::view::Shown& shown,
                             const BindContext& context) {
  using football::Phase;
  view.layout = Layout::kOwnMatch;
  auto& own = view.own;
  roundCaption(store, match, false);
  namePrefix<40>(own.home, football::view::teamName(store, match, true));
  namePrefix<40>(own.away, football::view::teamName(store, match, false));
  teamTarget(own.home, own.home_short, store, match.home);
  teamTarget(own.away, own.away_short, store, match.away);
  football::view::ownSides(store, match, shown, own.own_home, own.own_away);
  const bool before = !started(match);
  const int64_t kickoff = match.kickoff;  // 0 = unknown
  if (match.principal.valid() && !before)
    pair(own.centre, "%d : %d", match.principal);
  else if (kickoff != 0)
    localTime(own.centre, sizeof(own.centre), kickoff);
  else
    values::formatText(own.centre, sizeof(own.centre), "-- : --");
  matchProgress(match);
  own.large_centre = true;
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
        values::formatText(
            own.highlight, sizeof(own.highlight),
            model.language ? "Kickoff in %ld day%s" : "Anstoß in %ld %s", days,
            model.language ? (days == 1 ? "" : "s")
                           : (days == 1 ? "Tag" : "Tagen"));
      else if (hours >= 1)
        values::formatText(own.highlight, sizeof(own.highlight), "%s %ld %s",
                           translated("Anstoß in", "Kickoff in"), hours,
                           translated("Std.", "h"));
      else
        values::formatText(own.highlight, sizeof(own.highlight), "%s %ld %s",
                           translated("Anstoß in", "Kickoff in"), minutes,
                           translated("Min.", "min"));
    }
    return;
  }
  ownHighlight(store, match, context.now);
  shootout(store, match, context.now);
  const bool minute = match.minute != football::kNoValue;
  if (match.shootout.valid())
    values::formatText(own.pill, sizeof(own.pill), "%s %d : %d",
                       translated("i.E.", "pens"), match.shootout.home,
                       match.shootout.away);
  else if (match.phase == Phase::kFinished)
    values::formatText(own.pill, sizeof(own.pill), "%s",
                       translated("Beendet", "Full time"));
  else if (match.phase == Phase::kHalfTime)
    values::formatText(own.pill, sizeof(own.pill), "%s",
                       translated("Halbzeit", "Half time"));
  else if (match.phase == Phase::kPenalties)
    values::formatText(own.pill, sizeof(own.pill), "%s",
                       translated("Elfmeterschießen", "Shootout"));
  else if (match.phase == Phase::kSuspended)
    values::formatText(own.pill, sizeof(own.pill), "%s",
                       translated("Unterbrochen", "Suspended"));
  else if (minute) {
    if (match.added != football::kNoValue && match.added > 0)
      values::formatText(
          own.pill, sizeof(own.pill), "%s%u+%u'",
          (match.estimated & football::kEstimatedMinute) ? "~" : "",
          match.minute, match.added);
    else
      values::formatText(
          own.pill, sizeof(own.pill), "%s%u'",
          (match.estimated & football::kEstimatedMinute) ? "~" : "",
          match.minute);
  }
  if (match.phase == Phase::kFinished)
    own.pill_tone = own.pill_text = Tone::kDim;
}

void Presenter::matchRow(const football::MatchStore& store,
                         const football::StoredMatch& match,
                         const football::view::Shown& shown,
                         const BindContext& context, bool with_day,
                         MatchRow& out) {
  namePrefix<40>(out.home, football::view::teamName(store, match, true));
  namePrefix<40>(out.away, football::view::teamName(store, match, false));
  teamTarget(out.home, out.home_short, store, match.home);
  teamTarget(out.away, out.away_short, store, match.away);
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
    const auto day = localCivil(match.kickoff), today = localCivil(context.utc);
    if (with_day && context.valid &&
        (day.tm_yday != today.tm_yday || day.tm_year != today.tm_year)) {
      char date[16];
      localDate(date, sizeof(date), match.kickoff);
      // Weekday only: "Sa 15:30".
      char* space = strchr(date, ' ');
      if (space) *space = 0;
      values::formatText(out.centre, sizeof(out.centre), "%.3s %.5s", date,
                         time);
    } else {
      values::formatText(out.centre, sizeof(out.centre), "%s", time);
    }
    out.centre_tone = Tone::kDim;
  } else {
    values::formatText(out.centre, sizeof(out.centre), "--:--");
    out.centre_tone = Tone::kDim;
  }
}

void Presenter::bindConference(const football::MatchStore& store,
                               const football::view::Shown& shown,
                               const PreparedContent& content,
                               const BindContext& context) {
  const auto& page = content.matches;
  view.layout = Layout::kConference;
  view.page = page.page;
  view.pages = page.pages;
  view.slots = page.pages > 1 ? policy.visible_matches : page.row_count;
  if (page.pages > 1)
    values::formatText(view.caption, sizeof(view.caption), "%s \xC2\xB7 %u/%u",
                       translated("Konferenz", "Conference"), page.page + 1u,
                       static_cast<unsigned>(page.pages));
  else
    values::formatText(view.caption, sizeof(view.caption), "%s",
                       translated("Konferenz", "Conference"));
  const unsigned rows = std::min<unsigned>(page.row_count, view.matches.size());
  for (unsigned row = 0; row < rows; ++row)
    matchRow(store, store.match(page.rows[row]), shown, context, false,
             view.matches[row]);
  view.row_count = rows;
  // Rim: progress of the own match of the Konferenz.
  if (content.own != football::kNoMatch)
    matchProgress(store.match(content.own));
}

void Presenter::bindTable(const football::MatchStore& store,
                          const PreparedContent& content) {
  const auto& selection = content.table;
  const uint8_t id = selection.competition;
  const char* competition = nullptr;
  settings::CompetitionRoute entry;
  if (id != 0 && entryOf(id, entry) && !entry.competition_name.empty()) {
    competition = entry.competition_name.data.data();
    values::formatText(view.caption, sizeof(view.caption), "%.*s",
                       static_cast<int>(entry.competition_name.view().size()),
                       competition);
  }
  if (!competition)
    values::formatText(view.caption, sizeof(view.caption), "%s",
                       translated("Tabelle", "Table"));
  if (selection.label != football::kNoIndex) {
    char competition_label[sizeof(view.caption)];
    memcpy(competition_label, view.caption, sizeof(competition_label));
    values::formatText(view.caption, sizeof(view.caption), "%s · %s",
                       store.labelAt(selection.label).data, competition_label);
  }
  const int table_rows = selection.total;
  const int count = selection.count;
  if (count == 0) {
    char line[sizeof(view.caption) + 24];
    if (competition)
      values::formatText(line, sizeof(line), "%s %s", view.caption,
                         translated("hat keine Tabelle", "has no table"));
    else
      values::formatText(line, sizeof(line), "%s",
                         translated("Keine Tabelle", "No table"));
    cardText(translated("Tabelle", "Table"), line, nullptr, Tone::kNeutral);
    return;
  }
  view.layout = Layout::kTable;
  values::formatText(view.columns[0], sizeof(view.columns[0]), "%s",
                     translated("Pkt", "Pts"));
  values::formatText(view.columns[1], sizeof(view.columns[1]), "%s",
                     translated("Diff", "GD"));
  values::formatText(view.columns[2], sizeof(view.columns[2]), "%s",
                     translated("Sp", "P"));
  const int first = selection.first;
  view.row_count = count;
  for (int i = 0; i < count; ++i) {
    const auto& standing = store.row(content.table_rows[i]);
    auto& row = view.table[i];
    namePrefix<40>(row.name, football::view::rowName(store, standing));
    teamTarget(row.name, row.name_short, store, standing.team);
    value(row.position, sizeof(row.position),
          standing.position != football::kNoValue ? standing.position
                                                  : first + i + 1,
          true);
    value(row.played, sizeof(row.played), standing.played);
    value(row.points, sizeof(row.points), standing.points);
    if (standing.difference != football::kNoNumber)
      values::formatText(row.difference, sizeof(row.difference),
                         standing.difference > 0 ? "+%d" : "%d",
                         standing.difference);
    else
      values::formatText(row.difference, sizeof(row.difference), "--");
    row.own = (standing.flags & football::kRowOwn) != 0;
  }
  view.rim.kind = RimKind::kScroll;
  view.rim.first = static_cast<uint16_t>(first);
  view.rim.count = static_cast<uint16_t>(count);
  view.rim.total = static_cast<uint16_t>(table_rows);
}

void Presenter::bindMatchday(const football::MatchStore& store,
                             const football::view::Shown& shown,
                             const PreparedContent& content,
                             const BindContext& context) {
  const auto& selection = content.matchday;
  if (!selection.available || !content.matches.row_count) return;
  const int per_page = std::max<int>(1, policy.visible_matches);
  roundCaption(store, store.match(content.matches.rows[0]), true);
  view.layout = Layout::kConference;
  view.page = selection.page;
  view.pages = selection.pages;
  const int from = content.first;
  const int rows = content.matches.row_count;
  view.slots = selection.pages > 1 ? static_cast<uint8_t>(per_page)
                                   : static_cast<uint8_t>(rows);
  for (int row = 0; row < rows; ++row)
    matchRow(store, store.match(content.matches.rows[row]), shown, context,
             true, view.matches[row]);
  view.row_count = static_cast<uint8_t>(rows);
  view.rim.kind = RimKind::kScroll;
  view.rim.first = static_cast<uint16_t>(from);
  view.rim.count = static_cast<uint16_t>(rows);
  view.rim.total = content.total;
}

void Presenter::setStyle(screen::Type type) {
  const auto& value = policy.appearance[screen::index(type)];
  view.style = {value.background, value.colour,     value.text,
                value.accent,     value.text_scale, value.image};
}

uint32_t Presenter::bind(const BindContext& context) {
  const auto& data = *published;
  new (&view) ViewModel();
  presentation.no_badge = false;
  presentation.card_network = false;
  presentation.card_fault = 0;
  presentation.on_screen = nav.entry.competition;
  const auto now = context.now, utc = context.utc;
  const auto output = sequence::view(nav, navigation_data->input(), now);
  const auto type = output.empty ? screen::Type::kCrest : output.entry.type;
  view.screen = type;
  setStyle(type);
  const auto phase = navigation_data->selected.phase;
  const Reason reason =
      output.frame                                    ? Reason::kFrame
      : output.manual                                 ? Reason::kManual
      : !data.configured                              ? Reason::kSetup
      : phase == settings::screen_policy::Phase::kOwn ? Reason::kOwnMatch
      : phase == settings::screen_policy::Phase::kOtherGames
          ? Reason::kOtherGames
      : phase == settings::screen_policy::Phase::kMatchday ? Reason::kMatchday
      : phase == settings::screen_policy::Phase::kBreak    ? Reason::kBreak
                                                           : Reason::kIdle;
  presentation.screen_status =
      static_cast<uint32_t>(type) | static_cast<uint32_t>(reason) << 8;
  int faults = 0;
  firstFault(faults);
  if (presentation.starting) {
    const auto wifi = context.net->state;
    bool active = false;
    for (const auto& entry : navigation_data->data.competitions.entries)
      active |= entry.active;
    presentation.starting =
        active && !policy.frame && !model.demo && !context.ap &&
        data.fetched_ms == 0 && faults == 0 && !networkFault(context) &&
        wifi != network::State::kBackoff &&
        wifi != network::State::kBackoffAuth &&
        wifi != network::State::kWaitSsid && wifi != network::State::kApOnly;
    if (presentation.starting) {
      bindStart(!context.connected ? 0 : !context.valid ? 1 : 2);
      return 0;
    }
  }
  if (!policy.frame && !data.configured && context.ap) {
    const auto& net = *context.net;
    char ssid[48];
    values::formatText(ssid, sizeof(ssid), "\xC2\xBB %s", net.ap_ssid);
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
      values::formatText(view.card.title, sizeof(view.card.title),
                         translated("%s gestört", "%s failed"),
                         networkPhase(net.failure_step));
      values::formatText(
          view.card.hints[0], sizeof(view.card.hints[0]), "%s · %s",
          translated("IP im Browser öffnen", "Open IP in browser"),
          network::initStepCode(net.failure_step));
    }
  } else if (!policy.frame && !model.demo && data.fetched_ms == 0 &&
             networkFault(context)) {
    networkFaultCard(context);
  } else if (output.empty) {
    cardText(translated("Keine automatischen Inhalte", "No automatic content"),
             translated("Anzeige im Web prüfen", "Check Display in Web UI"),
             policy.frame ? translated("Bilder oder Wappen wählen",
                                       "Select images or crest")
                          : translated("Inhalte und Automatik prüfen",
                                       "Check content and automatic rules"),
             Tone::kNeutral);
  } else if (screen::sporting(type) &&
             content(nav.entry,
                     nav.paged ? sequence::PageMode::kManualHeld
                               : sequence::PageMode::kAutomatic,
                     nav.page, navigation_data)
                     .kind == sequence::ContentKind::kError) {
    const auto status =
        contentStatus(data.store, nav.entry.competition, nav.entry.type, utc);
    if (status.cause != football::Cause::kNone)
      faultCard(nav.entry.competition, status, now, utc);
    else if (!context.connected && !model.demo) {
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
      presentation.card_network = true;
    } else if (!context.valid)
      cardText(translated("Warte auf Uhrzeit", "Waiting for time"),
               translated("Zeitserver wird gefragt", "Asking the time server"),
               nullptr, Tone::kWarn);
    else
      cardText(translated("Keine Daten", "No data"),
               translated("Lade Daten", "Loading data"),
               translated("Im Web Anbieter prüfen", "Check provider in Web UI"),
               Tone::kNeutral);
  } else if (screen::sporting(type)) {
    const auto mode = nav.paged    ? sequence::PageMode::kManualHeld
                      : nav.manual ? sequence::PageMode::kManualAutomatic
                                   : sequence::PageMode::kAutomatic;
    const auto& prepared = navigation_data->prepare(nav.entry, mode, nav.page);
    if (type == screen::Type::kLiveSingle ||
        (type == screen::Type::kLiveMulti && prepared.total == 1)) {
      if (prepared.single)
        bindOwnMatch(data.store, *prepared.single, data.shown, context);
    } else if (type == screen::Type::kLiveMulti) {
      bindConference(data.store, data.shown, prepared, context);
    } else if (type == screen::Type::kMatchday) {
      bindMatchday(data.store, data.shown, prepared, context);
    } else if (type == screen::Type::kTable) {
      bindTable(data.store, prepared);
    }
  }
  view.page = output.page;
  view.pages = policy.appearance[screen::index(type)].dots ? output.pages : 0;
  return bindStatus(data, context);
}
}  // namespace ui
