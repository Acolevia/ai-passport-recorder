FROM python:3.12-alpine

WORKDIR /app
COPY server.py /app/server.py

ENV AI_PASSPORT_DATA_DIR=/data \
    AI_PASSPORT_PORT=8080 \
    PYTHONDONTWRITEBYTECODE=1 \
    PYTHONUNBUFFERED=1

VOLUME ["/data"]
EXPOSE 8080
HEALTHCHECK --interval=30s --timeout=3s --start-period=5s --retries=3 \
  CMD wget -q -O - http://127.0.0.1:8080/health >/dev/null || exit 1

CMD ["python", "/app/server.py"]
