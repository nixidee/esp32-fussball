// Actual extracted production layout functions, glyph advances and SafeArea.
// This fixture emits no pixels and contains no replacement layout algorithm.
#include "layout.inc"

unsigned cases = 0;
const char* scenario = "";
void require(bool pass, const char* why) {
  if (!pass) {
    std::cerr << "FAIL " << scenario << ": " << why << '\n';
    std::exit(1);
  }
}
std::vector<std::string> run(ViewModel& m, void (*layout)(), int scale,
                             unsigned side = 240) {
  const geometry::SafeArea area(side, side, hw::DisplayShape::kRound,
                                side / cfg::kContentMarginDivisor);
  f = {};
  f.m = &m;
  f.area = &area;
  f.ok = true;
  f.s = side;
  f.g = std::max(2, int(side) / 60);
  f.cx = f.cy = side / 2;
  f.R = side / 2 - area.margin();
  const int size_class = side <= cfg::kSizeClassSmallMax    ? 0
                         : side <= cfg::kSizeClassMediumMax ? 1
                                                            : 2;
  f.cap = fontFor(size_class, 0, scale);
  f.body = fontFor(size_class, 1, scale);
  f.title = fontFor(size_class, 2, scale);
  item_count = arena_used = 0;
  overflow = false;
  layout();
  statusPill();
  if (!f.ok) {
    std::cerr << "geometry side=" << side << " scale=" << scale
              << " IP=" << unsigned(m.badge_lines)
              << " bottom=" << m.badge_bottom << " gap=" << f.g
              << " fonts=" << unsigned(f.cap) << ',' << unsigned(f.body) << ','
              << unsigned(f.title) << '\n';
    for (std::size_t i = 0; i < item_count; ++i) {
      const auto& item = items[i];
      if ((item.kind == Kind::kText || item.kind == Kind::kPill) &&
          !area.contains(
              {item.x1, item.y1, item.x2 - item.x1 + 1, item.y2 - item.y1 + 1}))
        std::cerr << "outside " << int(item.kind) << ' ' << item.x1 << ','
                  << item.y1 << ' ' << item.x2 << ',' << item.y2 << ' '
                  << (item.kind == Kind::kText ? arena + item.start : "")
                  << '\n';
    }
  }
  require(f.ok, "production layout reports geometry failure");
  require(!overflow, "draw/text capacity exceeded");
  std::vector<std::string> text;
  for (std::size_t i = 0; i < item_count; ++i) {
    const auto& item = items[i];
    if (item.kind == Kind::kText) {
      require(area.contains({item.x1, item.y1, item.x2 - item.x1 + 1,
                             item.y2 - item.y1 + 1}),
              "text outside SafeArea");
      text.emplace_back(arena + item.start, item.end);
      for (std::size_t j = 0; j < i; ++j) {
        const auto& other = items[j];
        if (other.kind != Kind::kText) continue;
        require(item.x1 > other.x2 || other.x1 > item.x2 ||
                    item.y1 > other.y2 || other.y1 > item.y2,
                "text bands overlap");
      }
    } else if (item.kind == Kind::kPill)
      require(area.contains({item.x1, item.y1, item.x2 - item.x1 + 1,
                             item.y2 - item.y1 + 1}),
              "padded pill outside SafeArea");
  }
  ++cases;
  return text;
}
void badge(ViewModel& m, unsigned lines, bool bottom) {
  m.badge_lines = lines;
  m.badge_bottom = bottom;
  std::strcpy(m.badge[0], "192.168.4.1");
  std::strcpy(m.badge[1], "Setup-Netz");
  m.status_pill = true;
  std::strcpy(m.status, "OLG E10 · Stand 14:05");
}
void tableCases(int scale, unsigned lines, bool bottom, unsigned side) {
  ViewModel m;
  m.setLayout(Layout::kTable);
  badge(m, lines, bottom);
  m.table.count = 9;
  std::strcpy(m.caption, "Gruppe B · Bundesliga");
  std::strcpy(m.table.columns[0], "Pkt");
  std::strcpy(m.table.columns[1], "Diff");
  std::strcpy(m.table.columns[2], "Sp");
  for (unsigned i = 0; i < m.table.count; ++i) {
    auto& r = m.table.rows[i];
    std::strcpy(r.name, "Borussia Mönchengladbach");
    std::strcpy(r.name_short, "BMG");
    std::snprintf(r.position, sizeof(r.position), "%u", i + 1);
    std::strcpy(r.points, "-10000");
    std::strcpy(r.difference, "+10000");
    std::strcpy(r.played, "254");
    r.own = i == 4;
  }
  scenario = "table all values/group/9rows";
  const auto text = run(m, table, scale, side);
  for (const char* value : {"-10000", "+10000", "254"})
    require(std::count(text.begin(), text.end(), value) == 9,
            "required numeric column lost/clipped");
  for (const char* title : {"Pkt", "Diff", "Sp"})
    require(std::count_if(text.begin(), text.end(),
                          [&](const auto& value) {
                            return value.find(title) != std::string::npos;
                          }) == 1,
            "column header lost/clipped");
}
void otherCases(int scale, unsigned lines, bool bottom, unsigned side) {
  ViewModel m;
  m.setLayout(Layout::kOwnMatch);
  badge(m, lines, bottom);
  std::strcpy(m.caption, "Pokal · Finale");
  std::strcpy(m.own.home, "Borussia Mönchengladbach");
  std::strcpy(m.own.away, "FC Bayern München");
  std::strcpy(m.own.home_short, "BMG");
  std::strcpy(m.own.away_short, "FCB");
  std::strcpy(m.own.centre, "12:11");
  std::strcpy(m.own.pill, "105+2 · Verlängerung");
  std::strcpy(m.own.highlight, "105+2 · Müller · Auswechslung");
  m.own.shootout.active = true;
  m.own.shootout.count = 10;
  std::strcpy(m.own.shootout.caption, "Versuche 1–10/18 · gemeldet");
  for (unsigned i = 0; i < 10; ++i)
    m.own.shootout.attempts[i] = i == 5  ? Attempt::kUnknown
                                 : i % 2 ? Attempt::kAwayScored
                                         : Attempt::kHomeMissed;
  scenario = "Single18attempts first window/added period";
  auto attempts = run(m, ownMatch, scale, side);
  require(std::count(attempts.begin(), attempts.end(), "+") == 4 &&
              std::count(attempts.begin(), attempts.end(), "x") == 5 &&
              std::count(attempts.begin(), attempts.end(), "?") == 2,
          "reported attempts/unknown-actor heading lost");
  m.own.shootout.count = 8;
  std::strcpy(m.own.shootout.caption, "Versuche 11–18/18 · unvollständig");
  scenario = "Single18attempts last window";
  attempts = run(m, ownMatch, scale, side);
  require(std::count(attempts.begin(), attempts.end(), "+") == 3 &&
              std::count(attempts.begin(), attempts.end(), "x") == 4 &&
              std::count(attempts.begin(), attempts.end(), "?") == 2,
          "last attempt window lost");
  m.own.shootout.count = 0;
  std::strcpy(m.own.shootout.caption, "Versuche nicht geliefert");
  scenario = "Single missing attempts";
  run(m, ownMatch, scale, side);
  m.own.shootout = {};
  scenario = "Single normal added time";
  run(m, ownMatch, scale, side);
  m.setLayout(Layout::kConference);
  m.pages = 511;
  m.page = 510;
  m.conference.slots = 5;
  m.conference.count = 4;
  for (auto& r : m.conference.rows) {
    std::strcpy(r.home, "Borussia Mönchengladbach");
    std::strcpy(r.home_short, "BMG");
    std::strcpy(r.away, "FC Bayern München");
    std::strcpy(r.away_short, "FCB");
    std::strcpy(r.centre, "Sa 15:30");
    r.own = true;
  }
  scenario = "Conference4 rows/manual page511";
  auto text = run(m, conference, scale, side);
  require(std::count(text.begin(), text.end(), "511 / 511") == 1,
          "exact large page position lost");
  m.conference.count = 5;
  scenario = "Conference growth/Matchday layout";
  run(m, conference, scale, side);
  m.setLayout(Layout::kCard);
  m.card.line_count = m.card.hint_count = 2;
  std::strcpy(m.card.title, "Providerfehler");
  for (auto& r : m.card.lines) std::strcpy(r, "Netzwerk prüfen · E10");
  for (auto& r : m.card.hints) std::strcpy(r, "Neuer Versuch in 30 s");
  scenario = "status/start cards";
  run(m, card, scale, side);
  m.setLayout(Layout::kImage);
  scenario = "Crest/Images overlays";
  run(m, crest, scale, side);
}
int main() {
  for (unsigned side : {240u, 466u})
    for (int scale : {100, 130})
      for (unsigned lines : {0u, 1u, 2u})
        for (bool bottom : {false, true}) {
          tableCases(scale, lines, bottom, side);
          otherCases(scale, lines, bottom, side);
        }
  std::cout << "PASS " << cases
            << " actual-layout geometry/string cases; actual glyph advances, "
               "no pixels/legibility claim\n";
}
