FROM debian:bookworm-slim
RUN apt-get update && apt-get install -y --no-install-recommends \
      python3 python3-serial python3-usb fastboot \
    && rm -rf /var/lib/apt/lists/*
COPY . /image
ENTRYPOINT ["python3", "/image/boot.py"]
