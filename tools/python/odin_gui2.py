"""
odin_gui2.py - Odin-data ZMQ GUI client
Dependencies (all pip-installable): pyzmq PyQt5 pyqtgraph numpy PyYAML
"""

import sys
import json
import time
import logging
from datetime import datetime
from pathlib import Path

import numpy as np
import yaml
import zmq
from PyQt5.QtCore import QTimer, Qt, QThread, pyqtSignal
from PyQt5.QtWidgets import (
    QApplication, QMainWindow, QWidget, QVBoxLayout, QHBoxLayout,
    QPushButton, QLineEdit, QLabel, QSpinBox, QTabWidget, QSplitter,
    QTreeWidget, QTreeWidgetItem, QHeaderView, QGroupBox, QFormLayout,
    QComboBox, QPlainTextEdit, QScrollArea, QSizePolicy, QCheckBox, QSlider,
)
import pyqtgraph as pg

# ---------------------------------------------------------------------------
# Constants
# ---------------------------------------------------------------------------

logging.basicConfig(
    level=logging.DEBUG,
    format="%(asctime)s.%(msecs)03d [%(levelname)s] %(name)s: %(message)s",
    datefmt="%H:%M:%S",
)
log = logging.getLogger("odin_gui2")

EXCLUDED_PLUGINS = {"hdf", "liveview"}  # lower-cased names to skip when detecting main plugin
CAMERA_PORT = 9001
STATUS_TIMEOUT_MS = 5000
CONFIGURE_TIMEOUT_MS = 10000
MODE_CONFIGS_FILE = Path(__file__).parent / "mode_configs.yaml"

CAMERA_PROPERTIES = {
    "camera.exposure_time":    ("Exposure Time (s)",    float, True),
    "camera.image_timeout":    ("Image Timeout (s)",    float, True),
    "camera.num_frames":       ("Number of Frames",     int,   True),
    "camera.camera_number":    ("Camera Number",        int,   False),
    "camera.simulated_camera": ("Simulated Camera",     bool,  False),
    "camera.timestamp_mode":   ("Timestamp Mode",       int,   True),
    "camera.trigger_active":   ("Trigger Active",       int,   True),
    "camera.trigger_connector":("Trigger Connector",    int,   True),
    "camera.trigger_mode":     ("Trigger Mode",         int,   True),
    "camera.trigger_polarity": ("Trigger Polarity",     int,   True),
    "camera.trigger_source":   ("Trigger Source",       int,   True),
}

# ---------------------------------------------------------------------------
# ZMQ worker thread
# ---------------------------------------------------------------------------

class ZmqRequest:
    """Describes a single ZMQ request to be sent by ZmqWorker."""
    def __init__(self, tag: str, endpoint: str, msg_type: str, msg_val: str,
                 params: dict = None, timeout_ms: int = STATUS_TIMEOUT_MS):
        self.tag = tag                  # arbitrary label returned with the response
        self.endpoint = endpoint
        self.msg_type = msg_type
        self.msg_val = msg_val
        self.params = params or {}
        self.timeout_ms = timeout_ms


class ZmqWorker(QThread):
    """
    Runs all ZMQ I/O on a background thread so the Qt main thread is never
    blocked by socket.poll().

    Requests are queued via request(). Each completed request emits
    response_ready with (tag, sent_msg, response_or_None).
    """

    response_ready = pyqtSignal(str, dict, object)   # tag, sent, response|None

    def __init__(self, parent=None):
        super().__init__(parent)
        self._queue: list[ZmqRequest] = []
        self._ctx = zmq.Context.instance()
        # One persistent socket per endpoint, keyed by endpoint string
        self._sockets: dict[str, zmq.Socket] = {}
        self._msg_id = 0
        self._running = False
        # Use a ZMQ PAIR inproc socket to wake the thread when new work arrives
        self._wake_addr = f"inproc://zmqworker-{id(self)}"
        self._wake_send = self._ctx.socket(zmq.PAIR)
        self._wake_recv = self._ctx.socket(zmq.PAIR)
        self._wake_recv.bind(self._wake_addr)
        self._wake_send.connect(self._wake_addr)

    def request(self, req: ZmqRequest):
        log.debug("ZmqWorker.request: queuing %s %s (tag=%s)", req.msg_type, req.msg_val, req.tag)
        self._queue.append(req)
        self._wake_send.send(b"w")  # wake the run loop

    def run(self):
        log.debug("ZmqWorker: thread started")
        self._running = True
        poller = zmq.Poller()
        poller.register(self._wake_recv, zmq.POLLIN)

        while self._running:
            # Wait for either a wake signal or an incoming reply on any socket
            socks = dict(poller.poll(timeout=200))

            if self._wake_recv in socks:
                self._wake_recv.recv()  # drain the wake byte

            # Process all queued requests
            while self._queue:
                req = self._queue.pop(0)
                self._send_request(req, poller)

        log.debug("ZmqWorker: thread stopping, closing sockets")
        for sock in self._sockets.values():
            sock.close()
        self._wake_recv.close()
        self._wake_send.close()

    def _get_socket(self, endpoint: str) -> zmq.Socket:
        if endpoint not in self._sockets:
            log.debug("ZmqWorker: creating socket for %s", endpoint)
            sock = self._ctx.socket(zmq.DEALER)
            sock.connect(endpoint)
            self._sockets[endpoint] = sock
        return self._sockets[endpoint]

    def _send_request(self, req: ZmqRequest, poller: zmq.Poller):
        self._msg_id += 1
        msg = {
            "msg_type": req.msg_type,
            "msg_val": req.msg_val,
            "timestamp": datetime.now().isoformat(),
            "id": self._msg_id,
            "params": req.params,
        }
        sock = self._get_socket(req.endpoint)
        log.debug("ZmqWorker: sending %s/%s to %s (id=%d, tag=%s)",
                  req.msg_type, req.msg_val, req.endpoint, self._msg_id, req.tag)
        t0 = time.monotonic()
        sock.send_json(msg)

        if sock.poll(req.timeout_ms):
            resp = sock.recv_json()
            elapsed = (time.monotonic() - t0) * 1000
            log.debug("ZmqWorker: response for %s/%s in %.1f ms (tag=%s)",
                      req.msg_type, req.msg_val, elapsed, req.tag)
            self.response_ready.emit(req.tag, msg, resp)
        else:
            elapsed = (time.monotonic() - t0) * 1000
            log.warning("ZmqWorker: TIMEOUT after %.0f ms for %s/%s (tag=%s)",
                        elapsed, req.msg_type, req.msg_val, req.tag)
            self.response_ready.emit(req.tag, msg, None)

    def close_endpoint(self, endpoint: str):
        sock = self._sockets.pop(endpoint, None)
        if sock:
            sock.close()
            log.debug("ZmqWorker: closed socket for %s", endpoint)

    def stop(self):
        log.debug("ZmqWorker: stop requested")
        self._running = False
        self._wake_send.send(b"q")
        self.wait()


# ---------------------------------------------------------------------------
# Liveview receiver thread
# ---------------------------------------------------------------------------

