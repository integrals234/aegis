# Container images

`Dockerfile.dev` builds the development and CI environment described in
[docs/ENVIRONMENT.md](../docs/ENVIRONMENT.md).

```bash
docker build -f docker/Dockerfile.dev -t aegis-dev .
docker run --rm aegis-dev                     # runs scripts/ci_local.sh
docker run --rm -it aegis-dev bash            # interactive shell
```

**This image has not been built on the AEGIS development host.** Docker Desktop
WSL integration is disabled there, so no clean-machine transcript exists yet.
That limitation is recorded in [docs/LIMITATIONS.md](../docs/LIMITATIONS.md) and
AEGIS-009 carries a registered verification obligation until such a transcript —
from this image or from a CI runner — is committed.
