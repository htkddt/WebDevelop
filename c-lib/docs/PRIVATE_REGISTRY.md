# Publishing build artifacts to a private registry

This doc describes how to build the C library artifacts (`.o`, `.a`, shared library) and **publish them to a private registry** so other teams or CI can download pre-built binaries instead of compiling from source.

## Free option: GitHub Actions + Releases (no paid registry)

On **GitHub Free** you can use **GitHub Actions** to build the package and **GitHub Releases** as the “private” (or public) registry — no Artifactory/S3 subscription needed.

- **Public repos:** Unlimited Actions minutes; release assets and storage are free.
- **Private repos:** 2,000 Actions minutes/month free; release assets count toward repo storage (generous free tier).

### How it works

1. **Workflow:** [`.github/workflows/package.yml`](../.github/workflows/package.yml)
   - On **push to `main`/`master`:** builds `make package`, uploads the tarball as an **Actions artifact** (90-day retention).
   - On **push tag `v*`** (e.g. `v1.0.0`): builds the package, creates a **GitHub Release**, and attaches the tarball as a release asset.

2. **Publishing a version**
   ```bash
   git tag v1.0.0
   git push origin v1.0.0
   ```
   The workflow runs, creates the release, and uploads e.g. `m4engine-1.0.0-linux-amd64.tar.gz`.

3. **Download URL (stable)**
   ```
   https://github.com/OWNER/REPO/releases/download/v1.0.0/m4engine-1.0.0-linux-amd64.tar.gz
   ```
   Use this in scripts, CI, or docs; no extra registry or auth for public repos.

4. **Optional: require login for private repos**  
   For a **private** repo, release assets are only downloadable with a token (e.g. `GITHUB_TOKEN` in another workflow, or a PAT). That gives you a simple “private registry” on the free tier.

### Summary

| What              | Free? | Notes                                      |
|-------------------|-------|--------------------------------------------|
| Actions artifact  | Yes   | 90-day retention; good for CI/PR builds   |
| GitHub Release    | Yes   | Permanent; use as “registry” for versions |
| Release asset URL | Yes   | Stable download URL per version/tag       |
| Private repo      | Yes*  | 2,000 min/month; releases need PAT to download |

---

## 1. What gets built

| Artifact | Make target | Description |
|----------|-------------|-------------|
| Object files | `make all` or `make lib` | `build/lib_*.o` (PIC) for the engine, tenant, dispatcher, storage, validate, ollama |
| Static library | `make lib-static` | `lib/libm4engine.a` — link into C/C++ apps |
| Shared library | `make lib` | `lib/libm4engine.dylib` (macOS) or `lib/libm4engine.so` (Linux) |
| Package tarball | `make package` | `dist/m4engine-<VERSION>-<OS>-<ARCH>.tar.gz` (include/, lib/, VERSION, BUILD_INFO) |

**Version** is read from `c-lib/include/engine.h` (`ENGINE_VERSION`). **OS/ARCH** are detected (e.g. `darwin-arm64`, `linux-amd64`).

## 2. Create the package

```bash
# Build shared + static lib and create tarball
make package

# With MongoDB support (same as app)
make package USE_MONGOC=1
```

Output example: `dist/m4engine-1.0.0-darwin-arm64.tar.gz`

**Contents of the tarball:**

```
m4engine-1.0.0-darwin-arm64/
  include/           # Public headers (*.h)
  lib/
    libm4engine.a    # Static library
    libm4engine.dylib # or .so on Linux
  VERSION            # 1.0.0
  BUILD_INFO         # OS=darwin ARCH=arm64 USE_MONGOC=0
```

To also ship **object files** (e.g. for custom linking), extend the Makefile `package` target to copy `build/lib_*.o` into the tarball.

## 3. Push to a private registry

Use one of the following patterns; replace `REGISTRY_URL` and auth with your private registry.

### A. Generic HTTP/HTTPS (Artifactory, Nexus, or simple upload API)