class LiveviewThread(QThread):
    """Receives frames from a ZMQ SUB socket and emits them as numpy arrays."""
    frame_received = pyqtSignal(object)  # numpy array

    # Map HDF datatype strings to numpy dtype strings
    HDF_DTYPE_MAP = {
        "uint8": "uint8", "uint16": "uint16", "uint32": "uint32", "uint64": "uint64",
        "int8": "int8", "int16": "int16", "int32": "int32", "int64": "int64",
        "float": "float32", "float32": "float32", "float64": "float64",
    }

    def __init__(self, endpoint: str, hdf_dtype: str = None, hdf_dims: list = None, parent=None):
        super().__init__(parent)
        self._endpoint = endpoint
        self._running = False
        # dtype/dims from HDF config — used as fallback when header values are unreliable
        self._hdf_dtype = self.HDF_DTYPE_MAP.get(hdf_dtype, hdf_dtype) if hdf_dtype else None
        self._hdf_dims = [int(d) for d in hdf_dims] if hdf_dims else None

    def run(self):
        log.debug("LiveviewThread: connecting to %s (hdf_dtype=%s hdf_dims=%s)",
                  self._endpoint, self._hdf_dtype, self._hdf_dims)
        ctx = zmq.Context.instance()
        sock = ctx.socket(zmq.SUB)
        sock.connect(self._endpoint)
        sock.setsockopt_string(zmq.SUBSCRIBE, "")
        sock.setsockopt(zmq.RCVTIMEO, 200)
        self._running = True
        frame_count = 0
        while self._running:
            try:
                parts = sock.recv_multipart()
                if len(parts) >= 2:
                    header = json.loads(parts[0].decode())
                    raw_bytes = len(parts[1])

                    # Determine shape from header (values may be strings)
                    shape = header.get("shape", None)
                    if shape:
                        shape = [int(s) for s in shape]
                    # Fall back to HDF dims if header has no shape
                    if not shape and self._hdf_dims:
                        shape = self._hdf_dims

                    # Determine dtype: use header dtype first, then HDF config
                    raw_dtype = header.get("dtype", None)
                    if raw_dtype:
                        dtype_str = self.HDF_DTYPE_MAP.get(raw_dtype, raw_dtype)
                    elif self._hdf_dtype:
                        dtype_str = self._hdf_dtype
                    else:
                        dtype_str = "uint16"

                    # Check if frame is blosc-compressed: payload smaller than
                    # uncompressed size (shape elements × itemsize)
                    payload = parts[1]
                    if shape:
                        uncompressed_size = int(np.prod(shape)) * np.dtype(dtype_str).itemsize
                        if raw_bytes < uncompressed_size:
                            try:
                                import blosc
                                payload = blosc.decompress(payload)
                                log.debug("LiveviewThread: blosc decompressed %d → %d bytes",
                                          raw_bytes, len(payload))
                            except ImportError:
                                log.error("LiveviewThread: frame appears compressed (got %d bytes, "
                                          "expected %d) but blosc is not installed — "
                                          "pip install blosc", raw_bytes, uncompressed_size)
                                continue
                            except Exception as e:
                                log.error("LiveviewThread: blosc decompression failed: %s", e)
                                continue

                    arr = np.frombuffer(payload, dtype=dtype_str)

                    # Reshape the flat array into a displayable 2D frame.
                    #
                    # Three cases based on the header shape [H, W]:
                    #   1. arr.size == H*W          → single 2D frame, use as-is
                    #   2. arr.size == N * H*W       → chunked superframe, extract frame 0
                    #   3. No header shape or no match → fall back to HDF dims
                    #
                    # 3D datasets (e.g. histograms) are handled in LiveviewTab._on_frame
                    # via the Z-reduction controls; they arrive with a 3D shape in the header.
                    if shape and len(shape) == 2:
                        h, w = shape
                        frame_elements = h * w
                        if arr.size == frame_elements:
                            arr = arr.reshape(h, w)
                        elif arr.size % frame_elements == 0:
                            superframe_size = arr.size // frame_elements
                            log.debug("LiveviewThread: chunked superframe — %d frames of (%d,%d), "
                                      "extracting frame 0", superframe_size, h, w)
                            arr = arr.reshape(superframe_size, h, w)[0]
                        else:
                            log.warning("LiveviewThread: cannot reshape %d elements into (%d,%d) "
                                        "or any multiple — skipping frame", arr.size, h, w)
                            continue
                    elif shape and len(shape) >= 3:
                        # 3D (or higher) shape from header — pass through for Z-reduction
                        try:
                            arr = arr.reshape(shape)
                        except ValueError:
                            log.warning("LiveviewThread: cannot reshape %d elements into %s "
                                        "— skipping frame", arr.size, shape)
                            continue
                    elif self._hdf_dims and np.prod(self._hdf_dims) == arr.size:
                        arr = arr.reshape(self._hdf_dims)
                    else:
                        log.warning("LiveviewThread: no usable shape (header=%s hdf_dims=%s) "
                                    "for %d elements — skipping frame", shape, self._hdf_dims, arr.size)
                        continue

                    frame_count += 1
                    if frame_count == 1 or frame_count % 100 == 0:
                        log.debug("LiveviewThread: frame %d — raw_bytes=%d header=%s "
                                  "arr.shape=%s arr.dtype=%s",
                                  frame_count, raw_bytes, header, arr.shape, arr.dtype)
                    self.frame_received.emit(arr)
                else:
                    log.warning("LiveviewThread: unexpected message with %d parts", len(parts))
            except zmq.Again:
                pass
            except Exception as e:
                log.error("LiveviewThread: error processing frame: %s", e)
        log.debug("LiveviewThread: stopped after %d frames", frame_count)
        sock.close()

    def stop(self):
        self._running = False
        self.wait()


# ---------------------------------------------------------------------------
# JSON tree widget
# ---------------------------------------------------------------------------

class JsonTreeWidget(QTreeWidget):
    """A QTreeWidget that displays a nested dict/list and optionally allows editing."""

    value_edited = pyqtSignal(list, object)  # key_path, new_value

    def __init__(self, editable=False, parent=None):
        super().__init__(parent)
        self.setHeaderLabels(["Key", "Value"])
        self.header().setSectionResizeMode(0, QHeaderView.ResizeToContents)
        self.setAlternatingRowColors(True)
        self._editable = editable
        self._updating = False
        if editable:
            self.itemChanged.connect(self._on_item_changed)

    def load(self, data):
        self._updating = True
        self.clear()
        self._populate(self.invisibleRootItem(), data)
        self.expandAll()
        self._updating = False

    def _populate(self, parent, data):
        if isinstance(data, dict):
            for k, v in data.items():
                item = QTreeWidgetItem(parent, [str(k), ""])
                self._populate(item, v)
        elif isinstance(data, list):
            for i, v in enumerate(data):
                item = QTreeWidgetItem(parent, [str(i), ""])
                self._populate(item, v)
        else:
            if parent is self.invisibleRootItem():
                item = QTreeWidgetItem(parent, ["", str(data)])
                if self._editable:
                    item.setFlags(item.flags() | Qt.ItemIsEditable)
            else:
                parent.setText(1, str(data))
                if self._editable:
                    parent.setFlags(parent.flags() | Qt.ItemIsEditable)

    def _on_item_changed(self, item, column):
        if self._updating or column != 1:
            return
        path = self._item_path(item)
        raw = item.text(1)
        self.value_edited.emit(path, _parse_value(raw))

    @staticmethod
    def _item_path(item):
        path = []
        while item is not None and item.parent() is not None:
            path.insert(0, item.parent().text(0) or item.text(0))
            item = item.parent()
        return path


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def _parse_value(text: str):
    try:
        return int(text)
    except ValueError:
        pass
    try:
        return float(text)
    except ValueError:
        pass
    if text.lower() in ("true", "false"):
        return text.lower() == "true"
    return text


