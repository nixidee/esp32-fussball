#!/usr/bin/env python3
"""Run lifecycle bodies and whole OTA/portal-DNS owners against SDK faults."""
from pathlib import Path
import argparse
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def function(source, signature):
    """Copy an original function; only mask comments/strings for brace matching."""
    masked = re.sub(r'//[^\n]*|/\*[\s\S]*?\*/|"(?:\\.|[^"\\])*"',
                    lambda match: " " * len(match[0]), source)
    start = source.index(signature)
    brace = masked.index("{", start)
    depth, end = 1, brace + 1
    while depth:
        depth += (masked[end] == "{") - (masked[end] == "}")
        end += 1
    return source[start:end]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--main-source", type=Path, default=ROOT / "src/main.cpp",
                        help="optional preserved main.cpp for a failure reproduction")
    args = parser.parse_args()
    entry = args.main_source.read_text()
    web = (ROOT / "components/web/web_service.cpp").read_text()
    network = (ROOT / "components/network/network_service.cpp").read_text()
    routes = re.search(r"constexpr Route kRoutes\[\] = \{[\s\S]*?\n\};", web)[0]
    handlers = set(re.findall(r'\{"[^\"]+", HTTP_\w+, (\w+), (\w+)\}', routes))
    handlers = sorted({name for pair in handlers for name in pair if name != "nullptr"})
    with tempfile.TemporaryDirectory(prefix="runtime-readiness-") as temporary:
        build = Path(temporary)
        sdk = ROOT / "scripts/tests/fixtures/runtime_readiness_sdk.h"
        for header in ("esp_err.h", "esp_app_desc.h", "esp_app_format.h",
                       "esp_image_format.h", "esp_ota_ops.h", "esp_system.h",
                       "esp_timer.h", "esp_log.h"):
            (build / header).write_text('#pragma once\n#include "runtime_readiness_sdk.h"\n')
        (build / "hw_target.h").write_text('#pragma once\n#include "targets/xiao_esp32c6_gc9a01.h"\n')
        (build / "sdkconfig.h").write_text('#pragma once\n#define CONFIG_IDF_TARGET_ESP32C6 1\n')
        main_parts = [function(entry, name) for name in (
            "void onSettingsChanged(", "void runDiagnosticLoop()", 'extern "C" void app_main()')]
        # Timing budgets now come from the production app_config.h included by
        # the fixture; do not reconstruct retired local copies from main.cpp.
        (build / "runtime_main.inc").write_text("\n".join(main_parts))
        lifecycle = [function(web, signature) for signature in (
            "esp_err_t init()", "bool healthy()", "void poll()")]
        declarations = "\n".join("esp_err_t " + name + "(httpd_req_t*) {return ESP_OK;}"
                                  for name in handlers)
        (build / "runtime_web.inc").write_text(
            declarations + "\n" + routes + "\n" + "\n".join(lifecycle))
        (build / "runtime_mdns.inc").write_text(function(network, "esp_err_t applyHttpService()"))
        (build / "lwip").mkdir()
        (build / "lwip/sockets.h").write_text('#include "portal_dns_sdk.h"\n')
        (build / "freertos").mkdir()
        for header in ("FreeRTOS.h", "task.h"):
            (build / "freertos" / header).write_text('#include "portal_dns_task_sdk.h"\n')
        command = ["clang++", "-std=c++20", "-g", "-O1",
                   "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                   "-DFUSSBALL_TARGET_XIAO_ESP32C6_GC9A01",
                   "-I" + str(build), "-I" + str(sdk.parent),
                   "-I" + str(ROOT / "include"), "-iquote", str(ROOT)]
        command += ["-I" + str(path) for path in (ROOT / "components").glob("*/include")]
        command += [str(sdk.parent / "runtime_readiness_host.cpp"), "-o", str(build / "check")]
        subprocess.run(command, check=True)
        cases = ("ota_headers", "success", "network", "web", "files", "display", "ui", "watchdog",
                 "http_start", "http_route", "http_404", "http_405", "http_debug", "http_stop")
        for case in cases:
            subprocess.run([str(build / "check"), case], check=True)
        network_command = command[:-3] + ["-I" + str(ROOT / "components/network"),
                                          str(ROOT / "components/network/portal_dns.cpp"),
                                          str(sdk.parent / "runtime_network_host.cpp"),
                                          "-o", str(build / "network_check")]
        subprocess.run(network_command, check=True)
        network_cases = ("mdns", "dns_invalid", "dns_valid", "dns_receive", "dns_send",
                         "dns_open", "dns_bind", "dns_recv_option", "dns_send_option",
                         "dns_short_send", "dns_timeout", "dns_interrupt",
                         "dns_send_interrupt", "dns_disabled", "dns_non_a", "dns_bad_label",
                         "dns_task", "dns_runtime_open")
        for case in network_cases:
            subprocess.run([str(build / "network_check"), case], check=True)
    print(f"PASS {len(cases) + len(network_cases)} production main/HTTP/OTA/mDNS/DNS scenarios (ASan/UBSan)")


if __name__ == "__main__":
    main()
