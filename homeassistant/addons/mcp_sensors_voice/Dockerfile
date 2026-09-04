FROM python:3.12-slim-bookworm
WORKDIR /app
COPY app/ /app/
RUN pip install --no-cache-dir -r requirements.txt
ENV BRIDGE_HOST=0.0.0.0
ENV BRIDGE_PORT=8765
EXPOSE 8765
CMD ["python", "-u", "bridge.py"]
