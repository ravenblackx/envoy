FROM python:3.11.0b5-slim-bullseye@sha256:c0b8b2d5a8de66d2a4e77c1280a4a204cc139ce5899ff127616b34b50a93fc00

ADD requirements.txt /tmp/flask-requirements.txt
RUN pip3 install -qr /tmp/flask-requirements.txt
RUN mkdir /code

ENTRYPOINT ["python3", "/code/service.py"]
