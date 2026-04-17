# Operations Guide

## Starting

```bash
./vertexsdr
./vertexsdr /etc/websdr.cfg
```

Band test mode (generates band label PNG, no server):

```bash
./vertexsdr --test-band <center_khz> <bw_khz> <output.png>
```

Background:

```bash
nohup ./vertexsdr > /var/log/vertexsdr.log 2>&1 &
```

## Runtime Endpoints

Browser UI: `/`, `/index.html`, `/m.html`, `/sysop.html`

Audio/waterfall: `/~~stream`, `/~~waterstream` (WebSocket)

Status: `/~~status`, `/~~otherstable`, `/~~othersj`, `/~~histogram`, `/~~orgstatus`, `/~~fetchdx`

Logbook/chat: `/~~logbook`, `/~~loginsert`, `/~~chat`, `/~~chatidentities`, `/~~chatcensor`

Admin: `/~~configreload`, `/~~setconfig`, `/~~setdir`, `/~~waterfalltext`, `/~~blockmee`

Raw: `/~~raw`, `/~~iqbalance`

## Config Reload

`/~~configreload?how=1` - quiet reload
`/~~configreload?how=2` - reload + force browser refresh
`/~~configreload?how=3` - shutdown

You can pass `cfg=newpath.cfg` to switch config files during reload.

## Signals

- `SIGHUP`: config reload
- `SIGINT`: quit
- `SIGTERM`: quit
- `SIGPIPE`: ignored

## When to Restart Instead of Reload

Restart if you changed:
- Device type or path
- Sample rate
- IQ/non-IQ mode
- FFT backend build variant

## Systemd Setup

Create `/etc/systemd/system/vertexsdr.service`:

```ini
[Unit]
Description=VertexSDR WebSDR Server
After=network.target

[Service]
Type=simple
User=websdr
Group=websdr
WorkingDirectory=/home/websdr
ExecStart=/usr/local/bin/vertexsdr /etc/vertexsdr.cfg
Restart=on-failure
RestartSec=10
StandardOutput=journal
StandardError=journal

[Install]
WantedBy=multi-user.target
```

Then:

```bash
sudo systemctl daemon-reload
sudo systemctl enable vertexsdr
sudo systemctl start vertexsdr
sudo journalctl -u vertexsdr -f
```

## Monitoring

```bash
curl http://localhost:8901/~~status
curl http://localhost:8901/~~otherstable
top -p $(pidof vertexsdr)
```

## Support Development

https://www.paypal.com/paypalme/magicint1337

## License

LGPL 3.0. See COPYING.
