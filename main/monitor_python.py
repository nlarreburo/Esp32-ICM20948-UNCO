import struct
import socket
import sys
import os
import glob
import time
import threading
import queue
import json
import signal
import numpy as np
from collections import deque
import matplotlib.pyplot as plt

# =============================================================================
# CONSTANTES
# =============================================================================

HEADER_FORMAT = '<6sBBIHHffBHHH'
HEADER_SIZE   = struct.calcsize(HEADER_FORMAT)

ACEL_SCALE = {
    0: 16384.0,
    1:  8192.0,
    2:  4096.0,
    3:  2048.0,
}

DEFAULT_HOST   = '192.168.4.1'
DEFAULT_PORT   = 5000
SAMPLE_RATE_HZ = 225
WINDOW_SECONDS = 10
WINDOW_SAMPLES = SAMPLE_RATE_HZ * WINDOW_SECONDS
CHUNK_HDR_SIZE = 9
MEAS_HDR_SIZE  = 12
STREAM_ACCEL_SCALE = 8192.0   # ±4g — debe coincidir con ICM20948_ACCEL_RANGE_4G

# =============================================================================
# PARSEO DE MEDICIoN
# =============================================================================

def parse_measurement(data: bytes) -> dict:
    if len(data) < HEADER_SIZE:
        print(f"  Error: datos muy cortos ({len(data)} bytes)")
        return None

    hdr = struct.unpack_from(HEADER_FORMAT, data, offset=0)

    mac_bytes    = hdr[0]
    slot_index   = hdr[1]
    trigger_src  = hdr[2]
    timestamp_ms = hdr[3]
    pre_ms       = hdr[4]
    post_ms      = hdr[5]
    threshold_g  = hdr[6]
    rms_trigger  = hdr[7]
    acel_range   = hdr[8]
    sample_rate  = hdr[9]
    num_samples  = hdr[10]
    pre_samples  = hdr[11]

    mac   = ':'.join(f'{b:02x}' for b in mac_bytes)
    scale = ACEL_SCALE.get(acel_range, 8192.0)

    samples_raw  = data[HEADER_SIZE:]
    actual_bytes = len(samples_raw)
    if actual_bytes < num_samples * 6:
        num_samples = actual_bytes // 6

    samples = np.frombuffer(samples_raw[:num_samples * 6], dtype='<i2').reshape(num_samples, 3)

    x_g = samples[:, 0].astype(float) / scale
    y_g = samples[:, 1].astype(float) / scale
    z_g = samples[:, 2].astype(float) / scale
    mag = np.sqrt(x_g**2 + y_g**2 + z_g**2)

    return {
        'mac':          mac,
        'slot':         slot_index,
        'trigger':      'AUTO' if trigger_src == 0 else 'MANUAL',
        'timestamp_ms': timestamp_ms,
        'pre_ms':       pre_ms,
        'post_ms':      post_ms,
        'threshold_g':  threshold_g,
        'rms_trigger':  rms_trigger,
        'acel_range':   acel_range,
        'sample_rate':  sample_rate,
        'num_samples':  num_samples,
        'pre_samples':  pre_samples,
        'x_g':          x_g,
        'y_g':          y_g,
        'z_g':          z_g,
        'mag':          mag,
        'rms_calc':     float(np.sqrt(np.mean(mag**2))),
        'peak_calc':    float(np.max(mag)),
    }


def print_measurement(m: dict, verbose: bool = False):
    print(f"  MAC:            {m['mac']}")
    print(f"  Slot:           {m['slot']}")
    print(f"  Trigger:        {m['trigger']}")
    print(f"  Timestamp:      {m['timestamp_ms']} ms")
    print(f"  Config:         pre={m['pre_ms']}ms post={m['post_ms']}ms "
          f"thr={m['threshold_g']:.2f}g")
    print(f"  Sample rate:    {m['sample_rate']} Hz")
    print(f"  Rango:          ±{2 ** (m['acel_range'] + 1)}g")
    print(f"  Muestras:       {m['num_samples']} total "
          f"(pre={m['pre_samples']} post={m['num_samples'] - m['pre_samples']})")
    print(f"  RMS trigger:    {m['rms_trigger']:.4f}g")
    print(f"  RMS calculado:  {m['rms_calc']:.4f}g")
    print(f"  PEAK calculado: {m['peak_calc']:.4f}g")

    if verbose:
        print(f"\n  {'i':>5} {'tipo':>5} {'x_g':>10} {'y_g':>10} {'z_g':>10} {'mag':>10}")
        for i in range(m['num_samples']):
            tipo = 'pre' if i < m['pre_samples'] else 'post'
            print(f"  {i:>5} {tipo:>5} {m['x_g'][i]:>10.4f} {m['y_g'][i]:>10.4f} "
                  f"{m['z_g'][i]:>10.4f} {m['mag'][i]:>10.4f}")


