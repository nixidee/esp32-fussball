"""Compile explicitly selected pure components from their original sources."""

from pathlib import Path

Import("env")

project_root = Path(env.subst("$PROJECT_DIR"))
pure_components = {
    "events": "+<event_admission.cpp>",
    "files": "+<file_names.cpp> +<file_io.cpp> +<file_candidate.cpp>",
    "geometry": "+<*.cpp>",
    "settings": (
        "+<app_config_model.cpp> +<app_config_record.cpp> +<settings_model.cpp>"
        " +<settings_record.cpp> +<screen_policy.cpp> +<screen_record.cpp>"
        " +<ui_config.cpp> +<runtime_config.cpp>"
    ),
    "timekeeping": "+<civil_time.cpp>",
    "json": "+<json_stream.cpp>",
}

env.Append(CPPPATH=[str(project_root / "include")])
for component, source_filter in pure_components.items():
    component_root = project_root / "components" / component
    env.Append(CPPPATH=[str(component_root / "include")])
    env.BuildSources(
        str(Path(env.subst("$BUILD_DIR")) / "pure" / component),
        str(component_root),
        src_filter=source_filter,
    )

# Session table and pure JSON codecs of the Web component; the service needs IDF.
web_root = project_root / "components" / "web"
env.Append(CPPPATH=[str(web_root / "include"), str(web_root)])

# Pure health coordinator; the system adapters require IDF.
system_root = project_root / "components" / "system"
env.Append(CPPPATH=[str(system_root / "include")])
env.BuildSources(
    str(Path(env.subst("$BUILD_DIR")) / "pure" / "health"),
    str(system_root),
    src_filter="+<health_meter.cpp> +<operation_admission.cpp>",
)

# Pure football: shared types/codecs, match store, provider adapters,
# scheduler, plans and views;
# the HTTP client and service files require IDF.
football_root = project_root / "components" / "football"
env.Append(CPPPATH=[str(football_root / "include")])
names_root = project_root / "components" / "names"
env.Append(CPPPATH=[str(names_root / "include")])
env.BuildSources(
    str(Path(env.subst("$BUILD_DIR")) / "pure" / "names"),
    str(names_root),
    src_filter="+<team_name_store.cpp> +<team_name_files.cpp> +<effective_names.cpp>",
)
# The library finder adds independent JSON-oracle paths only to test sources.
libdeps = Path(env.subst("$PROJECT_LIBDEPS_DIR")) / env.subst("$PIOENV")
env.Append(CPPPATH=[str(libdeps / "ArduinoJson" / "src")])
env.BuildSources(
    str(Path(env.subst("$BUILD_DIR")) / "pure" / "web"),
    str(web_root),
    src_filter="+<screen_json.cpp> +<settings_json.cpp> +<command_json.cpp>",
)
env.BuildSources(
    str(Path(env.subst("$BUILD_DIR")) / "pure" / "football"),
    str(football_root),
    src_filter=(
        "+<football_model.cpp> +<source_index.cpp> +<fixture_retention.cpp> +<budget_ledger.cpp> +<match_store.cpp> +<store_mappers.cpp>"
        " +<request_scheduler.cpp> +<provider_plan.cpp> +<store_views.cpp>"
        " +<matchday.cpp> +<screen_selection.cpp>"
        " +<schedule_coverage.cpp> +<provider_adapter.cpp> +<provider_readers.cpp> +<provider_selection.cpp> +<provider_coverage.cpp>"
        " +<demo_scenarios.cpp>"
    ),
)

# Pure input state machine and role mapping, and the display's error texts;
# the rest of the UI needs LVGL.
ui_root = project_root / "components" / "ui"
env.Append(CPPPATH=[str(ui_root / "include")])
env.BuildSources(
    str(Path(env.subst("$BUILD_DIR")) / "pure" / "ui"),
    str(ui_root),
    src_filter=(
        "+<input_policy.cpp> +<cause_text.cpp> +<navigation_policy.cpp>"
        " +<screen_sequence.cpp>"
    ),
)

# Pure Wi-Fi connection policy; the network service needs IDF.
network_root = project_root / "components" / "network"
env.Append(CPPPATH=[str(network_root / "include")])
env.BuildSources(
    str(Path(env.subst("$BUILD_DIR")) / "pure" / "network"),
    str(network_root),
    src_filter="+<wifi_policy.cpp>",
)
