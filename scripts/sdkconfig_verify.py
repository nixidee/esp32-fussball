"""Verify the generated sdkconfig.<env> after ESP-IDF configuration.

Runs after the ESP-IDF builder (post: script); the check itself is defined in
sdkconfig_guard.py so both phases share one implementation.
"""

Import("env")

env.VerifySdkconfig()
