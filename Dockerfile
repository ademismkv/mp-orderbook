# Zero-setup way to run quickstart.sh: same toolchain every time, no local
# g++ install needed. Doesn't give you a quieter CPU — it still shares
# whatever cores your machine has with everything else running on it — but
# it does remove "what g++ version do you have" as a variable entirely.
#
# Build:  docker build -t mp-orderbook .
# Run:    docker run --rm mp-orderbook
#
# ubuntu:22.04 ships g++ 11, the exact version this codebase has actually
# been built and tested with (see devlog) — not a guess at compatibility.
FROM ubuntu:22.04

RUN apt-get update && \
    apt-get install -y --no-install-recommends g++ && \
    rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY . .
RUN chmod +x quickstart.sh

CMD ["./quickstart.sh"]