def export_csv(m: dict, filepath: str):
    with open(filepath, 'w') as f:
        f.write(f"# MAC: {m['mac']}\n")
        f.write(f"# Slot: {m['slot']}\n")
        f.write(f"# Trigger: {m['trigger']}\n")
        f.write(f"# Timestamp: {m['timestamp_ms']} ms\n")
        f.write(f"# Sample rate: {m['sample_rate']} Hz\n")
        f.write(f"# Pre samples: {m['pre_samples']}\n")
        f.write(f"# RMS: {m['rms_calc']:.4f}g\n")
        f.write(f"# PEAK: {m['peak_calc']:.4f}g\n")
        f.write("indice,tipo,x_g,y_g,z_g,magnitud\n")
        for i in range(m['num_samples']):
            tipo = 'pre' if i < m['pre_samples'] else 'post'
            f.write(f"{i},{tipo},{m['x_g'][i]:.6f},{m['y_g'][i]:.6f},"
                    f"{m['z_g'][i]:.6f},{m['mag'][i]:.6f}\n")
    print(f"  Exportado: {filepath}")


# =============================================================================
# HELPERS DE MENu
# =============================================================================

def separator():
    print("\n" + "═" * 50)

def input_int(prompt: str, min_val: int, max_val: int) -> int:
    while True:
        try:
            val = int(input(prompt).strip())
            if min_val <= val <= max_val:
                return val
            print(f"  Ingresa un numero entre {min_val} y {max_val}")
        except (ValueError, EOFError, KeyboardInterrupt):
            return -1

def select_node(nodes: list, allow_all: bool = False) -> list:
    if not nodes:
        print("  No hay nodos online")
        return []

    print()
    for i, n in enumerate(nodes, 1):
        role   = n.get('role', 'slave')
        status = n.get('status', 'idle')
        mac    = n['mac']
        print(f"  {i}. {mac} ({role}) — {status}")

    if allow_all:
        print(f"  {len(nodes)+1}. Todos")

    max_val = len(nodes) + (1 if allow_all else 0)
    sel = input_int(f"\n> ", 1, max_val)
    if sel < 0:
        return []

    if allow_all and sel == len(nodes) + 1:
        return [n['mac'] for n in nodes]

    return [nodes[sel - 1]['mac']]

def select_slot() -> int:
    print()
    print("  Slot a consultar (0-4):")
    return input_int("> ", 0, 4)


# =============================================================================
# CONEXIoN TCP
# =============================================================================

