FROM jaegertracing/all-in-one@sha256:8ee62b0195c2c54764562326ae7c234d6e8bd0b2468e4de5203213f3ff72d144
HEALTHCHECK \
    --interval=1s \
    --timeout=1s \
    --start-period=1s \
    --retries=60 \
    CMD wget -q --header='Content-Type:application/json' -O - http://localhost:14269/health | grep "Server available"
