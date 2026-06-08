FROM python:3.11-slim

ENV PYTHONDONTWRITEBYTECODE=1
ENV PYTHONUNBUFFERED=1

RUN apt-get update && apt-get install -y --no-install-recommends \
    ca-certificates \
    ffmpeg \
    libopus0 \
    libopus-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY requirements.txt /app/requirements.txt
RUN pip install --no-cache-dir -r /app/requirements.txt

COPY voice_proxy.py /app/voice_proxy.py

EXPOSE 8080
CMD ["uvicorn", "voice_proxy:app", "--host", "0.0.0.0", "--port", "8080"]