class ESP32Connection:

    def __init__(self, host=DEFAULT_HOST, port=DEFAULT_PORT):
        self.host      = host
        self.port      = port
        self.sock      = None
        self._nodes    = []
        self._recv_buf = b''   # buffer compartido entre _read_line y recv_thread

    def connect(self) -> bool:
        try:
            self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            self.sock.settimeout(10.0)
            self.sock.connect((self.host, self.port))
            self._recv_buf = b''
            print(f"\n  Conectado a {self.host}:{self.port}")
            return True
        except Exception as e:
            print(f"\n  Error de conexion: {e}")
            return False

    def _read_line(self, timeout=10.0) -> str:
        """Lee hasta '\\n', dejando bytes sobrantes en self._recv_buf."""
        self.sock.settimeout(timeout)
        while b'\n' not in self._recv_buf:
            try:
                data = self.sock.recv(256)
                if not data:
                    break
                self._recv_buf += data
            except socket.timeout:
                break
        if b'\n' in self._recv_buf:
            line, self._recv_buf = self._recv_buf.split(b'\n', 1)
            return line.decode(errors='replace').strip()
        line, self._recv_buf = self._recv_buf, b''
        return line.decode(errors='replace').strip()

    def disconnect(self):
        if self.sock:
            try:
                self.sock.close()
            except Exception:
                pass
            self.sock = None

    def reconnect(self) -> bool:
        self.disconnect()
        return self.connect()

    def send_command(self, cmd: str) -> str:
        if not self.sock:
            return ''
        try:
            self.sock.sendall((cmd + '\n').encode())
            return self._read_line(timeout=10.0)
        except socket.timeout:
            print("  Timeout esperando respuesta")
            return ''
        except Exception as e:
            print(f"  Error: {e}")
            return ''

    def get_nodes(self) -> list:
        resp = self.send_command('CMD_GET_STATUS')
        try:
            json_str = resp.split(' ', 1)[1] if ' ' in resp else resp
            status   = json.loads(json_str)
            nodes    = []
            for i, n in enumerate(status['nodes']):
                if n['online']:
                    n['role'] = 'master' if i == 0 and status['role'] == 'master' \
                                else 'slave'
                    nodes.append(n)
            self._nodes = nodes
            return nodes
        except Exception as e:
            print(f"  Error parseando status: {e}")
            return []

    def get_data(self, mac: str, slot: int) -> dict:
        if not self.sock:
            return None

        self.sock.sendall(f'CMD_GET_DATA MAC={mac} SLOT={slot}\n'.encode())

        buf = self._recv_buf
        self._recv_buf = b''
        self.sock.settimeout(10.0)

        try:
            while len(buf) < MEAS_HDR_SIZE:
                chunk = self.sock.recv(4096)
                if not chunk:
                    return None
                buf += chunk

            pkt_type = buf[0]
            if pkt_type != 0x02:
                print(f"  Respuesta: {buf.decode(errors='replace').strip()}")
                return None

            pkt_mac    = ':'.join(f'{b:02x}' for b in buf[1:7])
            pkt_slot   = buf[7]
            pkt_length = struct.unpack_from('<I', buf, 8)[0]

            print(f"  MAC: {pkt_mac} | Slot: {pkt_slot} | Bytes: {pkt_length}")

            payload = buf[MEAS_HDR_SIZE:]
            while len(payload) < pkt_length:
                chunk = self.sock.recv(4096)
                if not chunk:
                    break
                payload += chunk

            print(f"  Recibidos: {len(payload)} bytes")
            m = parse_measurement(payload)
            if m:
                print()
                print_measurement(m)
            return m

        except socket.timeout:
            print("  Timeout esperando datos")
            return None

    def stream(self, macs: list, stop_all: bool = False):
        if not self.sock:
            return

        # CMD_STREAM_START_ALL ya fue enviado por menu_stream — no repetir por nodo
        if not stop_all:
            for mac in macs:
                self.sock.sendall(f'CMD_STREAM_START MAC={mac}\n'.encode())
                try:
                    resp = self._read_line(timeout=3.0)
                    if 'OK' not in resp:
                        print(f"  Error iniciando stream {mac}: {resp}")
                except socket.timeout:
                    print(f"  Timeout esperando OK para {mac}")

        windows  = {}
        stop_evt = threading.Event()

        def recv_thread():
            buf = self._recv_buf   # bytes que pudieron quedar tras leer los OK
            self._recv_buf = b''
            self.sock.settimeout(1.0)
            try:
                while not stop_evt.is_set():
                    try:
                        data = self.sock.recv(4096)
                        if not data:
                            break
                        buf += data

                        while len(buf) >= CHUNK_HDR_SIZE:
                            if buf[0] != 0x01:
                                buf = buf[1:]
                                continue

                            count   = struct.unpack_from('<H', buf, 7)[0]
                            payload = count * 6

                            if len(buf) < CHUNK_HDR_SIZE + payload:
                                break

                            pkt_mac     = ':'.join(f'{b:02x}' for b in buf[1:7])
                            samples_raw = buf[CHUNK_HDR_SIZE:CHUNK_HDR_SIZE + payload]
                            buf         = buf[CHUNK_HDR_SIZE + payload:]

                            if pkt_mac not in windows:
                                print(f"  Nuevo nodo: {pkt_mac}")
                                windows[pkt_mac] = {
                                    'queue':        queue.Queue(),
                                    'x_buf':        deque(maxlen=WINDOW_SAMPLES),
                                    'y_buf':        deque(maxlen=WINDOW_SAMPLES),
                                    'z_buf':        deque(maxlen=WINDOW_SAMPLES),
                                    't_buf':        deque(maxlen=WINDOW_SAMPLES),
                                    'sample_count': 0,
                                    'scale':        STREAM_ACCEL_SCALE,
                                }

                            windows[pkt_mac]['queue'].put((samples_raw, count))

                    except socket.timeout:
                        continue
            except Exception as e:
                if not stop_evt.is_set():
                    print(f"  Error recv: {e}")
            finally:
                stop_evt.set()

        rx_thread = threading.Thread(target=recv_thread, daemon=True)
        rx_thread.start()

        figs = {}

        def make_fig(mac):
            fig, ax = plt.subplots(figsize=(12, 5))
            fig.canvas.manager.set_window_title(f'Stream — {mac}')
            ax.set_title(f'Acelerometro — {mac}')
            ax.set_xlabel('Tiempo (s)')
            ax.set_ylabel('Aceleracion (g)')
            ax.set_ylim(-5, 5)
            ax.grid(True, alpha=0.3)
            lx, = ax.plot([], [], color='blue',  lw=1, label='X')
            ly, = ax.plot([], [], color='green', lw=1, label='Y')
            lz, = ax.plot([], [], color='red',   lw=1, label='Z')
            ax.legend(loc='upper right')
            plt.tight_layout()
            return fig, ax, lx, ly, lz

        # Capturar SIGINT antes de que llegue a matplotlib
        interrupted    = threading.Event()
        original_sigint = signal.getsignal(signal.SIGINT)

        def handle_sigint(sig, frame):
            interrupted.set()
            stop_evt.set()

        signal.signal(signal.SIGINT, handle_sigint)

        print(f"\n  Stream activo — Ctrl+C para detener\n")

        try:
            while not stop_evt.is_set() and not interrupted.is_set():
                for mac, w in list(windows.items()):
                    if mac not in figs:
                        figs[mac] = make_fig(mac)

                    fig, ax, lx, ly, lz = figs[mac]

                    while not w['queue'].empty():
                        try:
                            samples_raw, count = w['queue'].get_nowait()
                            samples = struct.unpack_from(f'<{count*3}h', samples_raw)
                            for i in range(count):
                                x_g = samples[i*3]   / w['scale']
                                y_g = samples[i*3+1] / w['scale']
                                z_g = samples[i*3+2] / w['scale']
                                t   = w['sample_count'] / SAMPLE_RATE_HZ
                                w['x_buf'].append(x_g)
                                w['y_buf'].append(y_g)
                                w['z_buf'].append(z_g)
                                w['t_buf'].append(t)
                                w['sample_count'] += 1
                        except queue.Empty:
                            break

                    if len(w['t_buf']) > 0:
                        t_list = list(w['t_buf'])
                        lx.set_data(t_list, list(w['x_buf']))
                        ly.set_data(t_list, list(w['y_buf']))
                        lz.set_data(t_list, list(w['z_buf']))
                        ax.set_xlim(t_list[0], max(t_list[-1], WINDOW_SECONDS))
                        fig.canvas.draw_idle()

                plt.pause(0.05)

        finally:
            # Restaurar handler ANTES de cerrar matplotlib
            signal.signal(signal.SIGINT, original_sigint)

            stop_evt.set()
            rx_thread.join(timeout=2.0)

            # Cerrar figuras individualmente
            for mac in list(figs.keys()):
                try:
                    plt.close(figs[mac][0])
                except Exception:
                    pass
            figs.clear()

            # Enviar stop al ESP32 solo si recv_thread ya salio (sin race en el socket)
            if not rx_thread.is_alive():
                try:
                    stop_cmd = b'CMD_STREAM_STOP_ALL\n' if stop_all else b'CMD_STREAM_STOP\n'
                    self.sock.sendall(stop_cmd)
                    self.sock.settimeout(1.0)
                    try:
                        self.sock.recv(1024)
                    except socket.timeout:
                        pass
                except Exception:
                    pass

            # Recrear socket limpio
            self.reconnect()

            if interrupted.is_set():
                print("\n  Stream detenido")