def _load_mode_configs() -> dict:
    if MODE_CONFIGS_FILE.exists():
        with open(MODE_CONFIGS_FILE) as f:
            return yaml.safe_load(f) or {}
    return {}


def _detect_main_plugin(plugin_names: list) -> str | None:
    for name in plugin_names:
        if name.lower() not in EXCLUDED_PLUGINS:
            return name
    return None


def _nested_set(d: dict, keys: list, value):
    for k in keys[:-1]:
        d = d.setdefault(k, {})
    d[keys[-1]] = value


# ---------------------------------------------------------------------------
# Individual tab widgets
# ---------------------------------------------------------------------------

class StatusConfigTab(QWidget):
    configure_requested = pyqtSignal(dict)  # params dict

    def __init__(self, parent=None):
        super().__init__(parent)
        splitter = QSplitter(Qt.Horizontal)

        # Status tree (read-only)
        status_box = QGroupBox("Status")
        sl = QVBoxLayout(status_box)
        self.status_tree = JsonTreeWidget(editable=False)
        sl.addWidget(self.status_tree)

        # Config tree (editable)
        config_box = QGroupBox("Configuration")
        cl = QVBoxLayout(config_box)
        self.config_tree = JsonTreeWidget(editable=True)
        self.config_tree.value_edited.connect(self._on_config_edited)
        cl.addWidget(self.config_tree)

        splitter.addWidget(status_box)
        splitter.addWidget(config_box)

        layout = QVBoxLayout(self)
        layout.addWidget(splitter)

    def update_status(self, data: dict):
        self.status_tree.load(data)

    def update_config(self, data: dict):
        self.config_tree.load(data)

    def _on_config_edited(self, path: list, value):
        if not path:
            return
        params = {}
        _nested_set(params, path, value)
        self.configure_requested.emit(params)


class HdfAcquisitionTab(QWidget):
    acquisition_requested = pyqtSignal(str, str, int)   # path, acq_id, frames
    stop_requested = pyqtSignal()

    def __init__(self, parent=None):
        super().__init__(parent)
        layout = QVBoxLayout(self)

        form_box = QGroupBox("New Acquisition")
        form = QFormLayout(form_box)

        self.path_input = QLineEdit("/data/odin-test-captures/")
        self.filename_input = QLineEdit("odin-data-capture")
        self.frames_input = QLineEdit("1000000")

        form.addRow("File Path:", self.path_input)
        form.addRow("File Name:", self.filename_input)
        form.addRow("Number of Frames:", self.frames_input)

        btn_layout = QHBoxLayout()
        self.start_btn = QPushButton("Create Acquisition")
        self.start_btn.clicked.connect(self._on_start)
        self.stop_btn = QPushButton("Stop Acquisition")
        self.stop_btn.clicked.connect(self.stop_requested)
        btn_layout.addWidget(self.start_btn)
        btn_layout.addWidget(self.stop_btn)
        form.addRow(btn_layout)

        layout.addWidget(form_box)

        status_box = QGroupBox("HDF Status")
        sl = QFormLayout(status_box)
        self.writing_label = QLabel("N/A")
        self.frames_written_label = QLabel("N/A")
        self.file_path_label = QLabel("N/A")
        self.file_name_label = QLabel("N/A")
        sl.addRow("Writing:", self.writing_label)
        sl.addRow("Frames written:", self.frames_written_label)
        sl.addRow("File path:", self.file_path_label)
        sl.addRow("File name:", self.file_name_label)
        layout.addWidget(status_box)
        layout.addStretch()

    def update_hdf_status(self, hdf_status: dict):
        self.writing_label.setText(str(hdf_status.get("writing", "N/A")))
        self.frames_written_label.setText(str(hdf_status.get("frames_written", "N/A")))
        self.file_path_label.setText(str(hdf_status.get("file_path", "N/A")))
        self.file_name_label.setText(str(hdf_status.get("file_name", "N/A")))

    def _on_start(self):
        try:
            frames = int(self.frames_input.text())
        except ValueError:
            return
        self.acquisition_requested.emit(
            self.path_input.text(),
            self.filename_input.text(),
            frames,
        )


class PluginCommandsTab(QWidget):
    """Shows supported commands grouped by plugin, each in its own QGroupBox."""
    command_requested = pyqtSignal(str, str)  # plugin_name, command

    def __init__(self, parent=None):
        super().__init__(parent)
        outer = QVBoxLayout(self)

        self._info_label = QLabel("Connect to an odin-data instance to populate commands.")
        self._info_label.setAlignment(Qt.AlignCenter)
        outer.addWidget(self._info_label)

        scroll = QScrollArea()
        scroll.setWidgetResizable(True)
        self._container = QWidget()
        self._layout = QVBoxLayout(self._container)
        self._layout.addStretch()
        scroll.setWidget(self._container)
        outer.addWidget(scroll)

        self._groups: dict[str, QGroupBox] = {}

    def update_commands(self, all_plugin_commands: dict):
        """
        all_plugin_commands: { plugin_name: {"supported": [...]} }
        """
        # Remove old groups
        for box in self._groups.values():
            self._layout.removeWidget(box)
            box.deleteLater()
        self._groups.clear()

        if not all_plugin_commands:
            self._info_label.setText("No commands available.")
            return

        self._info_label.setVisible(False)

        # Insert before the trailing stretch
        insert_pos = self._layout.count() - 1

        for plugin_name, info in all_plugin_commands.items():
            commands = info.get("supported", []) if isinstance(info, dict) else list(info)
            if not commands:
                continue
            box = QGroupBox(plugin_name)
            box_layout = QHBoxLayout(box)
            for cmd in commands:
                btn = QPushButton(cmd)
                btn.clicked.connect(
                    lambda checked, p=plugin_name, c=cmd: self.command_requested.emit(p, c)
                )
                box_layout.addWidget(btn)
            box_layout.addStretch()
            self._layout.insertWidget(insert_pos, box)
            self._groups[plugin_name] = box
            insert_pos += 1


