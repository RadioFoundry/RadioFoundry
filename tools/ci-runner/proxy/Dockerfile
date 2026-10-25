# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
#
# The runner's egress proxy (../README.md): tinyproxy as an unprivileged user, allowing HTTPS (CONNECT to port 443)
# only to the hosts in `allow`. The runner's network has no route out; this container is its only way to the
# internet, and it resolves the names, so the runner needs no DNS.
#
#   docker build -t rfoundry/ci-proxy:latest tools/ci-runner/proxy
ARG BASE=alpine:3.20
FROM ${BASE}
RUN apk add --no-cache tinyproxy
COPY tinyproxy.conf allow /etc/tinyproxy/
USER nobody
EXPOSE 8888
ENTRYPOINT ["tinyproxy", "-d", "-c", "/etc/tinyproxy/tinyproxy.conf"]