```bash
VERSION=1.0.0
OS_ARCH=darwin-arm64
TGZ="dist/m4engine-${VERSION}-${OS_ARCH}.tar.gz"

# Upload (example: Artifactory generic repo, or your internal API)
curl -f -u "USER:TOKEN" -X PUT \
  "https://REGISTRY_URL/artifactory/generic/m4engine/${VERSION}/m4engine-${VERSION}-${OS_ARCH}.tar.gz" \
  -T "$TGZ"
```

- **Artifactory:** [REST API – Deploy artifact](https://www.jfrog.com/confluence/display/JFROG/Artifactory+REST+API#ArtifactoryRESTAPI-DeployArtifact)
- **Nexus:** [REST API – Upload component](https://help.sonatype.com/repomanager3/rest-and-integration-api/components-api)

### B. AWS S3

```bash
VERSION=1.0.0
OS_ARCH=darwin-arm64
TGZ="dist/m4engine-${VERSION}-${OS_ARCH}.tar.gz"

aws s3 cp "$TGZ" "s3://YOUR_BUCKET/m4engine/${VERSION}/m4engine-${VERSION}-${OS_ARCH}.tar.gz" \
  --acl private
```

Consumers: `aws s3 cp s3://YOUR_BUCKET/m4engine/1.0.0/... .`

### C. GitHub Packages (generic container or custom)

For **generic file storage** you can use GitHub Releases or a custom artifact store. Example with **GitHub Release**:

```bash
# Create release and upload asset (requires gh CLI)
gh release create "v${VERSION}" "dist/m4engine-${VERSION}-${OS_ARCH}.tar.gz" --title "v${VERSION}"
```

- [GitHub Releases API](https://docs.github.com/en/rest/releases/releases)

### D. Docker private registry (image with built libs)

Ship the built library inside a Docker image and push to your private Docker registry.

**Dockerfile.example** (in repo or `deployments/`):

```dockerfile
FROM debian:bookworm-slim AS builder
RUN apt-get update && apt-get install -y build-essential libcurl4-openssl-dev && rm -rf /var/lib/apt/lists/*
WORKDIR /build
COPY . .
RUN make lib lib-static && make package

FROM debian:bookworm-slim
COPY --from=builder /build/dist/*.tar.gz /artifacts/
COPY --from=builder /build/lib /usr/local/lib
COPY --from=builder /build/c-lib/include /usr/local/include/m4engine
# Optional: set ldconfig for .so
RUN echo "/usr/local/lib" > /etc/ld.so.conf.d/m4engine.conf && ldconfig
```

Build and push:

```bash
docker build -f Dockerfile.example -t your-registry.example.com/m4engine:1.0.0 .
docker push your-registry.example.com/m4engine:1.0.0
```

Consumers can `docker pull` and copy artifacts from the image or link against `/usr/local/lib` and `/usr/local/include/m4engine` in a multi-stage build.

## 4. Consuming from the private registry

- **GitHub Releases (free):**  
  `curl -sL -o m4engine.tar.gz "https://github.com/OWNER/REPO/releases/download/v1.0.0/m4engine-1.0.0-linux-amd64.tar.gz"`  
  (use a PAT in the URL for private repos: `https://TOKEN@github.com/...` or use `Authorization` header.)  
  Then `tar xzf m4engine.tar.gz` and use `-I.../include -L.../lib -lm4engine`.
- **HTTP/Artifactory/Nexus:** Download the tarball (e.g. `curl -u USER:TOKEN -o m4engine.tar.gz REGISTRY_URL/...`), then `tar xzf m4engine.tar.gz` and use `-I.../include -L.../lib -lm4engine`.
- **S3:** `aws s3 cp s3://... .` then unpack.
- **Docker:** Use the image as build stage and copy `lib/` and `include/` into your app image or build tree.

## 5. Optional: include object files in the package

To publish **.o** files for maximum flexibility (e.g. custom LDFLAGS), add to the Makefile `package` target:

```make
# Add after copying lib/
	@mkdir -p $(DIST_DIR)/$(DIST_NAME)/build
	cp $(LIB_OBJECTS) $(DIST_DIR)/$(DIST_NAME)/build/
```

Then the tarball will also contain `build/lib_engine.o`, `lib_tenant.o`, etc., and consumers can link them with their own flags.