class DecoderModeTab(QWidget):
    mode_change_requested = pyqtSignal(str, dict)  # mode_name, hdf_config

    def __init__(self, parent=None):
        super().__init__(parent)
        self._mode_configs = _load_mode_configs()
        self._buttons: dict[str, QPushButton] = {}
        self._current_mode: str = ""

        layout = QVBoxLayout(self)
        self._status_label = QLabel("Current Mode: N/A")
        layout.addWidget(self._status_label)

        scroll = QScrollArea()
        scroll.setWidgetResizable(True)
        container = QWidget()
        self._btn_layout = QVBoxLayout(container)
        scroll.setWidget(container)
        layout.addWidget(scroll)

        self._build_buttons()

    def _build_buttons(self, available: list[str] | None = None):
        # Clear existing buttons
        for btn in self._buttons.values():
            btn.deleteLater()
        self._buttons.clear()
        # Remove the old stretch
        while self._btn_layout.count():
            item = self._btn_layout.takeAt(0)
            if item.widget():
                item.widget().deleteLater()

        modes = available if available is not None else list(self._mode_configs.keys())
        for mode in modes:
            config = self._mode_configs.get(mode, {})
            btn = QPushButton(mode)
            btn.clicked.connect(lambda checked, m=mode, c=config: self.mode_change_requested.emit(m, c))
            self._btn_layout.addWidget(btn)
            self._buttons[mode] = btn
        self._btn_layout.addStretch()
        # Re-apply current highlight
        if self._current_mode:
            self.set_current_mode(self._current_mode)

    def set_available_modes(self, modes: list[str]):
        """Rebuild buttons to show only modes reported by odin-data."""
        self._build_buttons(available=modes)

    def set_current_mode(self, mode: str):
        self._current_mode = mode
        self._status_label.setText(f"Current Mode: {mode}")
        for name, btn in self._buttons.items():
            if name == mode:
                btn.setStyleSheet("background-color: #4caf50; color: white; font-weight: bold;")
            else:
                btn.setStyleSheet("")


class LiveviewTab(QWidget):
    def __init__(self, parent=None):
        super().__init__(parent)
        self._thread: LiveviewThread | None = None
        self._liveview_endpoint: str | None = None
        self._hdf_dtype: str | None = None
        self._hdf_dims: list | None = None
        self._frame_count = 0
        self._last_3d_shape: tuple | None = None  # tracks shape of most recent 3D frame

        layout = QVBoxLayout(self)

        # --- Main controls row ---
        ctrl_layout = QHBoxLayout()
        self.toggle_btn = QPushButton("Enable Liveview")
        self.toggle_btn.setCheckable(True)
        self.toggle_btn.clicked.connect(self._toggle)
        self.toggle_btn.setEnabled(False)

        self.colormap_combo = QComboBox()
        self.colormap_combo.addItems(["thermal", "viridis", "inferno", "plasma", "grey", "bipolar"])
        self.colormap_combo.currentTextChanged.connect(self._update_colormap)

        self.flip_h_btn = QCheckBox("Flip H")
        self.flip_h_btn.setChecked(True)
        self.flip_v_btn = QCheckBox("Flip V")
        self.rotate_combo = QComboBox()
        self.rotate_combo.addItems(["0°", "90°", "180°", "270°"])
        self.rotate_combo.setCurrentIndex(1)

        ctrl_layout.addWidget(self.toggle_btn)
        ctrl_layout.addWidget(QLabel("Colour map:"))
        ctrl_layout.addWidget(self.colormap_combo)
        ctrl_layout.addSpacing(16)
        ctrl_layout.addWidget(self.flip_h_btn)
        ctrl_layout.addWidget(self.flip_v_btn)
        ctrl_layout.addWidget(QLabel("Rotate:"))
        ctrl_layout.addWidget(self.rotate_combo)
        ctrl_layout.addStretch()
        layout.addLayout(ctrl_layout)

        # --- Z-reduction row (shown only when 3D frames arrive) ---
        self._z_group = QGroupBox("Z / Bin Reduction")
        self._z_group.setVisible(False)
        z_layout = QHBoxLayout(self._z_group)

        z_layout.addWidget(QLabel("Bin axis:"))
        self.z_axis_combo = QComboBox()
        self.z_axis_combo.addItems(["Axis 0", "Axis 1", "Axis 2"])
        self.z_axis_combo.setCurrentIndex(2)  # histogram shape is [X, Y, bins] → axis 2
        self.z_axis_combo.currentIndexChanged.connect(self._on_z_axis_changed)
        z_layout.addWidget(self.z_axis_combo)

        z_layout.addSpacing(12)
        z_layout.addWidget(QLabel("Reduce:"))
        self.z_reduce_combo = QComboBox()
        self.z_reduce_combo.addItems(["Sum", "Mean", "Max", "Single bin"])
        self.z_reduce_combo.currentIndexChanged.connect(self._on_z_reduce_changed)
        z_layout.addWidget(self.z_reduce_combo)

        z_layout.addSpacing(12)
        self._z_range_label = QLabel("Bins 0–0 of 0:")
        z_layout.addWidget(self._z_range_label)

        self.z_lo_slider = QSlider(Qt.Horizontal)
        self.z_lo_slider.setMinimum(0)
        self.z_lo_slider.setMaximum(0)
        self.z_lo_slider.setValue(0)
        self.z_lo_slider.valueChanged.connect(self._on_z_lo_changed)
        z_layout.addWidget(self.z_lo_slider)

        self.z_hi_slider = QSlider(Qt.Horizontal)
        self.z_hi_slider.setMinimum(0)
        self.z_hi_slider.setMaximum(0)
        self.z_hi_slider.setValue(0)
        self.z_hi_slider.valueChanged.connect(self._on_z_hi_changed)
        z_layout.addWidget(self.z_hi_slider)

        layout.addWidget(self._z_group)

        # --- Info labels ---
        self._shape_label = QLabel("Frame shape (from HDF config): unknown")
        layout.addWidget(self._shape_label)

        self._frame_label = QLabel("Frames received: 0  |  Last frame: never")
        layout.addWidget(self._frame_label)

        self.image_view = pg.ImageView()
        self.image_view.setSizePolicy(QSizePolicy.Expanding, QSizePolicy.Expanding)
        layout.addWidget(self.image_view)

    def set_endpoint(self, endpoint: str):
        self._liveview_endpoint = endpoint
        self.toggle_btn.setEnabled(True)

    def set_hdf_config(self, dtype: str, dims: list):
        """Store HDF dataset dtype and dims for use when decoding frames."""
        self._hdf_dtype = dtype
        self._hdf_dims = dims
        self._shape_label.setText(f"Frame shape (from HDF config): {dims}  dtype: {dtype}")

    def _toggle(self, checked: bool):
        if checked:
            self.toggle_btn.setText("Disable Liveview")
            log.debug("LiveviewTab: enabling, endpoint=%s", self._liveview_endpoint)
            self._frame_count = 0
            self._frame_label.setText("Frames received: 0  |  Last frame: never")
            self._start()
        else:
            self.toggle_btn.setText("Enable Liveview")
            log.debug("LiveviewTab: disabling")
            self._stop()

    def _start(self):
        if not self._liveview_endpoint:
            log.warning("LiveviewTab: no endpoint set, cannot start")
            return
        self._thread = LiveviewThread(self._liveview_endpoint,
                                      hdf_dtype=self._hdf_dtype,
                                      hdf_dims=self._hdf_dims)
        self._thread.frame_received.connect(self._on_frame)
        self._thread.start()

    def _stop(self):
        if self._thread:
            self._thread.stop()
            self._thread = None

    # ------------------------------------------------------------------
    # Z / bin reduction helpers
    # ------------------------------------------------------------------

    def _update_z_controls(self, shape: tuple):
        """Show/configure the Z-reduction group when a 3D frame arrives."""
        log.debug("LiveviewTab: 3D frame shape=%s last=%s", shape, self._last_3d_shape)
        if shape == self._last_3d_shape:
            return
        self._last_3d_shape = shape
        axis = self.z_axis_combo.currentIndex()
        n_bins = shape[axis]
        # Block signals while updating slider ranges
        for w in (self.z_lo_slider, self.z_hi_slider):
            w.blockSignals(True)
        self.z_lo_slider.setMinimum(0)
        self.z_lo_slider.setMaximum(n_bins - 1)
        self.z_lo_slider.setValue(0)
        self.z_hi_slider.setMinimum(0)
        self.z_hi_slider.setMaximum(n_bins - 1)
        self.z_hi_slider.setValue(n_bins - 1)
        for w in (self.z_lo_slider, self.z_hi_slider):
            w.blockSignals(False)
        self._update_z_range_label(n_bins)
        self._z_group.setVisible(True)
        self._z_group.show()
        log.debug("LiveviewTab: z_group visible=%s", self._z_group.isVisible())

    def _update_z_range_label(self, n_bins: int | None = None):
        lo = self.z_lo_slider.value()
        hi = self.z_hi_slider.value()
        total = (self.z_hi_slider.maximum() + 1) if n_bins is None else n_bins
        mode = self.z_reduce_combo.currentText()
        if mode == "Single bin":
            self._z_range_label.setText(f"Bin {lo} of {total}:")
        else:
            self._z_range_label.setText(f"Bins {lo}–{hi} of {total} ({mode}):")

    def _on_z_axis_changed(self):
        if self._last_3d_shape:
            self._last_3d_shape = None  # force refresh
            # will be reconfigured on next frame

    def _on_z_reduce_changed(self):
        self._update_z_range_label()
        # Single-bin mode: hi slider not needed
        single = self.z_reduce_combo.currentText() == "Single bin"
        self.z_hi_slider.setVisible(not single)

    def _on_z_lo_changed(self, val: int):
        # Keep lo ≤ hi (except in single-bin mode)
        if self.z_reduce_combo.currentText() != "Single bin":
            if val > self.z_hi_slider.value():
                self.z_hi_slider.setValue(val)
        self._update_z_range_label()

    def _on_z_hi_changed(self, val: int):
        if val < self.z_lo_slider.value():
            self.z_lo_slider.setValue(val)
        self._update_z_range_label()

    def _reduce_z(self, arr: np.ndarray) -> np.ndarray:
        """Collapse the bin axis of a 3D array down to 2D."""
        axis = self.z_axis_combo.currentIndex()
        lo = self.z_lo_slider.value()
        hi = self.z_hi_slider.value()
        mode = self.z_reduce_combo.currentText()

        # Slice along the chosen axis
        slices = [slice(None)] * arr.ndim
        if mode == "Single bin":
            slices[axis] = lo
            return arr[tuple(slices)]
        else:
            slices[axis] = slice(lo, hi + 1)
            sub = arr[tuple(slices)]
            if mode == "Sum":
                return sub.sum(axis=axis)
            elif mode == "Mean":
                return sub.mean(axis=axis)
            else:  # Max
                return sub.max(axis=axis)

    # ------------------------------------------------------------------
    # Display helpers
    # ------------------------------------------------------------------

    def _apply_transforms(self, arr: np.ndarray) -> np.ndarray:
        if self.flip_h_btn.isChecked():
            arr = np.fliplr(arr)
        if self.flip_v_btn.isChecked():
            arr = np.flipud(arr)
        rot = self.rotate_combo.currentIndex()  # 0=0°, 1=90°, 2=180°, 3=270°
        if rot:
            arr = np.rot90(arr, k=rot)
        return arr

    def _dtype_max(self, dtype: np.dtype) -> float:
        if np.issubdtype(dtype, np.integer):
            return float(np.iinfo(dtype).max)
        return 1.0  # float types: normalise to [0, 1]

    def _on_frame(self, arr: np.ndarray):
        self._frame_count += 1
        ts = datetime.now().strftime("%H:%M:%S.%f")[:-3]
        self._frame_label.setText(f"Frames received: {self._frame_count}  |  Last frame: {ts}")

        # Handle 3D (histogram/spectral) frames
        if arr.ndim == 3:
            log.debug("LiveviewTab: _on_frame received 3D array shape=%s", arr.shape)
            self._update_z_controls(arr.shape)
            arr = self._reduce_z(arr)

        arr = self._apply_transforms(arr)
        if self._frame_count == 1:
            # First frame: set levels to full dtype range
            levels = (0, self._dtype_max(arr.dtype))
            self.image_view.setImage(arr, autoLevels=False, autoRange=False, levels=levels)
        else:
            # Subsequent frames: preserve whatever levels the user has set
            self.image_view.setImage(arr, autoLevels=False, autoRange=False)

    def _update_colormap(self, name: str):
        try:
            cmap = pg.colormap.get(name)
            self.image_view.setColorMap(cmap)
        except Exception:
            pass

    def stop(self):
        self._stop()


