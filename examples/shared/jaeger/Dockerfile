FROM jaegertracing/all-in-one@sha256:ef1d0f21a763b93a1a34e85aba842948cf47e6eead668223088ac93a407a2c07
HEALTHCHECK \
    --interval=1s \
    --timeout=1s \
    --start-period=1s \
    --retries=60 \
    CMD wget -q --header='Content-Type:application/json' -O - http://localhost:14269/health | grep "Server available"
