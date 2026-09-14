#!/usr/bin/env sh
set -eu

if ! command -v docker >/dev/null 2>&1; then
    echo "Docker is required: https://docs.docker.com/engine/install/" >&2
    exit 1
fi

if [ ! -f .env ]; then
    if [ -n "${AI_PASSPORT_UPLOAD_TOKEN:-}" ]; then
        token="$AI_PASSPORT_UPLOAD_TOKEN"
    elif command -v openssl >/dev/null 2>&1; then
        token="$(openssl rand -hex 24)"
    else
        token="$(od -An -N24 -tx1 /dev/urandom | tr -d ' \n')"
    fi
    umask 077
    printf 'AI_PASSPORT_UPLOAD_TOKEN=%s\nAI_PASSPORT_PORT=%s\nAI_PASSPORT_MAX_UPLOAD_BYTES=67108864\n' \
        "$token" "${AI_PASSPORT_PORT:-8080}" > .env
fi

docker compose up -d --build
echo "Server started. Copy AI_PASSPORT_UPLOAD_TOKEN from .env into the badge settings page."
echo "Health check: http://SERVER_IP:${AI_PASSPORT_PORT:-8080}/health"