class CameraControlTab(QWidget):
    camera_command_requested = pyqtSignal(str)               # command string
    camera_property_changed = pyqtSignal(str, object)        # path, value
    camera_config_requested = pyqtSignal()
    camera_enabled_changed = pyqtSignal(bool)                # True = enable

    def __init__(self, parent=None):
        super().__init__(parent)
        self._state = "disconnected"
        self._property_widgets: dict[str, QWidget] = {}

        layout = QVBoxLayout(self)

        # Enable toggle
        enable_layout = QHBoxLayout()
        self.enable_checkbox = QCheckBox("Enable Camera Connection")
        self.enable_checkbox.setChecked(False)
        self.enable_checkbox.toggled.connect(self._on_enable_toggled)
        enable_layout.addWidget(self.enable_checkbox)
        enable_layout.addStretch()
        layout.addLayout(enable_layout)

        # Status + buttons
        status_box = QGroupBox("Camera Control")
        sl = QVBoxLayout(status_box)
        self.status_label = QLabel("Camera Status: Disconnected")
        sl.addWidget(self.status_label)

        btn_layout = QHBoxLayout()
        self.connect_btn = QPushButton("Connect")
        self.connect_btn.clicked.connect(self._on_connect_clicked)
        self.connect_btn.setEnabled(False)
        self.capture_btn = QPushButton("Start Capture")
        self.capture_btn.clicked.connect(self._on_capture_clicked)
        self.capture_btn.setEnabled(False)
        btn_layout.addWidget(self.connect_btn)
        btn_layout.addWidget(self.capture_btn)
        sl.addLayout(btn_layout)
        layout.addWidget(status_box)

        # Properties
        props_box = QGroupBox("Camera Properties")
        self._props_form = QFormLayout(props_box)
        self._build_property_widgets()
        refresh_btn = QPushButton("Refresh Properties")
        refresh_btn.clicked.connect(self.camera_config_requested)
        self._props_form.addRow("", refresh_btn)
        layout.addWidget(props_box)
        layout.addStretch()

    def _build_property_widgets(self):
        for path, (label, ptype, editable) in CAMERA_PROPERTIES.items():
            if ptype == bool:
                widget = QComboBox()
                widget.addItems(["False", "True"])
                widget.setEnabled(editable)
                if editable:
                    widget.currentIndexChanged.connect(
                        lambda idx, p=path: self.camera_property_changed.emit(p, idx == 1)
                    )
            else:
                widget = QLineEdit()
                widget.setReadOnly(not editable)
                if editable:
                    widget.editingFinished.connect(
                        lambda p=path, w=widget, t=ptype: self.camera_property_changed.emit(
                            p, self._convert(w.text(), t)
                        )
                    )
            self._property_widgets[path] = widget
            self._props_form.addRow(label + ":", widget)

    @staticmethod
    def _convert(text: str, ptype):
        try:
            if ptype == bool:
                return text.lower() in ("true", "1", "yes")
            return ptype(text)
        except (ValueError, TypeError):
            return None

    def update_properties(self, config: dict):
        for path, widget in self._property_widgets.items():
            parts = path.split(".")
            val = config
            try:
                for p in parts:
                    val = val[p]
            except (KeyError, TypeError):
                val = None
            if val is None:
                continue
            if isinstance(widget, QComboBox):
                widget.setCurrentIndex(1 if val else 0)
            else:
                widget.setText(str(val))

    def _on_enable_toggled(self, checked: bool):
        self.camera_enabled_changed.emit(checked)
        if checked:
            self.connect_btn.setEnabled(True)
        else:
            self.connect_btn.setEnabled(False)
            self.capture_btn.setEnabled(False)
            self._state = "disconnected"
            self.status_label.setText("Camera Status: Disabled")

    def set_state(self, state: str):
        self._state = state
        self.status_label.setText(f"Camera Status: {state.capitalize()}")
        enabled = self.enable_checkbox.isChecked()
        if state == "disconnected":
            self.connect_btn.setText("Connect")
            self.connect_btn.setEnabled(enabled)
            self.capture_btn.setText("Start Capture")
            self.capture_btn.setEnabled(False)
        elif state == "connected":
            self.connect_btn.setText("Disconnect")
            self.connect_btn.setEnabled(enabled)
            self.capture_btn.setText("Start Capture")
            self.capture_btn.setEnabled(enabled)
        elif state == "capturing":
            self.connect_btn.setText("Disconnect")
            self.connect_btn.setEnabled(False)
            self.capture_btn.setText("End Capture")
            self.capture_btn.setEnabled(enabled)

    def _on_connect_clicked(self):
        if self._state == "disconnected":
            self.camera_command_requested.emit("connect")
        elif self._state == "connected":
            self.camera_command_requested.emit("disconnect")

    def _on_capture_clicked(self):
        if self._state == "connected":
            self.camera_command_requested.emit("capture")
        elif self._state == "capturing":
            self.camera_command_requested.emit("end_capture")


