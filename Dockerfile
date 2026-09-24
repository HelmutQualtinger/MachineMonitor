FROM alpine:3.20 AS build

WORKDIR /src
RUN apk add --no-cache gcc musl-dev
COPY app.c .
RUN gcc -O2 -Wall -o machinemonitor app.c -lpthread

FROM alpine:3.20

WORKDIR /app

COPY --from=build /src/machinemonitor .
COPY static ./static

EXPOSE 8000

CMD ["./machinemonitor"]
