# The archive service: the push endpoint, the ingest and the MCP server.
# Build from the repo root: docker build -f server/Dockerfile .
FROM python:3.12-slim
WORKDIR /app
COPY server/requirements.txt server/requirements.txt
RUN pip install --no-cache-dir -r server/requirements.txt
COPY tools/logdb.py tools/zlog.py tools/
COPY tools/mcp/server.py tools/mcp/
COPY firmware/src/readings.cpp firmware/src/
COPY server/app.py server/
ENV DONGLE_ARCHIVE=/data/archive DONGLE_DB=/data/db/dongle.db PYTHONUNBUFFERED=1
EXPOSE 8765
CMD ["python", "-m", "uvicorn", "server.app:app", "--host", "0.0.0.0", "--port", "8765"]
