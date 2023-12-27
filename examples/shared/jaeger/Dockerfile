FROM jaegertracing/all-in-one@sha256:92874cf488550b825e573d1af903490bf8c0e3771bcbb42f423f9c8f77af0d71
HEALTHCHECK \
    --interval=1s \
    --timeout=1s \
    --start-period=1s \
    --retries=60 \
    CMD wget -q --header='Content-Type:application/json' -O - http://localhost:14269/health | grep "Server available"
