# iroh's relay, at the version src/p2p links (Cargo.lock), in development
# mode: plain HTTP on 3340, no TLS, no rate limits. For trying devices that
# reach each other only through a relay; never expose it.
FROM rust:1-slim-bookworm AS build
ARG IROH_RELAY_VERSION=1.3.0
RUN cargo install iroh-relay --version "${IROH_RELAY_VERSION}" --features server --locked

FROM debian:bookworm-slim
RUN apt-get update && apt-get install -y --no-install-recommends ca-certificates \
    && rm -rf /var/lib/apt/lists/*
COPY --from=build /usr/local/cargo/bin/iroh-relay /usr/local/bin/iroh-relay
EXPOSE 3340
ENTRYPOINT ["iroh-relay", "--dev"]
