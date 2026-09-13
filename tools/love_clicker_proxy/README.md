# Love Clicker proxy

Плата — DHCP-сервер `192.168.11.1/24`, раздаёт адреса PC (пул `.100–.119`).

## PC

1. Ethernet: «Получить IP автоматически» (DHCP).
2. Не подключать плату к LAN, где уже есть другой DHCP (роутер).
3. `pip install -r requirements.txt`
4. Положить `privatekey-1122907.pem` рядом с `proxy.py` (вручную на каждый хост).
5. `python proxy.py` — прокси + mDNS + SSH-туннель на VPS.

Открыть: http://195.209.218.245:8080/

Без туннеля: `python proxy.py --no-ssh`
