FROM python:3.11-slim

WORKDIR /app

# qrcode: generates QR bitmaps for the OLED display
COPY room_server.py .
COPY schedule.json .

EXPOSE 8080

CMD ["python", "-u", "room_server.py"]