# =============================================================================
# MODO SD
# =============================================================================

def sd_list_files(sd_path: str):
    search_path = os.path.join(sd_path, 'mediciones', '*.bin')
    files = sorted(glob.glob(search_path))
    if not files:
        files = sorted(glob.glob(os.path.join(sd_path, '*.bin')))
    if not files:
        print(f"  No se encontraron archivos .bin en {sd_path}")
        return []
    print(f"\n  Archivos encontrados: {len(files)}\n")
    for i, f in enumerate(files, 1):
        print(f"  {i}. {os.path.basename(f)} ({os.path.getsize(f)} bytes)")
    return files


def sd_mode():
    separator()
    print("  MODO SD")
    separator()
    sd_path = input("\n  Ruta de la SD (o archivo .bin): ").strip()

    if sd_path.endswith('.bin'):
        if os.path.exists(sd_path):
            with open(sd_path, 'rb') as f:
                m = parse_measurement(f.read())
            if m:
                print_measurement(m)
                resp = input_int("\n  1. Exportar CSV\n  2. Volver\n\n> ", 1, 2)
                if resp == 1:
                    export_csv(m, sd_path.replace('.bin', '.csv'))
        return

    files = sd_list_files(sd_path)
    if not files:
        return

    while True:
        separator()
        print("  ARCHIVOS SD")
        separator()
        for i, f in enumerate(files, 1):
            print(f"  {i}. {os.path.basename(f)}")
        print(f"  {len(files)+1}. Exportar todos a CSV")
        print(f"  {len(files)+2}. Volver")

        sel = input_int("\n> ", 1, len(files) + 2)
        if sel < 0 or sel == len(files) + 2:
            break

        if sel == len(files) + 1:
            for f in files:
                with open(f, 'rb') as fh:
                    m = parse_measurement(fh.read())
                if m:
                    export_csv(m, f.replace('.bin', '.csv'))
            continue

        filepath = files[sel - 1]
        with open(filepath, 'rb') as f:
            m = parse_measurement(f.read())
        if m:
            print_measurement(m)
            resp = input_int("\n  1. Exportar CSV\n  2. Ver detalle\n  3. Volver\n\n> ", 1, 3)
            if resp == 1:
                export_csv(m, filepath.replace('.bin', '.csv'))
            elif resp == 2:
                print_measurement(m, verbose=True)
                input("\n  [Enter para continuar]")


