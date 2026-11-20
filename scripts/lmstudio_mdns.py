"""Publish an IPv4 LM Studio hostname on one Windows LAN adapter."""
import argparse
import ipaddress
import json
import logging
from logging.handlers import RotatingFileHandler
from pathlib import Path
import re
import signal
import socket
import time

import ifaddr
from zeroconf import IPVersion, ServiceInfo, Zeroconf


def adapter_addresses(interface):
    addresses = set()
    for adapter in ifaddr.get_adapters():
        if adapter.nice_name != interface:
            continue
        for entry in adapter.ips:
            if isinstance(entry.ip, str):
                address = ipaddress.ip_address(entry.ip)
                if address.version == 4 and not (address.is_loopback or address.is_link_local or address.is_unspecified):
                    addresses.add(str(address))
    return sorted(addresses)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--interface', required=True, help='Windows adapter description')
    parser.add_argument('--hostname', default='lmstudio')
    parser.add_argument('--port', type=int, default=1234)
    parser.add_argument('--state-dir', required=True, type=Path)
    args = parser.parse_args()
    if not re.fullmatch(r'[a-zA-Z0-9](?:[a-zA-Z0-9-]{0,61}[a-zA-Z0-9])?', args.hostname):
        parser.error('hostname must be one DNS label, without .local')
    if not 1 <= args.port <= 65535:
        parser.error('port must be in 1..65535')
    args.state_dir.mkdir(parents=True, exist_ok=True)
    logging.basicConfig(level=logging.INFO, format='%(asctime)s %(levelname)s %(message)s', handlers=[
        RotatingFileHandler(args.state_dir / 'publisher.log', maxBytes=1024 * 1024, backupCount=2, encoding='utf-8')])
    stop = args.state_dir / 'stop'
    status = args.state_dir / 'status.json'
    running = True

    def shutdown(*_):
        nonlocal running
        running = False

    for name in ('SIGINT', 'SIGTERM'):
        signal.signal(getattr(signal, name), shutdown)

    def write_status(state, addresses, error=None):
        temporary = status.with_suffix('.tmp')
        temporary.write_text(json.dumps(dict(state=state, hostname=args.hostname + '.local',
            interface=args.interface, addresses=addresses, port=args.port,
            updated_at=time.time(), error=error), indent=2), encoding='utf-8')
        temporary.replace(status)

    zc = None
    current = None
    try:
        while running and not stop.exists():
            addresses = adapter_addresses(args.interface)
            if addresses != current:
                if zc is not None:
                    zc.close()  # Withdraw the old records before changing interface/address.
                    zc = None
                current = None
                if not addresses:
                    write_status('waiting_for_network', [])
                    current = []
                else:
                    try:
                        zc = Zeroconf(interfaces=addresses, ip_version=IPVersion.V4Only)
                        info = ServiceInfo('_http._tcp.local.',
                            f'{args.hostname} LM Studio._http._tcp.local.',
                            addresses=[socket.inet_aton(address) for address in addresses],
                            port=args.port, server=args.hostname + '.local.',
                            properties={'path': '/v1/models', 'api': 'openai-compatible'},
                            host_ttl=30, other_ttl=30)
                        # Do not silently rename the service on conflicts.
                        zc.register_service(info, allow_name_change=False)
                        current = addresses
                        logging.info('Published %s.local:%s on %s', args.hostname, args.port, addresses)
                        write_status('published', addresses)
                    except Exception as exc:
                        logging.exception('Publication failed; retrying')
                        write_status('error', addresses, str(exc))
                        if zc is not None:
                            zc.close()
                            zc = None
            for _ in range(10):
                if not running or stop.exists():
                    break
                time.sleep(1)
    finally:
        if zc is not None:
            zc.close()
        write_status('stopped', [])


if __name__ == '__main__':
    main()
