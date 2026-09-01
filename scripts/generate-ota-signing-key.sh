#!/usr/bin/env sh
set -eu

key_path="keys/ota_signing_key.pem"
if [ -e "$key_path" ]; then
  echo "Refusing to replace existing $key_path"
  exit 1
fi

mkdir -p keys
pio pkg exec --package tool-esptoolpy -- espsecure.py generate_signing_key \
  --version 2 "$key_path"
chmod 600 "$key_path"
echo "Created $key_path. Back it up securely; losing it prevents future OTA updates."
