"""Generate the Arduino 3.3.10 TLS implementation without enabling PSK.

Arduino-ESP32 3.3.10 accidentally wraps its entire ssl_client.cpp in a PSK
feature guard. ObjectLink does not use PSK, so enabling unused cipher suites is
the wrong product workaround. The exact upstream hash and marker counts make
this configure-time compatibility patch fail closed on any dependency drift.
"""

from __future__ import annotations

import hashlib
import pathlib
import sys


EXPECTED_SHA256 = "85755cb020d9671a4bf2e254c2be33e5506c93db65a921f7554f59ab0a4afc78"
OUTER_GUARD = "#if !defined(MBEDTLS_KEY_EXCHANGE__SOME__PSK_ENABLED)"
IMPLEMENTATION_START = 'const char *pers = "esp32-tls";'
PSK_BRANCH = "  } else if (pskIdent != NULL && psKey != NULL) {"
PSK_TAIL = """  } else {
    return -1;
  }

  // Note - this check for BOTH key and cert is relied on"""


def fail(message: str) -> None:
    raise SystemExit(f"Arduino TLS compatibility patch failed: {message}")


def main() -> None:
    if len(sys.argv) != 3:
        fail("expected input and output paths")
    source = pathlib.Path(sys.argv[1])
    output = pathlib.Path(sys.argv[2])
    raw = source.read_bytes()
    if hashlib.sha256(raw).hexdigest() != EXPECTED_SHA256:
        fail("unexpected ssl_client.cpp SHA-256; audit the new Arduino version")

    text = raw.decode("utf-8").replace("\r\n", "\n")
    guard_at = text.find(OUTER_GUARD)
    body_at = text.find(IMPLEMENTATION_START)
    if guard_at < 0 or body_at < 0 or body_at <= guard_at:
        fail("outer PSK guard markers are missing")
    text = text[:guard_at] + text[body_at:]
    if not text.rstrip().endswith("#endif"):
        fail("outer PSK guard terminator is missing")
    text = text.rstrip()
    text = text[: text.rfind("#endif")].rstrip() + "\n"

    if text.count(PSK_BRANCH) != 1 or text.count(PSK_TAIL) != 1:
        fail("PSK branch markers changed")
    text = text.replace(
        PSK_BRANCH,
        """  }
#if defined(MBEDTLS_KEY_EXCHANGE__SOME__PSK_ENABLED) || \\
    defined(MBEDTLS_KEY_EXCHANGE_SOME_PSK_ENABLED)
  else if (pskIdent != NULL && psKey != NULL) {""",
    )
    text = text.replace(
        PSK_TAIL,
        """  }
#endif
  else {
    return -1;
  }

  // Note - this check for BOTH key and cert is relied on""",
    )
    output.parent.mkdir(parents=True, exist_ok=True)
    text += (
        '\nextern "C" void hampter_arduino_ssl_compat_anchor() {}\n'
    )
    output.write_text(text, encoding="utf-8", newline="\n")


if __name__ == "__main__":
    main()