class MessageLogTab(QWidget):
    def __init__(self, parent=None):
        super().__init__(parent)
        layout = QVBoxLayout(self)
        self._log = QPlainTextEdit()
        self._log.setReadOnly(True)
        clear_btn = QPushButton("Clear")
        clear_btn.clicked.connect(self._log.clear)
        layout.addWidget(self._log)
        layout.addWidget(clear_btn)

    def append(self, direction: str, msg: dict):
        ts = datetime.now().strftime("%H:%M:%S.%f")[:-3]
        self._log.appendPlainText(f"[{ts}] {direction}")
        self._log.appendPlainText(json.dumps(msg, indent=2))
        self._log.appendPlainText("-" * 60)
        sb = self._log.verticalScrollBar()
        sb.setValue(sb.maximum())


# ---------------------------------------------------------------------------
# Main window
# ---------------------------------------------------------------------------

# Tags used to route ZmqWorker responses back to the right handler
TAG_STATUS         = "status"
TAG_CONFIG         = "request_configuration"
TAG_COMMANDS       = "request_commands"
TAG_CONFIGURE      = "configure"
TAG_EXECUTE        = "execute"
TAG_CAM_CONFIG     = "cam_config"
TAG_CAM_STATUS     = "cam_status"
TAG_CAM_CONFIGURE  = "cam_configure"


