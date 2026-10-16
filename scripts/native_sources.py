"""Compile explicitly selected pure components from their original sources."""

from pathlib import Path

Import("env")

project_root = Path(env.subst("$PROJECT_DIR"))
pure_components = ("events", "files", "geometry", "settings", "timekeeping")

env.Append(CPPPATH=[str(project_root / "include")])
for component in pure_components:
    component_root = project_root / "components" / component
    env.Append(CPPPATH=[str(component_root / "include")])
    env.BuildSources(
        str(Path(env.subst("$BUILD_DIR")) / "pure" / component),
        str(component_root),
    )

# Header-only session table of the Web component; its other sources need IDF.
env.Append(CPPPATH=[str(project_root / "components" / "web" / "include")])

# Pure coordinator in the core service; the remaining core files require IDF.
core_root = project_root / "components" / "core"
env.Append(CPPPATH=[str(core_root / "include")])
env.BuildSources(
    str(Path(env.subst("$BUILD_DIR")) / "pure" / "health"),
    str(core_root),
    src_filter="+<health_meter.cpp>",
)

# Pure football core: shared types and parsers, match store and provider
# mappers (ArduinoJson from lib_deps), scheduler, plans, views and files;
# the HTTP client and service files require IDF.
football_root = project_root / "components" / "football"
env.Append(CPPPATH=[str(football_root / "include")])
# The library finder adds lib_deps paths only to the test sources.
libdeps = Path(env.subst("$PROJECT_LIBDEPS_DIR")) / env.subst("$PIOENV")
env.Append(CPPPATH=[str(libdeps / "ArduinoJson" / "src")])
env.BuildSources(
    str(Path(env.subst("$BUILD_DIR")) / "pure" / "football"),
    str(football_root),
    src_filter=(
        "+<football_model.cpp> +<budget_ledger.cpp> +<match_store.cpp> +<store_mappers.cpp>"
        " +<request_scheduler.cpp> +<provider_plan.cpp> +<store_views.cpp>"
        " +<football_files.cpp> +<matchday.cpp>"
    ),
)

# Pure input state machine and role mapping, and the display's error texts;
# the rest of the UI needs LVGL.
ui_root = project_root / "components" / "ui"
env.Append(CPPPATH=[str(ui_root / "include")])
env.BuildSources(
    str(Path(env.subst("$BUILD_DIR")) / "pure" / "ui"),
    str(ui_root),
    src_filter="+<input_policy.cpp> +<cause_text.cpp> +<navigation_policy.cpp>",
)

# Pure Wi-Fi connection policy; the network service needs IDF.
network_root = project_root / "components" / "network"
env.Append(CPPPATH=[str(network_root / "include")])
env.BuildSources(
    str(Path(env.subst("$BUILD_DIR")) / "pure" / "network"),
    str(network_root),
    src_filter="+<wifi_policy.cpp>",
)
