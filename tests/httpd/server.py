#!/usr/bin/env python3
"""HTTP-сервер для проверок загрузчика.

Умеет не только отдавать файл, но и вести себя неправильно ровно теми
способами, на которых ломается докачка: ответить 200 на запрос с Range,
ответить 206 с чужим Content-Range, ответить 416. Без такого сервера эти ветки
проверить нечем, а именно они молча портят скачанный файл.

Порт выбирается свободный и печатается первой строкой в stdout, чтобы тест не
угадывал его и не конфликтовал с параллельными прогонами.

Пути:
  /ok/<name>         честная отдача с поддержкой Range
  /norange/<name>    Range игнорируется, всегда 200 и всё тело
  /badrange/<name>   206, но Content-Range начинается не там, где просили
  /416/<name>        всегда 416
  /slow/<name>       отдаёт по байту с задержкой (проверка застоя)
  /notfound          404 с HTML-телом
"""

import http.server
import os
import socketserver
import sys
import threading
import time

ROOT = sys.argv[1] if len(sys.argv) > 1 else "."


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *args):  # тишина: вывод теста и так плотный
        pass

    def _body(self, name):
        path = os.path.join(ROOT, os.path.basename(name))
        with open(path, "rb") as f:
            return f.read()

    def _range_start(self):
        header = self.headers.get("Range")
        if not header or not header.startswith("bytes="):
            return None
        try:
            return int(header[len("bytes="):].split("-")[0])
        except ValueError:
            return None

    def do_GET(self):  # noqa: N802
        parts = self.path.lstrip("/").split("/", 1)
        mode = parts[0]
        name = parts[1] if len(parts) > 1 else ""

        if mode == "notfound":
            body = b"<html><body>Not found</body></html>"
            self.send_response(404)
            self.send_header("Content-Type", "text/html")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return

        try:
            data = self._body(name)
        except OSError:
            self.send_response(404)
            self.send_header("Content-Length", "0")
            self.end_headers()
            return

        start = self._range_start()

        if mode == "416":
            self.send_response(416)
            self.send_header("Content-Range", f"bytes */{len(data)}")
            self.send_header("Content-Length", "0")
            self.end_headers()
            return

        if mode == "norange" or start is None:
            # Сервер, не умеющий Range: всегда полное тело с кодом 200.
            self.send_response(200)
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)
            return

        if mode == "badrange":
            # 206 есть, а тело начинается не оттуда, откуда обещано. Именно
            # этот случай и портит файл при слепом дописывании.
            wrong = 0
            chunk = data[wrong:]
            self.send_response(206)
            self.send_header("Content-Range", f"bytes {wrong}-{len(data) - 1}/{len(data)}")
            self.send_header("Content-Length", str(len(chunk)))
            self.end_headers()
            self.wfile.write(chunk)
            return

        if mode == "slow":
            self.send_response(200)
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            for byte in data:
                self.wfile.write(bytes([byte]))
                self.wfile.flush()
                time.sleep(0.5)
            return

        chunk = data[start:]
        self.send_response(206)
        self.send_header("Content-Range", f"bytes {start}-{len(data) - 1}/{len(data)}")
        self.send_header("Content-Length", str(len(chunk)))
        self.end_headers()
        self.wfile.write(chunk)


class Server(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True


def main():
    server = Server(("127.0.0.1", 0), Handler)
    print(server.server_address[1], flush=True)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    # Живём, пока жив родитель: он закрывает наш stdin при завершении.
    try:
        sys.stdin.read()
    except KeyboardInterrupt:
        pass
    server.shutdown()


if __name__ == "__main__":
    main()