class MainWindow(QMainWindow):
    def __init__(self):
        super().__init__()
        self.setWindowTitle("Odin Data GUI")
        self.resize(1200, 800)

        self._endpoint: str | None = None
        self._camera_endpoint: str | None = None
        self._camera_enabled: bool = False
        self._main_plugin: str | None = None
        self._commands_fetched: bool = False  # only fetch commands once per connection
        self._liveview_host: str = "localhost"
        self._pending_configures: int = 0   # count of in-flight configure/execute messages
        self._refresh_timer = QTimer(self)
        self._refresh_timer.timeout.connect(self._refresh)

        # Single background ZMQ worker for all I/O
        self._worker = ZmqWorker(self)
        self._worker.response_ready.connect(self._on_response)
        self._worker.start()
        log.debug("MainWindow: ZmqWorker started")

        self._build_ui()

    # ------------------------------------------------------------------
    # UI construction
    # ------------------------------------------------------------------

    def _build_ui(self):
        central = QWidget()
        root = QVBoxLayout(central)

        # --- Toolbar ---
        toolbar = QHBoxLayout()

        self._endpoint_input = QLineEdit()
        self._endpoint_input.setPlaceholderText("tcp://192.168.0.46:5000")
        self._endpoint_input.setText("tcp://192.168.0.46:5000")
        self._endpoint_input.setMinimumWidth(220)

        self._connect_btn = QPushButton("Connect")
        self._connect_btn.clicked.connect(self._on_connect)

        self._status_indicator = QLabel("● Disconnected")
        self._status_indicator.setStyleSheet("color: red;")

        self._refresh_btn = QPushButton("Refresh")
        self._refresh_btn.clicked.connect(self._refresh)
        self._refresh_btn.setEnabled(False)

        self._auto_refresh_btn = QPushButton("Auto Refresh: Off")
        self._auto_refresh_btn.setCheckable(True)
        self._auto_refresh_btn.clicked.connect(self._toggle_auto_refresh)
        self._auto_refresh_btn.setEnabled(False)

        self._interval_spin = QSpinBox()
        self._interval_spin.setRange(500, 60000)
        self._interval_spin.setSuffix(" ms")
        self._interval_spin.setValue(5000)
        self._interval_spin.valueChanged.connect(self._update_timer)

        toolbar.addWidget(QLabel("Endpoint:"))
        toolbar.addWidget(self._endpoint_input)
        toolbar.addWidget(self._connect_btn)
        toolbar.addWidget(self._status_indicator)
        toolbar.addStretch()
        toolbar.addWidget(self._refresh_btn)
        toolbar.addWidget(self._auto_refresh_btn)
        toolbar.addWidget(QLabel("Interval:"))
        toolbar.addWidget(self._interval_spin)

        root.addLayout(toolbar)

        # --- Plugin name label ---
        self._plugin_label = QLabel("Main plugin: not detected")
        root.addWidget(self._plugin_label)

        # --- Tabs ---
        self._tabs = QTabWidget()

        self._status_tab = StatusConfigTab()
        self._status_tab.configure_requested.connect(self._send_configure)

        self._hdf_tab = HdfAcquisitionTab()
        self._hdf_tab.acquisition_requested.connect(self._start_acquisition)
        self._hdf_tab.stop_requested.connect(self._stop_acquisition)

        self._dpdk_tab = PluginCommandsTab()
        self._dpdk_tab.command_requested.connect(
            lambda plugin, cmd: self._send_execute(cmd, plugin=plugin)
        )

        self._mode_tab = DecoderModeTab()
        self._mode_tab.mode_change_requested.connect(self._change_mode)

        self._liveview_tab = LiveviewTab()

        self._camera_tab = CameraControlTab()
        self._camera_tab.camera_command_requested.connect(self._send_camera_command)
        self._camera_tab.camera_property_changed.connect(self._send_camera_property)
        self._camera_tab.camera_config_requested.connect(self._refresh_camera)
        self._camera_tab.camera_enabled_changed.connect(self._on_camera_enabled)

        self._log_tab = MessageLogTab()

        self._tabs.addTab(self._status_tab, "Status / Config")
        self._tabs.addTab(self._hdf_tab, "HDF Acquisition")
        self._tabs.addTab(self._dpdk_tab, "Plugin Commands")
        self._tabs.addTab(self._mode_tab, "Decoder Mode")
        self._tabs.addTab(self._liveview_tab, "Liveview")
        self._tabs.addTab(self._camera_tab, "Camera Control")
        self._tabs.addTab(self._log_tab, "Message Log")

        root.addWidget(self._tabs)
        self.setCentralWidget(central)

    # ------------------------------------------------------------------
    # Connection
    # ------------------------------------------------------------------

    def _on_connect(self):
        if self._endpoint is not None:
            self._disconnect()
            return

        endpoint = self._endpoint_input.text().strip()
        if not endpoint:
            return

        base = endpoint.rsplit(":", 1)[0]
        self._endpoint = endpoint
        self._camera_endpoint = f"{base}:{CAMERA_PORT}"
        self._liveview_host = endpoint.split("//")[-1].split(":")[0]

        log.debug("MainWindow: connecting to %s (camera: %s)", self._endpoint, self._camera_endpoint)

        self._connect_btn.setText("Disconnect")
        self._status_indicator.setText("● Connected")
        self._status_indicator.setStyleSheet("color: green;")
        self._refresh_btn.setEnabled(True)
        self._auto_refresh_btn.setEnabled(True)

        self._refresh()

    def _disconnect(self):
        log.debug("MainWindow: disconnecting")
        if self._refresh_timer.isActive():
            self._refresh_timer.stop()
            self._auto_refresh_btn.setChecked(False)
            self._auto_refresh_btn.setText("Auto Refresh: Off")

        self._liveview_tab.stop()

        if self._endpoint:
            self._worker.close_endpoint(self._endpoint)
        if self._camera_endpoint:
            self._worker.close_endpoint(self._camera_endpoint)

        self._endpoint = None
        self._camera_endpoint = None
        self._camera_enabled = False
        self._main_plugin = None
        self._commands_fetched = False
        self._pending_configures = 0
        self._plugin_label.setText("Main plugin: not detected")
        self._connect_btn.setText("Connect")
        self._status_indicator.setText("● Disconnected")
        self._status_indicator.setStyleSheet("color: red;")
        self._refresh_btn.setEnabled(False)
        self._auto_refresh_btn.setEnabled(False)
        self._camera_tab.enable_checkbox.setChecked(False)

    # ------------------------------------------------------------------
    # Response dispatcher (called on main thread via signal)
    # ------------------------------------------------------------------

    def _on_response(self, tag: str, sent: dict, resp):
        log.debug("MainWindow._on_response: tag=%s resp=%s", tag, "OK" if resp else "TIMEOUT")

        # Always log sent message; log response if we got one
        direction_out = f"→ {tag}"
        self._log_tab.append(direction_out, sent)
        if resp is None:
            self._log_tab.append(f"← {tag} [TIMEOUT]", {})
            return
        self._log_tab.append(f"← {tag}", resp)

        params = resp.get("params", {})

        if tag == TAG_STATUS:
            self._status_tab.update_status(params)
            self._try_detect_plugin(params)
            for key in ("hdf", "HDF"):
                if key in params:
                    self._hdf_tab.update_hdf_status(params[key])
                    break
            if self._main_plugin and self._main_plugin in params:
                plugin_status = params[self._main_plugin]
                mode = plugin_status.get("mode")
                if mode:
                    self._mode_tab.set_current_mode(mode)
                available = (plugin_status.get("available_modes")
                             or plugin_status.get("modes"))
                if available and isinstance(available, list):
                    self._mode_tab.set_available_modes(available)

        elif tag == TAG_CONFIG:
            self._status_tab.update_config(params)
            self._try_detect_plugin(params)
            self._check_liveview_config(params)
            # Fetch commands once per connection when plugin is known
            if self._main_plugin and not self._commands_fetched:
                self._commands_fetched = True
                self._fetch_commands()

        elif tag == TAG_COMMANDS:
            log.debug("MainWindow: plugin commands: %s", list(params.keys()) if params else [])
            self._dpdk_tab.update_commands(params or {})

        elif tag == TAG_CONFIGURE:
            self._pending_configures = max(0, self._pending_configures - 1)
            if self._pending_configures == 0:
                QTimer.singleShot(500, self._refresh)

        elif tag == TAG_EXECUTE:
            self._pending_configures = max(0, self._pending_configures - 1)
            if self._pending_configures == 0:
                QTimer.singleShot(500, self._refresh)

        elif tag == TAG_CAM_CONFIG:
            self._camera_tab.update_properties(params)

        elif tag == TAG_CAM_STATUS:
            camera_status = (
                params.get("status", {}).get("camera_status")
                or params.get("camera_status")
            )
            if camera_status:
                self._camera_tab.set_state(camera_status)

        elif tag == TAG_CAM_CONFIGURE:
            pass  # state already updated optimistically

    # ------------------------------------------------------------------
    # Refresh
    # ------------------------------------------------------------------

    def _refresh(self):
        if not self._endpoint:
            return
        log.debug("MainWindow._refresh: queuing status + config")
        self._worker.request(ZmqRequest(TAG_STATUS, self._endpoint, "cmd", "status"))
        self._worker.request(ZmqRequest(TAG_CONFIG, self._endpoint, "cmd", "request_configuration"))
        self._refresh_camera()

    def _fetch_commands(self):
        if not self._endpoint or not self._main_plugin:
            return
        log.debug("MainWindow._fetch_commands: queuing request_commands")
        self._worker.request(ZmqRequest(TAG_COMMANDS, self._endpoint, "cmd", "request_commands"))

    def _on_camera_enabled(self, enabled: bool):
        self._camera_enabled = enabled
        log.debug("MainWindow: camera enabled = %s", enabled)
        if enabled and self._camera_endpoint:
            self._refresh_camera()

    def _refresh_camera(self):
        if not self._camera_endpoint or not self._camera_enabled:
            return
        log.debug("MainWindow._refresh_camera: queuing camera requests")
        self._worker.request(ZmqRequest(TAG_CAM_CONFIG, self._camera_endpoint, "cmd", "request_configuration"))
        self._worker.request(ZmqRequest(TAG_CAM_STATUS, self._camera_endpoint, "cmd", "status"))

    # ------------------------------------------------------------------
    # Plugin detection
    # ------------------------------------------------------------------

    def _try_detect_plugin(self, params: dict):
        if self._main_plugin:
            return
        names = params.get("plugins", {}).get("names", [])
        name = _detect_main_plugin(names)
        if name:
            log.debug("MainWindow: detected main plugin: %s", name)
            self._main_plugin = name
            self._plugin_label.setText(f"Main plugin: {name}")

    # ------------------------------------------------------------------
    # Liveview config detection
    # ------------------------------------------------------------------

    def _check_liveview_config(self, params: dict):
        lv = params.get("Liveview", {})
        addr = lv.get("live_view_socket_addr", "")
        if addr:
            port = addr.split(":")[-1]
            endpoint = f"tcp://{self._liveview_host}:{port}"
            log.debug("MainWindow: liveview endpoint: %s", endpoint)
            self._liveview_tab.set_endpoint(endpoint)

        # Extract HDF dataset dtype+dims and pass to liveview tab
        try:
            datasets = params.get("hdf", {}).get("dataset", {})
            for ds_name, ds_config in datasets.items():
                dims = ds_config.get("dims")
                dtype = ds_config.get("datatype")
                if dims:
                    log.debug("MainWindow: HDF dataset '%s' dtype=%s dims=%s", ds_name, dtype, dims)
                    self._liveview_tab.set_hdf_config(dtype, dims)
                    break
        except Exception:
            pass

    # ------------------------------------------------------------------
    # Configure
    # ------------------------------------------------------------------

    def _send_configure(self, params: dict):
        if not self._endpoint:
            return
        log.debug("MainWindow._send_configure: %s", list(params.keys()))
        self._pending_configures += 1
        self._worker.request(ZmqRequest(
            TAG_CONFIGURE, self._endpoint, "cmd", "configure",
            params=params, timeout_ms=CONFIGURE_TIMEOUT_MS
        ))

    # ------------------------------------------------------------------
    # Execute (DPDK commands)
    # ------------------------------------------------------------------

    def _send_execute(self, command: str, plugin: str | None = None):
        if not self._endpoint or not self._main_plugin:
            log.warning("MainWindow._send_execute: no endpoint or plugin, skipping %s", command)
            return
        target = plugin or self._main_plugin
        log.debug("MainWindow._send_execute: %s (plugin=%s)", command, target)
        self._pending_configures += 1
        params = {target: {"command": command}}
        self._worker.request(ZmqRequest(
            TAG_EXECUTE, self._endpoint, "cmd", "execute",
            params=params, timeout_ms=CONFIGURE_TIMEOUT_MS
        ))

    # ------------------------------------------------------------------
    # Acquisition
    # ------------------------------------------------------------------

    def _start_acquisition(self, path: str, acq_id: str, frames: int):
        if not self._endpoint or not self._main_plugin:
            log.warning("MainWindow._start_acquisition: no endpoint or plugin")
            return
        log.debug("MainWindow._start_acquisition: path=%s acq_id=%s frames=%d", path, acq_id, frames)

        self._send_configure({
            self._main_plugin: {
                "update_config": True,
                "rx_enable": False,
                "proc_enable": True,
                "rx_frames": frames,
            },
            "hdf": {"write": False},
        })
        self._send_configure({
            self._main_plugin: {
                "update_config": True,
                "rx_enable": False,
                "proc_enable": True,
                "rx_frames": frames,
            },
            "hdf": {
                "file": {"path": path},
                "frames": frames,
                "acquisition_id": acq_id,
                "write": True,
            },
        })
        self._send_configure({
            self._main_plugin: {"update_config": True, "rx_enable": False, "proc_enable": True},
            "hdf": {"write": False},
        })
        self._send_configure({
            self._main_plugin: {"update_config": True, "rx_enable": True},
            "hdf": {"write": True},
        })
        self._send_execute("arm")
        self._send_execute("start_capture")
        self._send_execute("start_writing", plugin="hdf")

    def _stop_acquisition(self):
        log.debug("MainWindow._stop_acquisition")
        self._send_execute("stop_capture")
        self._send_execute("stop_writing", plugin="hdf")

    # ------------------------------------------------------------------
    # Decoder mode
    # ------------------------------------------------------------------

    def _change_mode(self, mode: str, mode_config: dict):
        if not self._endpoint or not self._main_plugin:
            return
        log.debug("MainWindow._change_mode: %s", mode)
        params = {self._main_plugin: {"mode": mode}}
        if "hdf" in mode_config:
            params["hdf"] = mode_config["hdf"]
        self._send_configure(params)

    # ------------------------------------------------------------------
    # Camera
    # ------------------------------------------------------------------

    def _send_camera_command(self, command: str):
        if not self._camera_endpoint:
            return
        log.debug("MainWindow._send_camera_command: %s", command)
        self._worker.request(ZmqRequest(
            TAG_CAM_CONFIGURE, self._camera_endpoint, "cmd", "configure",
            params={"command": command}, timeout_ms=CONFIGURE_TIMEOUT_MS
        ))
        state_map = {
            "connect": "connected",
            "disconnect": "disconnected",
            "capture": "capturing",
            "end_capture": "connected",
        }
        if command in state_map:
            self._camera_tab.set_state(state_map[command])

    def _send_camera_property(self, path: str, value):
        if not self._camera_endpoint or value is None:
            return
        parts = path.split(".")
        nested = value
        for part in reversed(parts[1:]):
            nested = {part: nested}
        params = {parts[0]: nested}
        log.debug("MainWindow._send_camera_property: %s = %s", path, value)
        self._worker.request(ZmqRequest(
            TAG_CAM_CONFIGURE, self._camera_endpoint, "cmd", "configure",
            params=params, timeout_ms=CONFIGURE_TIMEOUT_MS
        ))

    # ------------------------------------------------------------------
    # Auto-refresh timer
    # ------------------------------------------------------------------

    def _toggle_auto_refresh(self, checked: bool):
        if checked:
            self._auto_refresh_btn.setText("Auto Refresh: On")
            self._update_timer()
        else:
            self._auto_refresh_btn.setText("Auto Refresh: Off")
            self._refresh_timer.stop()

    def _update_timer(self):
        if self._auto_refresh_btn.isChecked():
            self._refresh_timer.start(self._interval_spin.value())

    # ------------------------------------------------------------------
    # Cleanup
    # ------------------------------------------------------------------

    def closeEvent(self, event):
        log.debug("MainWindow: closing")
        self._liveview_tab.stop()
        self._worker.stop()
        super().closeEvent(event)


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------

if __name__ == "__main__":
    # Ensure pyqtgraph uses PyQt5 backend
    pg.setConfigOption("background", "k")
    pg.setConfigOption("foreground", "w")

    app = QApplication(sys.argv)
    app.setApplicationName("Odin Data GUI")
    win = MainWindow()
    win.show()
    sys.exit(app.exec_())
