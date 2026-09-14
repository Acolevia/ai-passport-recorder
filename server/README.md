# Folo Recorder upload server

The server accepts authenticated, resumable `.FRC` uploads and stores completed files under
`/data/<device-id>/`. A `.part` file is never presented as a completed recording.

Start it from the repository root:

```sh
FOLO_UPLOAD_TOKEN="replace-with-a-long-random-token" docker compose up -d --build
```

Check health:

```sh
curl http://localhost:8080/health
```

Check the authenticated endpoint used by the recorder:

```sh
curl -H "Authorization: Bearer $FOLO_UPLOAD_TOKEN" \
  http://localhost:8080/api/v1/status
```

List recordings:

```sh
curl -H "Authorization: Bearer $FOLO_UPLOAD_TOKEN" \
  http://localhost:8080/api/v1/recordings
```

The data volume is named `folo-recorder-c3_folo-recordings` by default. Back up that Docker
volume as part of normal server backups. Put a TLS reverse proxy in front of the service when
it is reachable outside a trusted LAN.
