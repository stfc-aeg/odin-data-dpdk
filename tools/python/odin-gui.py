import sys
import json
import zmq
from datetime import datetime
from PyQt5.QtWidgets import (QApplication, QWidget, QVBoxLayout, QHBoxLayout, QPushButton, QTextEdit,
                             QLineEdit, QLabel, QComboBox, QSpinBox, QSplitter, QDialog, QFormLayout,
                             QTreeWidget, QTreeWidgetItem, QTabWidget, QHeaderView, QGroupBox, QSizePolicy)
from PyQt5.QtCore import QTimer, Qt, pyqtSignal

import liveviewer

class JsonTreeWidget(QTreeWidget):
    itemChanged = pyqtSignal(QTreeWidgetItem, int)

    def __init__(self, parent=None, editable=False):
        super().__init__(parent)
        self.setHeaderLabels(["Key", "Value"])
        self.setColumnWidth(0, 200)
        self.editable = editable
        self.itemChanged.connect(self.on_item_changed)

    def update_data(self, data):
        self.clear()
        self.add_json(data)

    def add_json(self, data, parent=None):
        if isinstance(data, dict):
            for key, value in data.items():
                item = QTreeWidgetItem(parent or self, [str(key), ""])
                if self.editable:
                    item.setFlags(item.flags() | Qt.ItemIsEditable)
                self.add_json(value, item)
        elif isinstance(data, list):
            for i, value in enumerate(data):
                item = QTreeWidgetItem(parent or self, [str(i), ""])
                if self.editable:
                    item.setFlags(item.flags() | Qt.ItemIsEditable)
                self.add_json(value, item)
        else:
            item = QTreeWidgetItem(parent or self, ["", str(data)])
            if self.editable:
                item.setFlags(item.flags() | Qt.ItemIsEditable)

        if parent is None:
            self.expandAll()

    def on_item_changed(self, item, column):
        print("config change")
        if column == 1 and self.editable:  # Only react to changes in the value column
            print("Valid")
            self.itemChanged.emit(item, column)

    def get_data(self):
        return self._get_item_data(self.invisibleRootItem())

    def _get_item_data(self, item):
        if item.childCount() == 0:
            return item.text(1)

        if item.text(0).isdigit():  # It's a list item
            return [self._get_item_data(item.child(i)) for i in range(item.childCount())]
        else:  # It's a dict item
            return {item.child(i).text(0): self._get_item_data(item.child(i)) for i in range(item.childCount())}


class AcquisitionDialog(QDialog):
    def __init__(self, parent=None):
        super().__init__(parent)
        self.setWindowTitle("Create Acquisition")
        layout = QFormLayout(self)

        self.path_input = QLineEdit(self)
        self.path_input.setText("/data/")
        self.filename_input = QLineEdit(self)
        self.filename_input.setText("odin-data-capture")
        self.frames_input = QLineEdit(self)
        self.frames_input.setText("1000")

        layout.addRow("File Path:", self.path_input)
        layout.addRow("File Name:", self.filename_input)
        layout.addRow("Number of Frames:", self.frames_input)

        self.start_button = QPushButton("Start", self)
        self.start_button.clicked.connect(self.accept)
        layout.addRow(self.start_button)


class TensorstoreDialog(QDialog):
    def __init__(self, parent=None):
        super().__init__(parent)
        self.setWindowTitle("Add New Dataset")
        layout = QFormLayout(self)

        self.path_input = QLineEdit(self)
        self.path_input.setText("/data0/tensorstore/")
        self.filename_input = QLineEdit(self)
        self.filename_input.setText("odin-data-capture")
        self.frames_input = QLineEdit(self)
        self.frames_input.setText("1000")
        self.max_concurrent_frames_input = QLineEdit(self)
        self.max_concurrent_frames_input.setText("64")

        self.storage_driver_combo = QComboBox(self)
        self.storage_driver_combo.addItem("zarr3")

        self.kvstore_driver_combo = QComboBox(self)
        self.kvstore_driver_combo.addItem("file")
        self.kvstore_driver_combo.addItem("s3")
        self.kvstore_driver_combo.setCurrentText("file")

        from PyQt5.QtWidgets import QCheckBox
        self.enable_writing_checkbox = QCheckBox("Enable Writing", self)
        self.enable_writing_checkbox.setChecked(True)

        layout.addRow("File Path:", self.path_input)
        layout.addRow("File Name:", self.filename_input)
        layout.addRow("Number of Frames:", self.frames_input)
        layout.addRow("Max Concurrent Frames:", self.max_concurrent_frames_input)
        layout.addRow("Storage Driver:", self.storage_driver_combo)
        layout.addRow("KVStore Driver:", self.kvstore_driver_combo)
        layout.addRow("Enable Writing:", self.enable_writing_checkbox)
        

        self.start_button = QPushButton("Start", self)
        self.start_button.clicked.connect(self.accept)
        layout.addRow(self.start_button)


