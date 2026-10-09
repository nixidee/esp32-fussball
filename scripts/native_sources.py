"""Compile explicitly selected pure components from their original sources."""

from pathlib import Path

Import("env")

project_root = Path(env.subst("$PROJECT_DIR"))
pure_components = ("events", "files", "geometry", "settings")

env.Append(CPPPATH=[str(project_root / "include")])
for component in pure_components:
    component_root = project_root / "components" / component
    env.Append(CPPPATH=[str(component_root / "include")])
    env.BuildSources(
        str(Path(env.subst("$BUILD_DIR")) / "pure" / component),
        str(component_root),
    )
