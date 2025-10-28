import cv2
import numpy as np
import json
import logging
import time
import tkinter as tk
from tkinter import ttk
from matplotlib.figure import Figure
from matplotlib.backends.backend_tkagg import FigureCanvasTkAgg
from PIL import Image, ImageTk
from multiprocessing import Process, Queue, Pipe
import zmq
import os
os.environ.pop("QT_QPA_PLATFORM_PLUGIN_PATH")

class LiveDataViewer:
    def __init__(self, endpoint, resize=(2048, 1152), colour='PLASMA', histogram_bins=256,
                histogram_range=(0, 65535), histogram_log_scale=False, roi=None, clip=(0, 65535)):
        self.endpoint = endpoint
        self.resize = resize
        self.colour = colour
        self.histogram_bins = histogram_bins
        self.histogram_range = histogram_range
        self.histogram_log_scale = histogram_log_scale
        self.roi = roi or (0, 0, *resize)
        self.clip = clip
        self.config_pipe, self._config_pipe_child = Pipe()
        self.histogram_pipe, self._histogram_pipe_child = Pipe()
        self.image_queue = Queue(maxsize=1)
        self.processes = []

        # Get all OpenCV color maps
        self.color_maps = ['NONE'] + [attr for attr in dir(cv2) if attr.startswith('COLORMAP_')]

    def start(self):
        self.processes = [
            Process(target=self._create_gui),
            Process(target=self._capture_images),
            Process(target=self._display_latest_image)
        ]
        for process in self.processes:
            process.start()

    def stop(self):
        for process in self.processes:
            process.terminate()
        for process in self.processes:
            process.join()

    def _create_gui(self):
        root = tk.Tk()
        root.title("Configuration Settings")

        variables = {
            'Width': tk.StringVar(value=str(self.resize[0])),
            'Height': tk.StringVar(value=str(self.resize[1])),
            'Colour Map': tk.StringVar(value=self.colour),
            'Histogram Bins': tk.StringVar(value=str(self.histogram_bins)),
            'Histogram Range Min': tk.StringVar(value=str(self.histogram_range[0])),
            'Histogram Range Max': tk.StringVar(value=str(self.histogram_range[1])),
            'Histogram Log Scale': tk.BooleanVar(value=self.histogram_log_scale),
            'ROI x1': tk.StringVar(value=str(self.roi[0])),
            'ROI y1': tk.StringVar(value=str(self.roi[1])),
            'ROI x2': tk.StringVar(value=str(self.roi[2])),
            'ROI y2': tk.StringVar(value=str(self.roi[3])),
            'Clip Min': tk.StringVar(value=str(self.clip[0])),
            'Clip Max': tk.StringVar(value=str(self.clip[1]))
        }

        for i, (label, var) in enumerate(variables.items()):
            tk.Label(root, text=f"{label}:").grid(row=i, column=0, sticky='e')
            if label == 'Colour Map':
                ttk.Combobox(root, textvariable=var, values=self.color_maps).grid(row=i, column=1)
            elif isinstance(var, tk.BooleanVar):
                tk.Checkbutton(root, variable=var).grid(row=i, column=1)
            else:
                tk.Entry(root, textvariable=var).grid(row=i, column=1)

        def update_settings():
            config = {
                'size': (int(variables['Width'].get()), int(variables['Height'].get())),
                'colour': variables['Colour Map'].get(),
                'histogram_bins': int(variables['Histogram Bins'].get()),
                'histogram_range': (int(variables['Histogram Range Min'].get()), int(variables['Histogram Range Max'].get())),
                'histogram_log_scale': variables['Histogram Log Scale'].get(),
                'roi': (int(variables['ROI x1'].get()), int(variables['ROI y1'].get()),
                        int(variables['ROI x2'].get()), int(variables['ROI y2'].get())),
                'clip': (int(variables['Clip Min'].get()), int(variables['Clip Max'].get()))
            }
            self.config_pipe.send(config)

        tk.Button(root, text="Update Settings", command=update_settings).grid(row=len(variables), column=0, columnspan=2)
        root.mainloop()

    def _capture_images(self):
        context = zmq.Context()
        socket = context.socket(zmq.SUB)
        socket.connect(self.endpoint)
        socket.setsockopt_string(zmq.SUBSCRIBE, '')

        while True:
            try:
                message = socket.recv_multipart(flags=zmq.NOBLOCK)
                self._process_frame(message)
            except zmq.Again:
                time.sleep(0.001)  # Small delay to prevent busy waiting
            except Exception as e:
                logging.error(f"Error receiving message: {e}")

    def _process_frame(self, message):
        self._update_config()
        header = json.loads(message[0].decode('utf-8'))
        dtype = 'float32' if header['dtype'] == "float" else header['dtype']
        data = np.frombuffer(message[1], dtype=dtype).reshape((2304, 4096))
        data = cv2.resize(data, self.resize)
        data = np.clip(data, *self.clip)
        roi_data = data[self.roi[1]:self.roi[3], self.roi[0]:self.roi[2]]
        
        if self.colour != 'NONE':
            colored_data = cv2.applyColorMap((roi_data / 256).astype(np.uint8), self._get_colourmap())
        else:
            colored_data = cv2.normalize(roi_data, None, 0, 255, cv2.NORM_MINMAX, cv2.CV_8U)
        
        _, buffer = cv2.imencode('.jpg', colored_data)
        
        while not self.image_queue.empty():
            self.image_queue.get()
        self.image_queue.put((buffer, roi_data))
        
        self._histogram_pipe_child.send({
            'histogram_bins': self.histogram_bins,
            'histogram_range': self.histogram_range,
            'histogram_log_scale': self.histogram_log_scale
        })

    def _update_config(self):
        while self._config_pipe_child.poll():
            config = self._config_pipe_child.recv()
            self.resize = config.get('size', self.resize)
            self.colour = config.get('colour', self.colour)
            self.histogram_bins = config.get('histogram_bins', self.histogram_bins)
            self.histogram_range = config.get('histogram_range', self.histogram_range)
            self.histogram_log_scale = config.get('histogram_log_scale', self.histogram_log_scale)
            self.roi = config.get('roi', self.roi)
            self.clip = config.get('clip', self.clip)

    def _get_colourmap(self):
        return getattr(cv2, self.colour, cv2.COLORMAP_PLASMA) if self.colour != 'NONE' else None

    def _display_latest_image(self):
        root = tk.Tk()
        root.title("Live Image")

        image_label = tk.Label(root)
        image_label.pack(side=tk.LEFT)

        histogram_frame = tk.Frame(root)
        histogram_frame.pack(side=tk.RIGHT)

        fig = Figure(figsize=(5, 4), dpi=100)
        canvas = FigureCanvasTkAgg(fig, master=histogram_frame)
        canvas.get_tk_widget().pack()

        while True:
            if not self.image_queue.empty():
                frame, roi_data = self.image_queue.get()
                img = cv2.imdecode(frame, cv2.IMREAD_COLOR)
                img = cv2.cvtColor(img, cv2.COLOR_BGR2RGB)
                img_pil = Image.fromarray(img)
                img_tk = ImageTk.PhotoImage(img_pil)
                image_label.config(image=img_tk)
                image_label.image = img_tk

                histogram_config = self.histogram_pipe.recv()
                fig.clear()
                ax = fig.add_subplot(111)
                ax.hist(roi_data.flatten(), bins=histogram_config['histogram_bins'],
                        range=histogram_config['histogram_range'],
                        log=histogram_config['histogram_log_scale'])
                ax.set_title("Histogram")
                canvas.draw()

            root.update()
            time.sleep(0.2)  # Update interval

if __name__ == '__main__':
    logging.basicConfig(level=logging.DEBUG)
    viewer = LiveDataViewer('tcp://localhost:5020')
    viewer.start()
    try:
        # Keep the main thread alive
        while True:
            time.sleep(1)
    except KeyboardInterrupt:
        viewer.stop()
