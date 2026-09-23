FROM alpine:3.20 AS build

WORKDIR /src
RUN apk add --no-cache gcc musl-dev
COPY app.c .
RUN gcc -O2 -Wall -o machinemonitor app.c -lpthread

FROM alpine:3.20

WORKDIR /app

# docker CLI (nur Client, kein Daemon) wird fuer "docker stats" (Container-Panel) benoetigt.
# Taken from Docker's official "docker:cli" image (itself Alpine-based) rather than
# apk's own docker-cli package, to keep the client version reliably in sync with
# whatever daemon it's likely talking to.
COPY --from=docker:cli /usr/local/bin/docker /usr/local/bin/docker

COPY --from=build /src/machinemonitor .
COPY static ./static

EXPOSE 8000

CMD ["./machinemonitor"]