# =============================================================================
# MENu TCP
# =============================================================================

def menu_status(conn: ESP32Connection):
    separator()
    print("  ESTADO DE LOS NODOS")
    separator()
    nodes = conn.get_nodes()
    if not nodes:
        print("  Sin nodos online")
        input("\n  [Enter para continuar]")
        return
    print()
    for n in nodes:
        print(f"  MAC: {n['mac']} | Estado: {n['status']} | Slot: {n['slot']}")
    input("\n  [Enter para continuar]")


def menu_trigger(conn: ESP32Connection):
    separator()
    print("  DISPARAR MEDICIoN")
    separator()
    print("\n  1. Todos los nodos")
    print("  2. Cancelar")
    sel = input_int("\n> ", 1, 2)
    if sel == 1:
        resp = conn.send_command('CMD_START')
        print(f"\n  {resp}")
        input("\n  [Enter para continuar]")


def menu_stop(conn: ESP32Connection):
    separator()
    print("  DETENER")
    separator()
    resp = conn.send_command('CMD_STOP')
    print(f"\n  {resp}")
    input("\n  [Enter para continuar]")


def menu_config(conn: ESP32Connection):
    separator()
    print("  CONFIGURAR PARaMETROS")
    separator()
    print()
    try:
        pre    = input("  Pre-trigger (ms) [500]: ").strip()  or '500'
        post   = input("  Post-trigger (ms) [1000]: ").strip() or '1000'
        manual = input("  Manual duration (ms) [2000]: ").strip() or '2000'
        thr    = input("  Threshold (g) [1.5]: ").strip() or '1.5'
    except KeyboardInterrupt:
        return

    resp = conn.send_command(
        f'CMD_SET_CONFIG PRE={pre} POST={post} MANUAL={manual} THR={thr}')
    print(f"\n  {resp}")
    input("\n  [Enter para continuar]")


