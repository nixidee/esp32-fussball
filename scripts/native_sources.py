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

# Pure coordinator in the core service; the remaining core files require IDF.
core_root = project_root / "components" / "core"
env.Append(CPPPATH=[str(core_root / "include")])
env.BuildSources(
    str(Path(env.subst("$BUILD_DIR")) / "pure" / "health"),
    str(core_root),
    src_filter="+<health_meter.cpp>",
)