class ZmqOdinDataGUI(QWidget):
    def __init__(self):
        super().__init__()
        self.context = zmq.Context()
        self.sockets = {}
        self.camera_sockets = {}
        self.msg_id = 0
        self.refresh_timer = QTimer(self)
        self.refresh_timer.timeout.connect(self.refresh_status)
        self.liveview_port = None
        self.liveview_process = None
        self.camera_state = "disconnected"
        
        # Store the main plugin name dynamically
        self.main_plugin_name = None
        
        # Camera property configuration
        self.camera_properties = {}
        self.camera_property_widgets = {}
        
        # Define camera properties to display - MOVED BEFORE init_ui
        self.setup_camera_properties()
        
        # Now initialize the UI after the properties are set up
        self.init_ui()

    def init_ui(self):
        main_layout = QVBoxLayout()

        # Connection input
        conn_layout = QHBoxLayout()
        self.conn_input = QLineEdit()
        self.conn_input.setPlaceholderText("Enter endpoint")
        self.conn_input.setText("tcp://192.168.0.32:6000")
        conn_button = QPushButton("Connect")
        conn_button.clicked.connect(self.add_connection)
        conn_layout.addWidget(self.conn_input)
        conn_layout.addWidget(conn_button)
        main_layout.addLayout(conn_layout)

        # Command input and connection list
        cmd_layout = QHBoxLayout()
        self.cmd_input = QLineEdit()
        self.cmd_input.setPlaceholderText("Enter command")
        send_button = QPushButton("Send")
        send_button.clicked.connect(self.send_command)
        self.conn_list = QComboBox()
        self.conn_list.addItem("All Connections")
        cmd_layout.addWidget(self.conn_list)
        cmd_layout.addWidget(self.cmd_input)
        cmd_layout.addWidget(send_button)
        main_layout.addLayout(cmd_layout)

        # Status refresh and Auto-refresh
        refresh_layout = QHBoxLayout()
        refresh_button = QPushButton("Refresh Status")
        refresh_button.clicked.connect(self.refresh_status)
        self.auto_refresh_toggle = QPushButton("Auto Refresh: Off")
        self.auto_refresh_toggle.setCheckable(True)
        self.auto_refresh_toggle.clicked.connect(self.toggle_auto_refresh)
        self.refresh_interval = QSpinBox()
        self.refresh_interval.setRange(0, 60000)
        self.refresh_interval.setSuffix(" ms")
        self.refresh_interval.setValue(5000)
        self.refresh_interval.valueChanged.connect(self.update_refresh_timer)
        refresh_layout.addWidget(refresh_button)
        refresh_layout.addWidget(self.auto_refresh_toggle)
        refresh_layout.addWidget(QLabel("Auto-refresh interval:"))
        refresh_layout.addWidget(self.refresh_interval)
        main_layout.addLayout(refresh_layout)

        # Main vertical splitter for resizable sections
        main_vertical_splitter = QSplitter(Qt.Vertical)
        main_vertical_splitter.setChildrenCollapsible(False)  # Prevent sections from collapsing
        main_vertical_splitter.setHandleWidth(4)  # Make splitter handle more visible

        # Log display
        log_widget = QWidget()
        log_layout = QVBoxLayout(log_widget)
        log_layout.setContentsMargins(0, 0, 0, 0)
        log_layout.addWidget(QLabel("Log"))
        self.log_display = QTextEdit()
        self.log_display.setReadOnly(True)
        self.log_display.setMinimumHeight(50)  # Set minimum height
        log_layout.addWidget(self.log_display)
        main_vertical_splitter.addWidget(log_widget)

        # Plugin status area (no scroll wrapper)
        plugin_widget = QWidget()
        plugin_layout = QHBoxLayout(plugin_widget)

        # Liveview plugin
        liveview_widget = QGroupBox("Liveview Plugin")
        liveview_layout = QVBoxLayout()
        self.liveview_button = QPushButton("Start LiveView")
        self.liveview_button.clicked.connect(self.toggle_liveview)
        liveview_layout.addWidget(self.liveview_button)
        liveview_widget.setLayout(liveview_layout)
        plugin_layout.addWidget(liveview_widget)

        # HDF plugin
        hdf_widget = QGroupBox("HDF Plugin")
        hdf_layout = QVBoxLayout()
        self.hdf_writing_label = QLabel("Writing: N/A")
        self.hdf_frames_written_label = QLabel("Frames written: N/A")
        self.hdf_file_path_label = QLabel("File path: N/A")
        self.hdf_file_name_label = QLabel("File name: N/A")
        self.main_plugin_label = QLabel("Main Plugin: Not detected")
        acquisition_button = QPushButton("Create Acquisition")
        acquisition_button.clicked.connect(self.open_acquisition_dialog)
        tensorstore_button = QPushButton("Add New Dataset")
        tensorstore_button.clicked.connect(self.open_tensorstore_dialog)
        hdf_layout.addWidget(self.hdf_writing_label)
        hdf_layout.addWidget(self.hdf_frames_written_label)
        hdf_layout.addWidget(self.hdf_file_path_label)
        hdf_layout.addWidget(self.hdf_file_name_label)
        hdf_layout.addWidget(self.main_plugin_label)
        hdf_layout.addWidget(acquisition_button)
        hdf_layout.addWidget(tensorstore_button)
        hdf_widget.setLayout(hdf_layout)
        plugin_layout.addWidget(hdf_widget)

        # Camera Control section
        camera_widget = QGroupBox("Camera Control")
        camera_layout = QVBoxLayout()
        
        # Camera status label
        self.camera_status_label = QLabel("Camera Status: Disconnected")
        camera_layout.addWidget(self.camera_status_label)
        
        # Camera control buttons
        camera_buttons_layout = QHBoxLayout()
        
        self.camera_connect_button = QPushButton("Connect")
        self.camera_connect_button.clicked.connect(self.toggle_camera_connection)
        
        self.camera_capture_button = QPushButton("Start Capture")
        self.camera_capture_button.clicked.connect(self.toggle_camera_capture)
        self.camera_capture_button.setEnabled(False)
        
        camera_buttons_layout.addWidget(self.camera_connect_button)
        camera_buttons_layout.addWidget(self.camera_capture_button)
        
        camera_layout.addLayout(camera_buttons_layout)
        
        # Camera properties section with scroll area
        from PyQt5.QtWidgets import QScrollArea
        
        properties_group = QGroupBox("Camera Properties")
        properties_group_layout = QVBoxLayout(properties_group)
        
        properties_container = QWidget()
        self.properties_layout = QFormLayout(properties_container)
        
        # Create placeholder widgets for camera properties
        for prop_path, config in self.camera_property_config.items():
            display_name, prop_type, editable = config
            
            if prop_type == bool:
                widget = QComboBox()
                widget.addItems(["False", "True"])
                widget.setEnabled(editable)
            elif prop_type in (int, float):
                widget = QLineEdit()
                widget.setReadOnly(not editable)
            else:
                widget = QLineEdit()
                widget.setReadOnly(not editable)
                
            self.camera_property_widgets[prop_path] = widget
            self.properties_layout.addRow(display_name + ":", widget)
            
            if editable:
                if prop_type == bool:
                    widget.currentIndexChanged.connect(
                        lambda idx, path=prop_path: self.update_camera_property(path, idx == 1)
                    )
                else:
                    widget.editingFinished.connect(
                        lambda path=prop_path, widget=widget: self.update_camera_property(
                            path, self.convert_property_value(widget.text(), self.camera_property_config[path][1])
                        )
                    )
        
        # Create scroll area for properties only (without refresh button)
        properties_scroll = QScrollArea()
        properties_scroll.setWidget(properties_container)
        properties_scroll.setWidgetResizable(True)
        properties_scroll.setVerticalScrollBarPolicy(Qt.ScrollBarAsNeeded)
        properties_scroll.setHorizontalScrollBarPolicy(Qt.ScrollBarAsNeeded)

        # Let it expand when there is room, but allow scrolling when constrained
        properties_scroll.setSizePolicy(QSizePolicy.Expanding, QSizePolicy.Expanding)

        # Add scroll area to properties group
        properties_group_layout.addWidget(properties_scroll)
        
        # Add refresh button outside the scroll area
        refresh_props_button = QPushButton("Refresh Properties")
        refresh_props_button.clicked.connect(self.request_camera_config)
        properties_group_layout.addWidget(refresh_props_button)
        
        camera_layout.addWidget(properties_group)
        
        camera_widget.setLayout(camera_layout)
        plugin_layout.addWidget(camera_widget)

        # Set maximum height for plugin widget so it doesn't grow unnecessarily large
        plugin_widget.setMaximumHeight(550)  # Adjust this value as needed

        # Add plugin widget directly to splitter (no scroll wrapper)
        main_vertical_splitter.addWidget(plugin_widget)

        # Horizontal splitter for Config and Status
        config_status_splitter = QSplitter(Qt.Horizontal)
        config_status_splitter.setChildrenCollapsible(False)  # Prevent sections from collapsing
        config_status_splitter.setHandleWidth(8)  # Make splitter handle wider for better spacing

        # Config display
        self.config_tree = JsonTreeWidget(editable=True)
        self.config_tree.itemChanged.connect(self.on_config_changed)
        self.config_tree.setMinimumWidth(100)  # Set minimum width
        config_widget = QWidget()
        config_layout = QVBoxLayout(config_widget)
        config_layout.setContentsMargins(0, 0, 0, 0)
        config_layout.addWidget(QLabel("Configuration"))
        config_layout.addWidget(self.config_tree)
        config_status_splitter.addWidget(config_widget)

        # Status display
        self.status_tree = JsonTreeWidget()
        self.status_tree.setMinimumWidth(100)  # Set minimum width
        status_widget = QWidget()
        status_layout = QVBoxLayout(status_widget)
        status_layout.setContentsMargins(0, 0, 0, 0)
        status_layout.addWidget(QLabel("Status"))
        status_layout.addWidget(self.status_tree)
        config_status_splitter.addWidget(status_widget)

        # Set minimum height for config/status section
        config_status_splitter.setMinimumHeight(100)

        # Add config/status splitter to lower section of main vertical splitter
        main_vertical_splitter.addWidget(config_status_splitter)

        # Set stretch factors for the vertical splitter
        main_vertical_splitter.setStretchFactor(0, 1)  # Log (flexible)
        main_vertical_splitter.setStretchFactor(1, 0)  # Plugin section (fixed-ish)
        main_vertical_splitter.setStretchFactor(2, 3)  # Config/Status (most flexible)

        # Set initial sizes for the vertical splitter
        main_vertical_splitter.setSizes([150, 300, 350])

        # Add the vertical splitter to the layout
        main_layout.addWidget(main_vertical_splitter)

        self.setLayout(main_layout)
        self.setWindowTitle('ZMQ Odin Data GUI Client')
        self.setGeometry(300, 300, 1200, 900)

        # Set up refresh timer
        self.refresh_timer = QTimer(self)
        self.refresh_timer.timeout.connect(self.refresh_status)
        self.update_refresh_timer()

    def toggle_auto_refresh(self):
        if self.auto_refresh_toggle.isChecked():
            self.auto_refresh_toggle.setText("Auto Refresh: On")
            self.update_refresh_timer()
        else:
            self.auto_refresh_toggle.setText("Auto Refresh: Off")
            self.refresh_timer.stop()

    def parse_status(self, status):
        plugins = []
        if 'plugins' in status and 'names' in status['plugins']:
            plugins = status['plugins']['names']
        return plugins

    def add_connection(self):
        endpoint = self.conn_input.text().strip()
        if endpoint and endpoint not in self.sockets:
            try:
                # Extract base address without port
                base_addr = endpoint.rsplit(':', 1)[0]
                camera_endpoint = f"{base_addr}:9001"
                
                # Create main socket
                socket = self.context.socket(zmq.DEALER)
                socket.connect(endpoint)
                self.sockets[endpoint] = socket
                
                # Create camera socket
                camera_socket = self.context.socket(zmq.DEALER)
                camera_socket.connect(camera_endpoint)
                self.camera_sockets[endpoint] = camera_socket
                
                self.conn_list.addItem(endpoint)
                self.conn_input.clear()
                self.log_message(f"Connected to {endpoint}")
                self.log_message(f"Connected camera control to {camera_endpoint}")
            except zmq.error.ZMQError as e:
                self.log_message(f"Failed to connect to {endpoint}: {str(e)}")

    def send_camera_command(self, command):
        """Send a command to the camera."""
        selected = self.conn_list.currentText()
        if selected == "All Connections":
            self.log_message("Please select a specific connection for camera commands.")
            return
        
        if selected not in self.camera_sockets:
            self.log_message(f"No camera connection for {selected}.")
            return
        
        try:
            # Format the command as specified
            command_obj = {
                "command": command
            }
            command_msg = {
                "params": command_obj
            }
            
            # Create and send the message
            msg = self.create_message('cmd', 'configure')
            msg.update(command_msg)
            
            self.camera_sockets[selected].send_json(msg)
            self.log_message(f"Send camera command: {json.dumps(msg, indent=2)}")
            
            # Wait for response
            if self.camera_sockets[selected].poll(5000):
                response = self.camera_sockets[selected].recv_json()
                self.log_message(f"Camera response: {json.dumps(response, indent=2)}")
                
                # Update state based on command
                self.update_camera_state_from_command(command)
            else:
                self.log_message("No response from camera within timeout.")
        except Exception as e:
            self.log_message(f"Error sending camera command: {str(e)}")

    def update_camera_state_from_command(self, command):
        """Update camera state based on sent command."""
        if command == "connect":
            self.update_camera_state("connected")
        elif command == "disconnect":
            self.update_camera_state("disconnected")
        elif command == "capture":
            self.update_camera_state("capturing")
        elif command == "end_capture":
            self.update_camera_state("connected")

    def update_camera_state(self, new_state):
        """Update camera state and refresh UI accordingly."""
        self.camera_state = new_state
        self.update_camera_ui()
        self.log_message(f"Camera state updated to: {new_state}")

    def update_camera_ui(self):
        """Update camera UI elements based on current state."""
        # Update status label
        self.camera_status_label.setText(f"Camera Status: {self.camera_state.capitalize()}")
        
        # Update connect/disconnect button
        if self.camera_state == "disconnected":
            self.camera_connect_button.setText("Connect")
            self.camera_connect_button.setEnabled(True)
            self.camera_capture_button.setText("Start Capture")
            self.camera_capture_button.setEnabled(False)
        elif self.camera_state == "connected":
            self.camera_connect_button.setText("Disconnect")
            self.camera_connect_button.setEnabled(True)
            self.camera_capture_button.setText("Start Capture")
            self.camera_capture_button.setEnabled(True)
        elif self.camera_state == "capturing":
            self.camera_connect_button.setText("Disconnect")
            self.camera_connect_button.setEnabled(False)  # Can't disconnect while capturing
            self.camera_capture_button.setText("End Capture")
            self.camera_capture_button.setEnabled(True)

    def toggle_camera_connection(self):
        """Handle connect/disconnect button click."""
        if self.camera_state == "disconnected":
            self.send_camera_command("connect")
        elif self.camera_state == "connected":
            self.send_camera_command("disconnect")

    def toggle_camera_capture(self):
        """Handle start/end capture button click."""
        if self.camera_state == "connected":
            self.send_camera_command("capture")
        elif self.camera_state == "capturing":
            self.send_camera_command("end_capture")

    def create_plugin_status_widgets(self, plugins):
        for plugin in plugins:
            if plugin not in self.plugin_status_widgets:
                widget = QWidget()
                layout = QVBoxLayout(widget)

                if plugin == "Liveview":
                    self.liveview_button = QPushButton("Start LiveView")
                    self.liveview_button.clicked.connect(self.toggle_liveview)
                    layout.addWidget(self.liveview_button)
                elif plugin == "hdf":
                    self.hdf_writing_label = QLabel("Writing: N/A")
                    self.hdf_frames_written_label = QLabel("Frames written: N/A")
                    self.hdf_file_path_label = QLabel("File path: N/A")
                    self.hdf_file_name_label = QLabel("File name: N/A")
                    layout.addWidget(self.hdf_writing_label)
                    layout.addWidget(self.hdf_frames_written_label)
                    layout.addWidget(self.hdf_file_path_label)
                    layout.addWidget(self.hdf_file_name_label)

                self.plugin_status_widgets[plugin] = widget
                self.plugin_status_layout.addWidget(widget)

    def update_plugin_status(self, status):
        if 'hdf' in status:
            hdf_status = status['hdf']
            self.hdf_writing_label.setText(f"Writing: {hdf_status.get('writing', 'N/A')}")
            self.hdf_frames_written_label.setText(f"Frames written: {hdf_status.get('frames_written', 'N/A')}")
            self.hdf_file_path_label.setText(f"File path: {hdf_status.get('file_path', 'N/A')}")
            self.hdf_file_name_label.setText(f"File name: {hdf_status.get('file_name', 'N/A')}")

    def send_command(self):
        command = self.cmd_input.text().strip()
        if not command:
            return

        selected = self.conn_list.currentText()
        targets = [selected] if selected != "All Connections" else self.sockets.keys()

        for target in targets:
            try:
                msg = self.create_message('cmd', command)
                self.sockets[target].send_json(msg)
                self.log_message(f"Sent to {target}: {json.dumps(msg, indent=2)}")

                if self.sockets[target].poll(5000):  # Wait for 5 seconds
                    response = self.sockets[target].recv_json()
                    self.log_message(f"Received from {target}: {json.dumps(response, indent=2)}")
                    self.update_displays(response)
                else:
                    self.log_message(f"No response from {target} within timeout.")
            except Exception as e:
                self.log_message(f"Error communicating with {target}: {str(e)}")

        self.cmd_input.clear()

    def check_camera_status(self):
        """Check camera status using ZMQ."""
        selected = self.conn_list.currentText()
        if selected == "All Connections" or selected not in self.camera_sockets:
            return
        
        try:
            # Create status request
            msg = self.create_message('cmd', 'status')
            self.camera_sockets[selected].send_json(msg)
            self.log_message(f"Get camera status: {json.dumps(msg, indent=2)}")
            
            if self.camera_sockets[selected].poll(5000):
                response = self.camera_sockets[selected].recv_json()
                self.log_message(f"Camera status: {json.dumps(response, indent=2)}")
                
                # Extract camera status from response
                if ('params' in response and 
                    'status' in response['params'] and 
                    'camera_status' in response['params']['status']):
                    camera_status = response['params']['status']['camera_status']
                    self.update_camera_state(camera_status)
            else:
                self.log_message("No camera status response within timeout.")
        except Exception as e:
            self.log_message(f"Error checking camera status: {str(e)}")


    def setup_camera_properties(self):
        """Configure which camera properties to display and how."""
        # Format: property_path: (display_name, property_type, editable)
        self.camera_property_config = {
            "camera.exposure_time": ("Exposure Time (s)", float, True),
            "camera.image_timeout": ("Image Timeout (s)", float, True),
            "camera.num_frames": ("Number of Frames", int, True),
            "camera.frames_per_second": ("Frames per second", int, True),
            "camera.camera_number": ("Camera Number", int, False),
            "camera.simulated_camera": ("Simulated Camera", bool, False),
            "camera.timestamp_mode": ("Timestamp Mode", int, True),
            "camera.trigger_active": ("Trigger Active", int, True),
            "camera.trigger_connector": ("Trigger Connector", int, True),
            "camera.trigger_mode": ("Trigger Mode", int, True),
            "camera.trigger_polarity": ("Trigger Polarity", int, True),
            "camera.trigger_source": ("Trigger Source", int, True)
        }

    def request_camera_config(self):
        """Request camera configuration from the camera ZMQ connection."""
        selected = self.conn_list.currentText()
        if selected == "All Connections" or selected not in self.camera_sockets:
            return
        
        try:
            # Create config request
            msg = self.create_message('cmd', 'request_configuration')
            self.log_message(f"Get camera configuration: {json.dumps(msg, indent=2)}")
            self.camera_sockets[selected].send_json(msg)
            
            if self.camera_sockets[selected].poll(5000):
                response = self.camera_sockets[selected].recv_json()
                self.log_message(f"Camera configuration: {json.dumps(response, indent=2)}")
                
                # Update camera properties from response
                if 'params' in response:
                    self.update_camera_properties_display(response['params'])
            else:
                self.log_message("No camera config response within timeout.")
        except Exception as e:
            self.log_message(f"Error requesting camera config: {str(e)}")

    def update_camera_properties_display(self, config_data):
        """Update the camera properties display with data from config response."""
        # Store the current configuration
        self.camera_properties = config_data
        
        # Update each property widget
        for prop_path, widget in self.camera_property_widgets.items():
            # Split path to navigate nested dict
            path_parts = prop_path.split('.')
            value = config_data
            
            # Navigate to the property in the nested dictionary
            try:
                for part in path_parts:
                    value = value[part]
                
                # Update widget based on its type
                if isinstance(widget, QComboBox):
                    # Boolean property
                    widget.setCurrentIndex(1 if value else 0)
                elif isinstance(widget, QLineEdit):
                    # Numeric or string property
                    widget.setText(str(value))
            except (KeyError, TypeError):
                # Property not found in config
                if isinstance(widget, QComboBox):
                    widget.setCurrentIndex(0)
                elif isinstance(widget, QLineEdit):
                    widget.setText("N/A")

    def convert_property_value(self, text_value, property_type):
        """Convert text input to appropriate property type."""
        try:
            if property_type == bool:
                return text_value.lower() in ('true', '1', 'yes')
            elif property_type == int:
                return int(text_value)
            elif property_type == float:
                return float(text_value)
            else:
                return text_value
        except (ValueError, TypeError):
            self.log_message(f"Error converting value '{text_value}' to {property_type.__name__}")
            return None

    def update_camera_property(self, property_path, value):
        """Send updated property value to camera."""
        if value is None:
            return
            
        selected = self.conn_list.currentText()
        if selected == "All Connections" or selected not in self.camera_sockets:
            return
        
        # Split path to build nested structure
        path_parts = property_path.split('.')
        
        # Build nested structure for this property
        nested_value = value
        for part in reversed(path_parts[1:]):  # Skip the first level
            nested_value = {part: nested_value}
        
        # Add the top level
        config_update = {path_parts[0]: nested_value}
        
        try:
            # Format the command
            command_msg = {
                "params": config_update
            }
            
            # Create and send the message
            msg = self.create_message('cmd', 'configure')
            msg.update(command_msg)
            
            self.log_message(f"Sent : {json.dumps(msg, indent=2)}")
            self.camera_sockets[selected].send_json(msg)
            self.log_message(f"Sent camera property update: {property_path} = {value}")
            
            # Wait for response
            if self.camera_sockets[selected].poll(5000):
                response = self.camera_sockets[selected].recv_json()
                self.log_message(f"Camera property update response: {json.dumps(response, indent=2)}")
            else:
                self.log_message("No response from camera within timeout.")
        except Exception as e:
            self.log_message(f"Error updating camera property: {str(e)}")


    def add_camera_property(self, property_path, display_name, property_type=str, editable=False):
        """
        Dynamically add a new camera property to display.
        
        Args:
            property_path (str): Path to the property in the config (e.g., 'camera.new_property')
            display_name (str): Human-readable name to display in the UI
            property_type (type): Python type of the property (bool, int, float, str)
            editable (bool): Whether the property should be editable
        """
        # Add to property config
        self.camera_property_config[property_path] = (display_name, property_type, editable)
        
        # Create widget based on type
        if property_type == bool:
            widget = QComboBox()
            widget.addItems(["False", "True"])
            widget.setEnabled(editable)
            if editable:
                widget.currentIndexChanged.connect(
                    lambda idx, path=property_path: self.update_camera_property(path, idx == 1)
                )
        else:
            widget = QLineEdit()
            widget.setReadOnly(not editable)
            if editable:
                widget.editingFinished.connect(
                    lambda path=property_path, widget=widget: self.update_camera_property(
                        path, self.convert_property_value(widget.text(), property_type)
                    )
                )
        
        # Store widget and add to layout
        self.camera_property_widgets[property_path] = widget
        self.properties_layout.addRow(display_name + ":", widget)
        
        # Try to update with current value if available
        if self.camera_properties:
            try:
                # Navigate to property in nested dict
                path_parts = property_path.split('.')
                value = self.camera_properties
                
                for part in path_parts:
                    value = value[part]
                    
                # Update widget
                if property_type == bool:
                    widget.setCurrentIndex(1 if value else 0)
                else:
                    widget.setText(str(value))
            except (KeyError, TypeError):
                # Property not found in current config
                pass
        
        self.log_message(f"Added camera property display for {property_path}")

    def refresh_status(self):
        """Refresh status of all connected systems."""
        self.cmd_input.setText("status")
        self.send_command()
        self.refresh_config()
        self.check_camera_status()
        self.request_camera_config()

    def refresh_config(self):
        self.cmd_input.setText("request_configuration")
        self.send_command()

    def update_refresh_timer(self):
        interval = self.refresh_interval.value()
        if interval > 0 and self.auto_refresh_toggle.isChecked():
            self.refresh_timer.start(interval)
        else:
            self.refresh_timer.stop()

    def create_message(self, msg_type, msg_val):
        self.msg_id += 1
        return {
            'msg_type': msg_type,
            'msg_val': msg_val,
            'timestamp': datetime.now().isoformat(),
            'id': self.msg_id,
            'params': {}
        }

    def log_message(self, message):
        self.log_display.append(message)
        self.log_display.append('-' * 40)  # Separator
        self.log_display.verticalScrollBar().setValue(
            self.log_display.verticalScrollBar().maximum()
        )  # Scroll to bottom

    def toggle_liveview(self):
        if self.liveview_button.text() == "Start LiveView":
            if self.liveview_port is not None:
                self.start_liveview()
                self.liveview_button.setText("Stop LiveView")
            else:
                self.log_message("LiveView port not set. Cannot start LiveView.")
        else:
            self.stop_liveview()
            self.liveview_button.setText("Start LiveView")

    def start_liveview(self):
        if self.liveview_process is None:
            endpoint = f"tcp://192.168.0.32:{self.liveview_port}"
            self.liveview_process = liveviewer.LiveDataViewer(endpoint)
            self.liveview_process.start()
            self.log_message(f"Started LiveView on port {self.liveview_port}")

    def stop_liveview(self):
        if self.liveview_process is not None:
            self.liveview_process.stop()
            self.liveview_process = None
            self.log_message("Stopped LiveView")

    def extract_plugin_name(self, config):
        """Extract the main plugin name from configuration."""
        try:
            if 'plugins' in config and 'names' in config['plugins']:
                plugins = config['plugins']['names']
                if plugins and len(plugins) > 0:
                    # Get the first plugin in the list
                    return plugins[0]
        except (KeyError, IndexError, TypeError) as e:
            self.log_message(f"Error extracting plugin name: {str(e)}")
        return None

    def update_displays(self, response):
        if 'params' in response:
            if response['msg_val'] == 'request_configuration':
                self.config_tree.update_data(response['params'])
                self.check_liveview_config(response['params'])
                
                # Extract and store the main plugin name
                plugin_name = self.extract_plugin_name(response['params'])
                if plugin_name:
                    self.main_plugin_name = plugin_name
                    self.main_plugin_label.setText(f"Main Plugin: {plugin_name}")
                    self.log_message(f"Detected main plugin: {plugin_name}")
                else:
                    self.log_message("Could not detect main plugin name from configuration")
                    
            elif response['msg_val'] == 'status':
                self.status_tree.update_data(response['params'])
                self.update_plugin_status(response['params'])
                
                # Also try to extract plugin name from status if not already set
                if not self.main_plugin_name:
                    plugin_name = self.extract_plugin_name(response['params'])
                    if plugin_name:
                        self.main_plugin_name = plugin_name
                        self.main_plugin_label.setText(f"Main Plugin: {plugin_name}")
                        self.log_message(f"Detected main plugin from status: {plugin_name}")

    def check_liveview_config(self, config):
        if 'Liveview' in config:
            liveview_config = config['Liveview']
            if 'live_view_socket_addr' in liveview_config:
                socket_addr = liveview_config['live_view_socket_addr']
                port = socket_addr.split(':')[-1]
                self.liveview_port = int(port)
                self.liveview_button.setEnabled(True)
                self.log_message(f"LiveView port set to {self.liveview_port}")
            else:
                self.liveview_button.setEnabled(False)
                self.log_message("LiveView socket address not found in configuration")
        else:
            self.liveview_button.setEnabled(False)
            self.log_message("LiveView configuration not found")

    def on_config_changed(self, item, column):
        if column == 1:  # Value column
            key = item.parent().text(0)
            value = item.text(1)
            new_value = self.parse_new_value(value)
            #config_data = self.config_tree.get_data()
            #self.send_config_message({"params": config_data})
            self.send_config_message({"params": {key: new_value}})

    def parse_new_value(self, value):
        """Parse a string value to its appropriate type."""
        # Try to convert to int
        try:
            return int(value)
        except ValueError:
            pass
        
        # Try to convert to float
        try:
            return float(value)
        except ValueError:
            pass
        
        # Check for boolean
        if value.lower() in ('true', 'false'):
            return value.lower() == 'true'
        
        # Return as string
        return value

    def open_acquisition_dialog(self):
        # Check if we have a plugin name before proceeding
        if not self.main_plugin_name:
            self.log_message("Error: Main plugin name not detected. Please refresh configuration first.")
            return
            
        dialog = AcquisitionDialog(self)
        if dialog.exec_():
            path = dialog.path_input.text()
            acquisition_id = dialog.filename_input.text()
            frames = dialog.frames_input.text()
            self.start_acquisition(path, acquisition_id, frames)

    def open_tensorstore_dialog(self):
        # Check if we have a plugin name before proceeding
        if not self.main_plugin_name:
            self.log_message("Error: Main plugin name not detected. Please refresh configuration first.")
            return
        dialog = TensorstoreDialog(self)
        if dialog.exec_():
            path = dialog.path_input.text()
            acquisition_id = dialog.filename_input.text()
            number_of_frames = dialog.frames_input.text()
            self.number_of_frames = number_of_frames
            storage_driver = dialog.storage_driver_combo.currentText()
            kvstore_driver = dialog.kvstore_driver_combo.currentText()
            enable_writing = dialog.enable_writing_checkbox.isChecked()
            max_concurrent_frames = dialog.max_concurrent_frames_input.text()
            self.max_concurrent_frames = max_concurrent_frames
            self.storage_driver = storage_driver
            self.kvstore_driver = kvstore_driver
            self.enable_writing = enable_writing
            self.start_tensorstore_acquisition(path, acquisition_id)
    def start_acquisition(self, path, acquisition_id, frames):
        if self.acquisition(path, acquisition_id, frames):
            self.start_sequence()

    def acquisition(self, path, acquisition_id, frames):
        # Check if we have a plugin name
        if not self.main_plugin_name:
            self.log_message("Error: Main plugin name not set. Cannot start acquisition.")
            return False
            
        try:
            frames_count = int(frames)

            # Use the dynamically extracted plugin name
            common_config = {
                self.main_plugin_name: {
                    "update_config": True,
                    "rx_enable": False,
                    "proc_enable": True,
                    "rx_frames": frames_count,
    
                },
                "hdf": {
                    "write": False
                }
            }

            self.log_message(f"Sending first configuration for acquisition setup using plugin: {self.main_plugin_name}")
            if not self.send_config_message({"params": common_config}):
                self.log_message("Failed to send the first configuration message. Aborting acquisition setup.")
                return False

            second_config = {
                self.main_plugin_name: common_config[self.main_plugin_name],
                "hdf": {
                    "file": {
                        "path": path
                    },
                    "frames": frames_count,
                    "acquisition_id": acquisition_id,
                    "write": True
                }
            }

            self.log_message("Sending second configuration for acquisition setup.")
            if not self.send_config_message({"params": second_config}):
                self.log_message("Failed to send the second configuration message. Aborting acquisition setup.")
                return False

            self.log_message("Acquisition setup completed successfully.")

        except ValueError as e:
            self.log_message(f"Invalid frames count provided: {e}")
            return False

        return True

    def start_tensorstore_acquisition(self, path, acquisition_id):
        if self.tensorstore_acquisition(path, acquisition_id):
            self.start_sequence()

    def tensorstore_acquisition(self, path, acquisition_id):
        if not self.main_plugin_name:
            self.log_message("Error: Main plugin name not set. Cannot start Tensorstore acquisition.")
            return False
        try:
            storage_driver = self.storage_driver if hasattr(self, 'storage_driver') else "zarr3"
            kvstore_driver = self.kvstore_driver if hasattr(self, 'kvstore_driver') else "file"
            max_concurrent_frames = int(self.max_concurrent_frames) if hasattr(self, 'max_concurrent_frames') else 64
            number_of_frames = int(self.frames) if hasattr(self, 'frames') else 1000
            enable_writing = self.enable_writing if hasattr(self, 'enable_writing') else True

            common_config = {
                self.main_plugin_name: {
                    "update_config": True,
                    "path": path,
                    "storage_driver": storage_driver,
                    "kvstore_driver": kvstore_driver,
                    "max_concurrent_writes": max_concurrent_frames,
                    "number_of_frames": number_of_frames,
                    "enable_writing": enable_writing,
                }
            }

            self.log_message(f"Sending first Tensorstore configuration using plugin: {self.main_plugin_name}")
            if not self.send_config_message({"params": common_config}):
                self.log_message("Failed to send the first Tensorstore configuration message. Aborting acquisition setup.")
                return False

            self.log_message("Tensorstore acquisition setup completed successfully.")

        except ValueError as e:
            self.log_message(f"Invalid input provided: {e}")
            return False

        return True

    def start_sequence(self):
        # Check if we have a plugin name
        if not self.main_plugin_name:
            self.log_message("Error: Main plugin name not set. Cannot start sequence.")
            return False
            
        self.log_message(f"Starting the configuration sequence with plugin: {self.main_plugin_name}")

        first_config = {
            "params": {
                self.main_plugin_name: {
                    "update_config": True,
                    "rx_enable": False,
                    "proc_enable": True,

                },
                "hdf": {
                    "Writing": False
                }
            }
        }
        
        if self.send_config_message(first_config):
            second_config = {
                "params": {
                    self.main_plugin_name: {
                        "update_config": True,
                        "rx_enable": True
                    },
                    "hdf": {
                        "Writing": True
                    }
                }
            }
            self.send_config_message(second_config)
            self.log_message("Configuration sequence completed.")
            return True
        else:
            self.log_message("Failed to send the first configuration message. Aborting sequence.")
            return False

    def send_config_message(self, config):
        selected = self.conn_list.currentText()
        targets = [selected] if selected != "All Connections" else self.sockets.keys()

        all_responses_valid = True
        for target in targets:
            try:
                msg = self.create_message('cmd', 'configure')
                msg.update(config)
                self.sockets[target].send_json(msg)
                self.log_message(f"Sent configuration to {target}: {json.dumps(msg, indent=2)}")

                if self.sockets[target].poll(10000):  # Wait for 10 seconds
                    response = self.sockets[target].recv_json()
                    self.log_message(f"Received from {target}: {json.dumps(response, indent=2)}")
                    self.update_displays(response)
                else:
                    self.log_message(f"No response from {target} within timeout.")
                    all_responses_valid = False
            except Exception as e:
                self.log_message(f"Error communicating with {target}: {str(e)}")
                all_responses_valid = False

        return True


if __name__ == '__main__':
    app = QApplication(sys.argv)
    ex = ZmqOdinDataGUI()
    ex.show()
    try:
        sys.exit(app.exec_())
    finally:
        # Clean up
        if ex.liveview_process is not None:
            ex.stop_liveview()
        # Close all sockets
        for socket in ex.sockets.values():
            socket.close()
        for socket in ex.camera_sockets.values():
            socket.close()