def menu_stream(conn: ESP32Connection):
    separator()
    print("  STREAM EN TIEMPO REAL")
    separator()
    print("\n  Obteniendo nodos...")
    nodes = conn.get_nodes()
    if not nodes:
        input("\n  [Enter para continuar]")
        return

    print("\n  Selecciona el nodo:")
    macs = select_node(nodes, allow_all=True)
    if not macs:
        return

    stop_all = len(macs) == len(nodes)
    if stop_all:
        conn.send_command('CMD_STREAM_START_ALL')

    conn.stream(macs, stop_all=stop_all)


def menu_get_data(conn: ESP32Connection):
    separator()
    print("  OBTENER MEDICIoN GUARDADA")
    separator()
    print("\n  Obteniendo nodos...")
    nodes = conn.get_nodes()
    if not nodes:
        input("\n  [Enter para continuar]")
        return

    print("\n  Selecciona el nodo:")
    macs = select_node(nodes, allow_all=False)
    if not macs:
        return

    slot = select_slot()
    if slot < 0:
        return

    m = conn.get_data(macs[0], slot)
    if m:
        print()
        resp = input_int("  1. Exportar CSV\n  2. Ver detalle\n  3. Volver\n\n> ", 1, 3)
        if resp == 1:
            csv_path = f"medicion_{macs[0].replace(':','') }_{slot}.csv"
            export_csv(m, csv_path)
        elif resp == 2:
            print_measurement(m, verbose=True)
            input("\n  [Enter para continuar]")
    else:
        input("\n  [Enter para continuar]")


def menu_sync(conn: ESP32Connection):
    separator()
    print("  SINCRONIZACIoN AUTOMaTICA")
    separator()
    print()
    print("  1. Activar sync")
    print("  2. Desactivar sync")
    print("  3. Volver")

    sel = input_int("\n> ", 1, 3)
    if sel == 1:
        print(f"\n  {conn.send_command('CMD_SYNC_START')}")
    elif sel == 2:
        print(f"\n  {conn.send_command('CMD_SYNC_STOP')}")
    input("\n  [Enter para continuar]")


def tcp_mode():
    separator()
    print("  CONEXIoN TCP")
    separator()

    host = input(f"\n  IP del master [{DEFAULT_HOST}]: ").strip() or DEFAULT_HOST
    port = input(f"  Puerto [{DEFAULT_PORT}]: ").strip()
    port = int(port) if port else DEFAULT_PORT

    conn = ESP32Connection(host, port)
    if not conn.connect():
        input("\n  [Enter para continuar]")
        return

    while True:
        separator()
        print("  MENu PRINCIPAL")
        separator()
        print()
        print("  1. Ver estado de los nodos")
        print("  2. Disparar medicion")
        print("  3. Detener medicion/stream")
        print("  4. Configurar parametros")
        print("  5. Stream en tiempo real")
        print("  6. Obtener medicion guardada")
        print("  7. Sincronizacion automatica")
        print("  8. Desconectar y salir")

        sel = input_int("\n> ", 1, 8)

        if sel < 0 or sel == 8:
            break
        elif sel == 1:
            menu_status(conn)
        elif sel == 2:
            menu_trigger(conn)
        elif sel == 3:
            menu_stop(conn)
        elif sel == 4:
            menu_config(conn)
        elif sel == 5:
            menu_stream(conn)
        elif sel == 6:
            menu_get_data(conn)
        elif sel == 7:
            menu_sync(conn)

    conn.disconnect()


# =============================================================================
# MENu PRINCIPAL
# =============================================================================

def main():
    while True:
        separator()
        print("  CLIENTE DE MONITOREO DE VIBRACIONES ESP32")
        separator()
        print()
        print("  1. Conectar por TCP al ESP32")
        print("  2. Leer archivos de la SD")
        print("  3. Salir")

        sel = input_int("\n> ", 1, 3)

        if sel < 0 or sel == 3:
            print("\n  Hasta luego.\n")
            break
        elif sel == 1:
            tcp_mode()
        elif sel == 2:
            sd_mode()


if __name__ == '__main__':
    main()