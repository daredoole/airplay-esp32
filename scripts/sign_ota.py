"""PlatformIO post-build signing for ESP-IDF's verify-on-OTA mode.

PlatformIO's ESP-IDF integration creates firmware.bin but does not run the
native IDF signing target. Sign that final application image in place so USB
uploads and artifacts handed to the WebUI contain the RSA v2 signature block.
"""

Import("env")

import os
import subprocess
import sys


def sign_firmware(source, target, env):
    project_dir = env.subst("$PROJECT_DIR")
    firmware = env.subst("$BUILD_DIR/${PROGNAME}.bin")
    signed = firmware + ".signed"
    key = os.path.join(project_dir, "keys", "ota_signing_key.pem")
    tool_dir = env.PioPlatform().get_package_dir("tool-esptoolpy")
    espsecure = os.path.join(tool_dir, "espsecure.py")

    if not os.path.isfile(key):
        raise RuntimeError(
            "Missing OTA signing key; run scripts/generate-ota-signing-key.sh"
        )

    subprocess.check_call(
        [
            sys.executable,
            espsecure,
            "sign_data",
            "--version",
            "2",
            "--keyfile",
            key,
            "--output",
            signed,
            firmware,
        ]
    )
    os.replace(signed, firmware)
    print("Signed firmware.bin for verify-on-OTA mode")


env.AddPostAction("$BUILD_DIR/${PROGNAME}.bin", sign_firmware)
