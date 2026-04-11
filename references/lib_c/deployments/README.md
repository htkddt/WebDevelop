# Docker deployments — Redis + ELK

Run Redis and the ELK stack (Elasticsearch + Kibana) for the M4-Hardcore AI Engine.

## Prerequisites

- Docker and Docker Compose v2 (`docker compose`).
- **Apple Silicon (M4):** Elasticsearch and Kibana are set to `platform: linux/amd64` so they run under emulation and avoid a known JVM SIGILL on arm64. First start may be slower.
- **Linux:** Increase vm.max_map_count for Elasticsearch:
  ```bash
  sudo sysctl -w vm.max_map_count=262144
  ```
  To make it permanent: add `vm.max_map_count=262144` to `/etc/sysctl.conf`.

## Start

From the project root:

```bash
docker compose -f deployments/docker-compose.yml up -d
```

Or from `deployments/`:

```bash
cd deployments && docker compose up -d
```

## Endpoints

| Service        | URL                    | Purpose                    |
|----------------|------------------------|----------------------------|
| Redis          | `127.0.0.1:6379`       | Cache, counters (Hiredis)  |
| Elasticsearch  | http://localhost:9200  | Index, search, ingest      |
| Kibana         | http://localhost:5601  | Dashboards, Dev Tools      |

## Stop

```bash
docker compose -f deployments/docker-compose.yml down
```

To remove data volumes as well:

```bash
docker compose -f deployments/docker-compose.yml down -v
```

If Elasticsearch previously failed to start (unhealthy), remove its volume and try again:

```bash
docker compose -f deployments/docker-compose.yml down -v
docker compose -f deployments/docker-compose.yml up -d
```
First ES startup can take 1–2 minutes before the healthcheck passes.

## Health

- Redis: `redis-cli -h 127.0.0.1 -p 6379 ping` → `PONG`
- Elasticsearch: `curl -s http://localhost:9200/_cluster/health?pretty`
- Kibana: open http://localhost:5601

## Kibana “Stack Monitoring” — node “not monitored” / “Monitor with Metricbeat”

**This is expected** with this compose file.

- **Stack Monitoring** in Kibana is a **separate** feature: it needs **Metricbeat** (or **Elastic Agent**) to collect **node-level** stats (CPU, JVM heap, disk, etc.) and write them into monitoring indices.
- This repo’s `docker-compose.yml` only runs **Elasticsearch + Kibana**. It does **not** run Metricbeat, so Kibana correctly reports **“The following nodes are not monitored”** and shows **`172.19.0.2:9300`** (the node’s **internal transport** address on the Docker network). That is **not** the same as the cluster being down.
- If `curl http://127.0.0.1:9200/_cluster/health` is **green** or **yellow** with one node, Elasticsearch is **up** for normal use (ingest, search, Discover, Dev Tools).

**What to do for local M4 dev:** use **Discover**, **Dev Tools**, and **`_cluster/health`** — ignore Stack Monitoring unless you deliberately add beats.

**If you want that screen populated:** add a Metricbeat (or Agent) container and follow Elastic’s docs for your stack version, e.g. [Monitor Elasticsearch with Metricbeat](https://www.elastic.co/guide/en/beats/metricbeat/current/metricbeat-module-elasticsearch.html) (8.x). Not required for c-lib ELK ingest testing.

## Integration test (c-lib bulk → Elasticsearch)

With **MongoDB** running and **c-lib** built using **`USE_MONGOC=1`**, consumer-side tests can verify **SharedCollection cold backfill**. See **[docs/TUTORIAL_BINDINGS.md](../docs/TUTORIAL_BINDINGS.md)** for build and integration. Diagnostics and env overrides: **`.cursor/elk.md`**.

## Security (production)

For production, enable Elasticsearch security and set passwords (e.g. `ELASTIC_PASSWORD`, `xpack.security.enabled=true`) and consider Redis `requirepass` and non-default ports.
