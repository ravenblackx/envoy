FROM jaegertracing/all-in-one@sha256:1feca128c5159db2489ea5c13ccbbeb7db3e7b00625371bbbe770391c69b15f4
HEALTHCHECK \
    --interval=1s \
    --timeout=1s \
    --start-period=1s \
    --retries=60 \
    CMD wget -q --header='Content-Type:application/json' -O - http://localhost:14269/health | grep "Server available"
