#!/bin/bash

# Display top 5 CPU-using Docker containers with stats

echo "======================================"
echo "   Top 5 Docker Containers by CPU"
echo "======================================"
echo ""

docker stats --no-stream --format "table {{.Container}}\t{{.CPUPerc}}\t{{.MemUsage}}" | \
  tail -n +2 | \
  sort -t'%' -k1 -rn | \
  head -5 | \
  awk '{
    printf "%-20s CPU: %-8s Memory: %s\n", $1, $2, $3
  }'

echo ""
echo "======================================"
